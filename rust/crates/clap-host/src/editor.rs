//! CLAP GUI negotiation, embedded native windows, and plugin-owned floating editor fallback.

#![expect(
    unsafe_code,
    reason = "attaches the CLAP GUI to native parents with UI-thread-controlled lifetimes"
)]

use std::cell::{Cell, RefCell};
use std::ffi::CString;
use std::rc::{Rc, Weak};
use std::sync::Arc;
use std::sync::atomic::Ordering;

use clack_extensions::gui::{GuiApiType, GuiConfiguration, GuiError, GuiSize, PluginGui, Window};
use plugin_host_core::{EditorCallbacks, NativeWindow, WindowError, WindowSize};
use raw_window_handle::RawWindowHandle;
use thiserror::Error;

use crate::host::MAX_EDITOR_DIMENSION;
use crate::instance::InstanceCore;
use crate::{PluginInstance, UiThread};

pub use plugin_host_core::WindowPosition;

thread_local! {
    /// Weak editor registrations let the common plugin tick service resize/show/hide requests.
    static EDITORS: RefCell<Vec<Weak<EditorShared>>> = const { RefCell::new(Vec::new()) };
}

/// Typed failures to open and attach a native CLAP editor.
#[derive(Debug, Error)]
pub enum EditorError {
    /// The plugin exposes no GUI extension, and `AudioBatch` has no generic parameter editor.
    #[error("This CLAP plugin does not provide a native editor")]
    MissingEditor,
    /// Neither embedding nor a suitable plugin-owned floating window is available.
    #[error("The CLAP editor does not support a native window on this platform")]
    UnsupportedWindowType,
    /// A second editor of the same instance is forbidden by CLAP.
    #[error("The plugin editor is already open")]
    AlreadyOpen,
    /// Plugin GUI operation failed.
    #[error(transparent)]
    Gui(#[from] GuiError),
    /// Native window creation failed.
    #[error(transparent)]
    Window(#[from] WindowError),
    /// The plugin did not provide a bounded positive editor size.
    #[error("The CLAP plugin reported an invalid editor size")]
    InvalidSize,
}

/// Owns one UI-thread CLAP GUI and, only for embedded editors, its native top-level window.
pub struct EditorWindow {
    shared: Rc<EditorShared>,
    window: Option<Rc<NativeWindow>>,
}

/// Main-thread editor callbacks, weak native-window link, and retained plugin lifetime.
struct EditorShared {
    core: Arc<InstanceCore>,
    gui: PluginGui,
    window: RefCell<Weak<NativeWindow>>,
    lifecycle: GuiLifecycle,
    resizing_for_plugin: Cell<bool>,
    size: Cell<WindowSize>,
}

/// Tracks successful creation, visibility, deferred errors, and exactly-once destruction.
#[derive(Default)]
struct GuiLifecycle {
    created: Cell<bool>,
    closed: Cell<bool>,
    error: RefCell<Option<GuiError>>,
}

impl EditorWindow {
    /// Embeds the GUI when supported, otherwise opens a plugin-owned floating window.
    /// The plugin GUI is destroyed before the host window on every success and failure path.
    /// A supplied owner must remain alive until the editor closes.
    /// Floating editors control their own geometry and do not restore a host window position.
    ///
    /// # Errors
    /// Returns actionable missing-editor, unsupported API, native creation or CLAP GUI errors.
    pub fn open(
        ui: &UiThread,
        instance: &PluginInstance,
        title: &str,
        owner: Option<RawWindowHandle>,
        position: Option<WindowPosition>,
    ) -> Result<Self, EditorError> {
        if instance.core.shared.gui_open.swap(true, Ordering::AcqRel) {
            return Err(EditorError::AlreadyOpen);
        }
        instance.core.shared.gui_closed.store(false, Ordering::Release);
        instance.core.shared.gui_resize.store(0, Ordering::Release);
        instance.core.shared.gui_visibility.store(0, Ordering::Release);
        let result = Self::open_inner(ui, instance, title, owner, position);
        if result.is_err() {
            instance.core.shared.gui_open.store(false, Ordering::Release);
        }
        result
    }

    /// Returns whether the native window or the plugin requested closure.
    #[must_use]
    pub fn is_closed(&self) -> bool {
        self.shared.lifecycle.closed.get() || self.shared.core.shared.gui_closed.load(Ordering::Acquire)
    }

    /// Returns the embedded window position, or none for a plugin-owned floating editor.
    #[must_use]
    pub fn position(&self) -> Option<WindowPosition> {
        self.window.as_ref().and_then(|window| window.position())
    }

    /// Shows a hidden editor again unless the plugin has reported a terminal GUI closure.
    pub fn show(&self, _ui: &UiThread) {
        if !self.shared.service_closed() {
            self.shared.show();
        }
    }

    /// Returns a deferred CLAP resize/show/hide failure for presentation by the UI owner.
    #[must_use]
    pub fn take_error(&self) -> Option<GuiError> {
        self.shared.lifecycle.error.borrow_mut().take()
    }

    /// Creates the cleanup owner before any plugin GUI resources exist.
    fn open_inner(
        ui: &UiThread,
        instance: &PluginInstance,
        title: &str,
        owner: Option<RawWindowHandle>,
        position: Option<WindowPosition>,
    ) -> Result<Self, EditorError> {
        let core = Arc::clone(&instance.core);
        let gui = {
            let mut parts = core.parts.borrow_mut();
            parts
                .plugin
                .plugin_handle()
                .get_extension::<PluginGui>()
                .ok_or(EditorError::MissingEditor)?
        };
        let mut editor = Self {
            shared: Rc::new(EditorShared {
                core,
                gui,
                window: RefCell::new(Weak::new()),
                lifecycle: GuiLifecycle::default(),
                resizing_for_plugin: Cell::new(false),
                size: Cell::new(WindowSize {
                    width: 640,
                    height: 480,
                }),
            }),
            window: None,
        };
        #[cfg(target_os = "macos")]
        let api_type = GuiApiType::COCOA;
        #[cfg(windows)]
        let api_type = GuiApiType::WIN32;
        #[cfg(not(any(target_os = "macos", windows)))]
        return Err(EditorError::UnsupportedWindowType);
        let configuration = {
            let mut parts = editor.shared.core.parts.borrow_mut();
            let handle = parts.plugin.plugin_handle();
            let configuration = select_configuration(api_type, gui.get_preferred_api(&handle), |configuration| {
                gui.is_api_supported(&handle, configuration)
            })
            .ok_or(EditorError::UnsupportedWindowType)?;
            editor
                .shared
                .lifecycle
                .finish_create(gui.create(&handle, configuration))?;
            configuration
        };
        if !editor.shared.service_closed() {
            if configuration.is_floating {
                editor.configure_floating(title, owner);
            } else {
                editor.attach_embedded(ui, title, owner, position, api_type)?;
            }
            if !editor.shared.service_closed() {
                // End the plugin borrow before the cleanup owner can run on a failed show.
                let result = {
                    let mut parts = editor.shared.core.parts.borrow_mut();
                    gui.show(&parts.plugin.plugin_handle())
                };
                result?;
                if !editor.shared.service_closed()
                    && let Some(window) = &editor.window
                {
                    window.show();
                }
            }
        }
        EDITORS.with_borrow_mut(|editors| editors.push(Rc::downgrade(&editor.shared)));
        Ok(editor)
    }

    /// Creates and retains the native parent before attachment, including on failed attachment.
    fn attach_embedded(
        &mut self,
        ui: &UiThread,
        title: &str,
        owner: Option<RawWindowHandle>,
        position: Option<WindowPosition>,
        api_type: GuiApiType<'_>,
    ) -> Result<(), EditorError> {
        let size;
        let resizable;
        {
            let mut parts = self.shared.core.parts.borrow_mut();
            let handle = parts.plugin.plugin_handle();
            size = self
                .shared
                .gui
                .get_size(&handle)
                .filter(|size| valid_size(*size))
                .ok_or(EditorError::InvalidSize)?;
            resizable = self.shared.gui.can_resize(&handle);
        }
        let size = window_size(size);
        self.shared.size.set(size);
        let callbacks: Rc<dyn EditorCallbacks> = self.shared.clone();
        let window = Rc::new(NativeWindow::create(ui, &callbacks, title, size, resizable, owner)?);
        *self.shared.window.borrow_mut() = Rc::downgrade(&window);
        self.window = Some(Rc::clone(&window));
        {
            let mut parts = self.shared.core.parts.borrow_mut();
            let handle = parts.plugin.plugin_handle();
            if let Some(scale) = window.content_scale() {
                let _ = self.shared.gui.set_scale(&handle, f64::from(scale));
            }
            let parent = Window::from_generic_ptr(api_type, window.parent());
            // SAFETY: EditorWindow retains the native parent until after its Drop destroys the plugin GUI.
            unsafe {
                self.shared.gui.set_parent(&handle, parent)?;
            }
        }
        if let Some(position) = position {
            window.move_to(position);
        }
        Ok(())
    }

    /// Supplies optional floating-window hints without creating a host-owned native window.
    fn configure_floating(&self, title: &str, owner: Option<RawWindowHandle>) {
        let mut parts = self.shared.core.parts.borrow_mut();
        let handle = parts.plugin.plugin_handle();
        if let Some(owner) = owner.and_then(transient_owner) {
            // SAFETY: The supplied host owner outlives the editor, as required by open's ownership contract.
            // Cocoa's CLAP handle is an NSView, not an NSWindow, including for set_transient.
            unsafe {
                let _ = self.shared.gui.set_transient(&handle, owner);
            }
        }
        if let Ok(title) = CString::new(title) {
            self.shared.gui.suggest_title(&handle, &title);
        }
    }
}

impl Drop for EditorWindow {
    fn drop(&mut self) {
        self.shared.destroy();
    }
}

impl EditorShared {
    /// Destroys the CLAP GUI exactly once while its native parent still exists.
    fn destroy(&self) {
        if !self.lifecycle.begin_destroy() {
            return;
        }
        self.core.shared.gui_open.store(false, Ordering::Release);
        {
            let mut parts = self.core.parts.borrow_mut();
            self.gui.destroy(&parts.plugin.plugin_handle());
        }
        self.core.shared.gui_closed.store(false, Ordering::Release);
        self.core.shared.gui_resize.store(0, Ordering::Release);
        self.core.shared.gui_visibility.store(0, Ordering::Release);
    }

    /// Shows a hidden GUI and captures errors without unwinding through native event callbacks.
    fn show(&self) {
        if !self.lifecycle.created.get() {
            return;
        }
        let result = {
            let mut parts = self.core.parts.borrow_mut();
            self.gui.show(&parts.plugin.plugin_handle())
        };
        if self.lifecycle.finish_visibility(true, result)
            && !self.service_closed()
            && let Some(window) = self.window.borrow().upgrade()
        {
            window.show();
        }
    }

    /// Services atomic resize and visibility requests on the UI thread, never from a plugin callback.
    fn service(&self) {
        // Closure takes priority over queued show/resize work, since the GUI may no longer exist.
        if self.service_closed() || !self.lifecycle.created.get() {
            return;
        }
        let resize = self.core.shared.gui_resize.swap(0, Ordering::AcqRel);
        if resize != 0 {
            let size = GuiSize {
                width: (resize >> 32) as u32,
                height: resize as u32,
            };
            // Only embedded editors have a host window whose geometry the host can service.
            if valid_size(size)
                && let Some(window) = self.window.borrow().upgrade()
            {
                self.resizing_for_plugin.set(true);
                let size = window_size(size);
                self.size.set(size);
                window.set_content_size(size);
                self.resizing_for_plugin.set(false);
            }
        }
        match self.core.shared.gui_visibility.swap(0, Ordering::AcqRel) {
            1 => self.show(),
            2 => {
                let result = {
                    let mut parts = self.core.parts.borrow_mut();
                    self.gui.hide(&parts.plugin.plugin_handle())
                };
                if self.lifecycle.finish_visibility(false, result)
                    && let Some(window) = self.window.borrow().upgrade()
                {
                    window.hide();
                }
            }
            _ => {}
        }
        self.service_closed();
    }

    /// Treats closed notifications as terminal and acknowledges destruction before any further GUI calls.
    /// CLAP requires `gui.destroy` even when `closed(was_destroyed = true)` reports a lost GUI.
    /// A non-destroyed close also retires this editor, so both notifications share exactly-once cleanup.
    fn service_closed(&self) -> bool {
        if !self.lifecycle.created.get() || !self.core.shared.gui_closed.swap(false, Ordering::AcqRel) {
            return false;
        }
        self.destroy();
        if let Some(window) = self.window.borrow().upgrade() {
            window.hide();
        }
        true
    }
}

impl Drop for EditorShared {
    fn drop(&mut self) {
        self.destroy();
    }
}

impl EditorCallbacks for EditorShared {
    fn constrain(&self, size: WindowSize) -> WindowSize {
        if self.resizing_for_plugin.get() {
            return size;
        }
        if !self.lifecycle.created.get() {
            return self.size.get();
        }
        let Ok(mut parts) = self.core.parts.try_borrow_mut() else {
            return self.size.get();
        };
        let handle = parts.plugin.plugin_handle();
        let requested = GuiSize {
            width: u32::try_from(size.width).unwrap_or(1),
            height: u32::try_from(size.height).unwrap_or(1),
        };
        self.gui
            .adjust_size(&handle, requested)
            .filter(|size| valid_size(*size))
            .map_or_else(|| self.size.get(), window_size)
    }
    fn resized(&self, size: WindowSize) {
        if self.resizing_for_plugin.get() || !self.lifecycle.created.get() {
            return;
        }
        let Ok(mut parts) = self.core.parts.try_borrow_mut() else {
            return;
        };
        let size = GuiSize {
            width: u32::try_from(size.width).unwrap_or(1),
            height: u32::try_from(size.height).unwrap_or(1),
        };
        if !valid_size(size) {
            return;
        }
        match self.gui.set_size(&parts.plugin.plugin_handle(), size) {
            Ok(()) => self.size.set(window_size(size)),
            Err(error) => self.lifecycle.record_error(error),
        }
    }
    fn scale_changed(&self, factor: f32) {
        if self.lifecycle.created.get()
            && let Ok(mut parts) = self.core.parts.try_borrow_mut()
        {
            let _ = self.gui.set_scale(&parts.plugin.plugin_handle(), f64::from(factor));
        }
    }
    fn close_requested(&self) {
        self.lifecycle.closed.set(true);
        if self.lifecycle.created.get()
            && let Ok(mut parts) = self.core.parts.try_borrow_mut()
            && let Err(error) = self.gui.hide(&parts.plugin.plugin_handle())
        {
            self.lifecycle.record_error(error);
        }
    }
}

impl GuiLifecycle {
    /// Arms destruction only after create succeeds, never for a rejected creation attempt.
    fn finish_create(&self, result: Result<(), GuiError>) -> Result<(), GuiError> {
        result?;
        self.created.set(true);
        Ok(())
    }

    /// Claims destruction once, and makes a destroyed GUI terminal for subsequent show requests.
    fn begin_destroy(&self) -> bool {
        if !self.created.replace(false) {
            return false;
        }
        self.closed.set(true);
        true
    }

    /// Changes visibility only after a successful plugin call, preserving the actual deferred failure.
    fn finish_visibility(&self, visible: bool, result: Result<(), GuiError>) -> bool {
        match result {
            Ok(()) => {
                self.closed.set(!visible);
                true
            }
            Err(error) => {
                self.record_error(error);
                false
            }
        }
    }

    /// Retains the first unreported failure rather than replacing its useful error with a later one.
    fn record_error(&self, error: GuiError) {
        self.error.borrow_mut().get_or_insert(error);
    }
}

/// Applies editor requests from the common main-thread plugin service.
pub(crate) fn service_editors(_ui: &UiThread) {
    let editors = EDITORS.with_borrow(|editors| editors.iter().filter_map(Weak::upgrade).collect::<Vec<_>>());
    for editor in editors {
        editor.service();
    }
    EDITORS.with_borrow_mut(|editors| editors.retain(|editor| editor.strong_count() > 0));
}

/// Chooses native embedding first, then a compatible floating preference, native floating, or an unspecified API.
/// Foreign platform APIs are not suitable even when a plugin advertises them as preferred.
fn select_configuration<'a>(
    native_api: GuiApiType<'a>,
    preferred: Option<GuiConfiguration<'a>>,
    mut is_supported: impl FnMut(GuiConfiguration<'a>) -> bool,
) -> Option<GuiConfiguration<'a>> {
    let embedded = GuiConfiguration {
        api_type: native_api,
        is_floating: false,
    };
    if is_supported(embedded) {
        return Some(embedded);
    }
    if let Some(preferred) = preferred
        && preferred.is_floating
        && (preferred.api_type == native_api || preferred.api_type.0.is_empty())
        && is_supported(preferred)
    {
        return Some(preferred);
    }
    [native_api, GuiApiType(c"")]
        .into_iter()
        .map(|api_type| GuiConfiguration {
            api_type,
            is_floating: true,
        })
        .find(|configuration| is_supported(*configuration))
}

/// Converts only this platform's owner handle, preserving Cocoa's `NSView` and Win32's HWND distinction.
fn transient_owner(owner: RawWindowHandle) -> Option<Window<'static, 'static>> {
    match owner {
        #[cfg(target_os = "macos")]
        RawWindowHandle::AppKit(owner) => Some(Window::from_generic_ptr(GuiApiType::COCOA, owner.ns_view.as_ptr())),
        #[cfg(windows)]
        RawWindowHandle::Win32(owner) => Some(Window::from_generic_ptr(GuiApiType::WIN32, owner.hwnd.get() as *mut _)),
        _ => None,
    }
}

/// Validates plugin-controlled window dimensions before native conversion.
const fn valid_size(size: GuiSize) -> bool {
    size.width > 0 && size.height > 0 && size.width <= MAX_EDITOR_DIMENSION && size.height <= MAX_EDITOR_DIMENSION
}

/// Converts already validated CLAP dimensions into native signed coordinates.
fn window_size(size: GuiSize) -> WindowSize {
    WindowSize {
        width: i32::try_from(size.width).unwrap_or(640),
        height: i32::try_from(size.height).unwrap_or(480),
    }
}

#[cfg(test)]
mod editor_policy {
    #[cfg(windows)]
    use std::num::NonZeroIsize;
    #[cfg(target_os = "macos")]
    use std::ptr::NonNull;

    #[cfg(target_os = "macos")]
    use raw_window_handle::AppKitWindowHandle;
    #[cfg(windows)]
    use raw_window_handle::Win32WindowHandle;

    use super::*;

    #[test]
    fn embedding_wins_over_a_floating_plugin_preference() {
        for native_api in [GuiApiType::COCOA, GuiApiType::WIN32] {
            let preferred = GuiConfiguration {
                api_type: native_api,
                is_floating: true,
            };
            let mut queried = Vec::new();
            let selected = select_configuration(native_api, Some(preferred), |configuration| {
                queried.push(configuration);
                true
            })
            .expect("native embedded support");
            assert!(!selected.is_floating);
            assert_eq!(selected.api_type, native_api);
            assert_eq!(queried, [selected]);
        }
    }

    #[test]
    fn floating_requires_unsupported_embedding_and_supported_configuration() {
        let floating = GuiConfiguration {
            api_type: GuiApiType::COCOA,
            is_floating: true,
        };
        let mut queried = Vec::new();
        assert_eq!(
            select_configuration(GuiApiType::COCOA, Some(floating), |configuration| {
                queried.push(configuration);
                configuration == floating
            }),
            Some(floating)
        );
        assert!(!queried.first().expect("embedding checked first").is_floating);
        assert_eq!(queried.len(), 2);
        assert!(select_configuration(GuiApiType::COCOA, Some(floating), |_| false).is_none());
    }

    #[test]
    fn foreign_or_unsupported_preferences_do_not_prevent_native_floating_fallback() {
        for preferred_api in [GuiApiType::WIN32, GuiApiType(c"")] {
            let mut queried = Vec::new();
            let selected = select_configuration(
                GuiApiType::COCOA,
                Some(GuiConfiguration {
                    api_type: preferred_api,
                    is_floating: true,
                }),
                |configuration| {
                    queried.push(configuration);
                    configuration.is_floating && configuration.api_type == GuiApiType::COCOA
                },
            )
            .expect("native floating fallback");
            assert!(selected.is_floating);
            assert_eq!(selected.api_type, GuiApiType::COCOA);
            assert!(
                queried
                    .iter()
                    .all(|configuration| configuration.api_type != GuiApiType::WIN32)
            );
        }
    }

    #[test]
    fn unspecified_floating_api_is_supported_with_or_without_a_preference() {
        let unspecified = GuiConfiguration {
            api_type: GuiApiType(c""),
            is_floating: true,
        };
        for preferred in [None, Some(unspecified)] {
            assert_eq!(
                select_configuration(GuiApiType::WIN32, preferred, |configuration| configuration
                    == unspecified),
                Some(unspecified)
            );
        }
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn transient_cocoa_owner_is_the_nsview_not_an_nswindow() {
        // The conversion is pointer-only, no native code dereferences this sentinel.
        let view = NonNull::dangling();
        let owner = RawWindowHandle::AppKit(AppKitWindowHandle::new(view));
        let owner = transient_owner(owner).expect("Cocoa owner");
        assert_eq!(owner.api_type(), GuiApiType::COCOA);
        assert_eq!(owner.as_cocoa_nsview(), Some(view.as_ptr()));
    }

    #[cfg(windows)]
    #[test]
    fn transient_win32_owner_preserves_the_hwnd() {
        // The conversion is pointer-only, no native code dereferences this sentinel.
        let handle = NonZeroIsize::new(1).expect("sentinel handle");
        let owner = RawWindowHandle::Win32(Win32WindowHandle::new(handle));
        let owner = transient_owner(owner).expect("Win32 owner");
        assert_eq!(owner.api_type(), GuiApiType::WIN32);
        assert_eq!(owner.as_win32_hwnd(), Some(handle.get() as *mut _));
    }

    #[test]
    fn failed_create_never_arms_destroy_and_successful_create_destroys_once() {
        let lifecycle = GuiLifecycle::default();
        assert!(lifecycle.finish_create(Err(GuiError::CreateError)).is_err());
        assert!(!lifecycle.begin_destroy());
        lifecycle.finish_create(Ok(())).expect("successful create");
        assert!(lifecycle.created.get());
        assert!(lifecycle.begin_destroy());
        assert!(lifecycle.closed.get());
        assert!(!lifecycle.created.get());
        assert!(!lifecycle.begin_destroy());
    }

    #[test]
    fn visibility_failure_preserves_state_and_the_first_actual_error() {
        let lifecycle = GuiLifecycle::default();
        lifecycle.finish_create(Ok(())).expect("successful create");
        assert!(lifecycle.finish_visibility(false, Ok(())));
        assert!(lifecycle.closed.get());
        assert!(!lifecycle.finish_visibility(true, Err(GuiError::ShowError)));
        assert!(lifecycle.closed.get());
        assert!(!lifecycle.finish_visibility(false, Err(GuiError::HideError)));
        assert!(matches!(lifecycle.error.borrow_mut().take(), Some(GuiError::ShowError)));
        assert!(lifecycle.error.borrow_mut().take().is_none());
        assert!(lifecycle.finish_visibility(true, Ok(())));
        assert!(!lifecycle.closed.get());
        assert!(lifecycle.begin_destroy());
        assert!(!lifecycle.begin_destroy());
    }

    #[test]
    fn rollback_drops_the_plugin_borrow_before_the_cleanup_owner() {
        let lifecycle = GuiLifecycle::default();
        let parts = RefCell::new(());
        let cleanup_observed = Cell::new(false);
        let cleanup = || {
            assert!(parts.try_borrow_mut().is_ok(), "plugin borrow must end before destroy");
            assert!(lifecycle.begin_destroy(), "GUI must be destroyed once");
            cleanup_observed.set(true);
        };
        let result = (|| -> Result<(), GuiError> {
            let guard = CleanupOnDrop(Some(cleanup));
            {
                let _parts = parts.borrow_mut();
                lifecycle.finish_create(Ok(()))?;
                Err(GuiError::SetParentError)?;
            }
            drop(guard);
            Ok(())
        })();
        assert!(matches!(result, Err(GuiError::SetParentError)));
        assert!(cleanup_observed.get());
        assert!(!lifecycle.begin_destroy());
    }

    /// Runs a test cleanup at the same lexical ownership boundary used by `EditorWindow`.
    struct CleanupOnDrop<F: FnOnce()>(Option<F>);

    impl<F: FnOnce()> Drop for CleanupOnDrop<F> {
        fn drop(&mut self) {
            self.0.take().expect("one cleanup")();
        }
    }

    #[test]
    fn native_editor_message_does_not_offer_nonexistent_parameter_controls() {
        assert_eq!(
            EditorError::MissingEditor.to_string(),
            "This CLAP plugin does not provide a native editor"
        );
    }
}
