//! Loading VST3 modules and creating objects from their plugin factories.
//!
//! On macOS a `.vst3` bundle is loaded through `CFBundle`,
//! then `bundleEntry` is called and `GetPluginFactory` returns the factory.
//! On Windows the library is `Contents/<architecture>-win/<name>.vst3` inside the bundle,
//! or the `.vst3` file itself for the older single-file layout,
//! and `InitDll` is called before `GetPluginFactory` when the library exports it.
//! Loaded modules stay loaded for the life of the process,
//! because unloading some plugins in mid-session crashes them,
//! and because adding chain slots and starting runs would otherwise load the same bundle again and again.

#![expect(
    unsafe_code,
    reason = "calls the entry points of plugin libraries and their factories"
)]

use std::collections::HashMap;
use std::ffi::c_void;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock, PoisonError};

use anyhow::{Context as _, Result, anyhow, bail};
use vst3::Steinberg::{
    FIDString, FUnknown, IPluginFactory, IPluginFactory3, IPluginFactory3Trait as _, IPluginFactoryTrait as _,
    kResultOk,
};
use vst3::{ComPtr, Interface};

use crate::factory::{ClassId, ClassInfo, FactoryInfo, read_classes, read_factory_info};
use crate::host_objects::HostApplication;
use crate::thread::UiThread;

/// Signature of `GetPluginFactory`.
type GetFactoryFunction = unsafe extern "system" fn() -> *mut IPluginFactory;

/// Every module loaded from disk, by bundle path, kept loaded until the process exits.
static LOADED_MODULES: OnceLock<Mutex<HashMap<PathBuf, Arc<Module>>>> = OnceLock::new();

/// A loaded VST3 module and its plugin factory.
pub struct Module {
    /// Bundle path, or a descriptive name for a factory created in process.
    path: PathBuf,
    /// The module's plugin factory.
    factory: ComPtr<IPluginFactory>,
    /// What the factory reports about itself.
    factory_info: FactoryInfo,
    /// The classes the factory lists.
    classes: Vec<ClassInfo>,
    /// Keeps the library loaded, `None` for a factory created in process.
    _library: Option<platform::Library>,
}

impl Module {
    /// Loads the module at a bundle path, or returns it when it is already loaded.
    ///
    /// # Errors
    ///
    /// Returns an error when the bundle cannot be loaded or has no plugin factory.
    pub fn load(ui: &UiThread, path: &Path) -> Result<Arc<Self>> {
        let modules = LOADED_MODULES.get_or_init(Mutex::default);
        if let Some(module) = modules.lock().unwrap_or_else(PoisonError::into_inner).get(path) {
            return Ok(Arc::clone(module));
        }
        let (library, factory) = platform::load(path)?;
        let module = Arc::new(Self::with_factory(ui, path.to_path_buf(), factory, Some(library)));
        modules
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .insert(path.to_path_buf(), Arc::clone(&module));
        Ok(module)
    }

    /// Wraps a factory that lives in this process, such as the fake plugins of the tests.
    #[must_use]
    pub fn from_factory(ui: &UiThread, name: &str, factory: ComPtr<IPluginFactory>) -> Arc<Self> {
        Arc::new(Self::with_factory(ui, PathBuf::from(name), factory, None))
    }

    /// Returns the bundle path.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Returns what the factory reports about itself.
    #[must_use]
    pub const fn factory_info(&self) -> &FactoryInfo {
        &self.factory_info
    }

    /// Returns the classes the factory lists.
    #[must_use]
    pub fn classes(&self) -> &[ClassInfo] {
        &self.classes
    }

    /// Returns the description of a class.
    #[must_use]
    pub fn class(&self, id: ClassId) -> Option<&ClassInfo> {
        self.classes.iter().find(|class| class.id == id)
    }

    /// Creates an object of a class through the factory, asking for the interface `I`.
    ///
    /// # Errors
    ///
    /// Returns an error when the factory cannot create the class or the object lacks the interface.
    pub(crate) fn create<I: Interface>(&self, _ui: &UiThread, id: ClassId) -> Result<ComPtr<I>> {
        let class_id = id.to_tuid();
        let interface_id: FIDString = I::IID.as_ptr().cast();
        let mut object: *mut c_void = std::ptr::null_mut();
        // SAFETY: the factory pointer is valid while the module lives,
        // both ids point to 16-byte arrays that outlive the call, and `object` is a writable pointer slot.
        let result = unsafe {
            self.factory
                .createInstance(class_id.as_ptr(), interface_id, &raw mut object)
        };
        if result != kResultOk || object.is_null() {
            bail!("The plugin factory could not create the class {id}");
        }
        // SAFETY: on success the factory returned an owned reference to an object implementing `I`.
        unsafe { ComPtr::from_raw(object.cast::<I>()) }.ok_or_else(|| anyhow!("The plugin returned no object"))
    }

    /// Builds the module around its factory, giving the factory the host context and reading its classes.
    fn with_factory(
        _ui: &UiThread,
        path: PathBuf,
        factory: ComPtr<IPluginFactory>,
        library: Option<platform::Library>,
    ) -> Self {
        if let Some(factory3) = factory.cast::<IPluginFactory3>() {
            let host = HostApplication::create();
            if let Some(context) = host.to_com_ptr::<FUnknown>() {
                // SAFETY: both pointers are valid, and the factory takes its own reference to the context.
                let _ = unsafe { factory3.setHostContext(context.as_ptr()) };
            }
        }
        let factory_info = read_factory_info(&factory);
        let classes = read_classes(&factory);
        Self {
            path,
            factory,
            factory_info,
            classes,
            _library: library,
        }
    }
}

impl std::fmt::Debug for Module {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Module")
            .field("path", &self.path)
            .field("classes", &self.classes.len())
            .finish_non_exhaustive()
    }
}

/// Takes ownership of the factory a library's `GetPluginFactory` returns.
///
/// # Safety
///
/// `function` must be the `GetPluginFactory` export of a loaded VST3 library.
unsafe fn take_factory(function: GetFactoryFunction) -> Result<ComPtr<IPluginFactory>> {
    // SAFETY: the caller guarantees that the function is the library's factory export,
    // which returns an owned reference or null.
    let pointer = unsafe { function() };
    // SAFETY: a non-null result is a valid IPluginFactory whose reference the caller now owns.
    unsafe { ComPtr::from_raw(pointer) }.context("The plugin library returned no factory")
}

#[cfg(target_os = "macos")]
mod platform {
    use std::ffi::c_void;
    use std::path::Path;

    use anyhow::{Result, bail};
    use objc2_core_foundation::{CFBundle, CFRetained, CFString, CFURL};
    use vst3::ComPtr;
    use vst3::Steinberg::IPluginFactory;

    use super::{GetFactoryFunction, take_factory};

    /// Signature of `bundleEntry`.
    type BundleEntryFunction = unsafe extern "system" fn(*mut c_void) -> bool;

    /// A loaded bundle, kept only so the code stays mapped.
    pub(super) struct Library {
        /// The bundle, retained until the process exits.
        _bundle: CFRetained<CFBundle>,
    }

    // SAFETY: the bundle is never used after loading, it is only retained so the library stays loaded,
    // and retaining and releasing a CFBundle is thread safe.
    unsafe impl Send for Library {}

    // SAFETY: no method is ever called through a shared reference to the library.
    unsafe impl Sync for Library {}

    /// Loads a bundle and returns it with its factory.
    pub(super) fn load(path: &Path) -> Result<(Library, ComPtr<IPluginFactory>)> {
        let Some(url) = CFURL::from_directory_path(path) else {
            bail!("Invalid plugin bundle path: {}", path.display());
        };
        let Some(bundle) = CFBundle::new(None, Some(&url)) else {
            bail!("Could not open the plugin bundle {}", path.display());
        };
        // SAFETY: loading the executable of a valid bundle has no preconditions besides a valid bundle.
        if !unsafe { bundle.load_executable_and_return_error(std::ptr::null_mut()) } {
            bail!("Could not load the plugin library of {}", path.display());
        }
        let entry = function(&bundle, "bundleEntry").or_else(|| function(&bundle, "BundleEntry"));
        let Some(entry) = entry else {
            bail!("The plugin does not export bundleEntry");
        };
        let Some(get_factory) = function(&bundle, "GetPluginFactory") else {
            bail!("The plugin does not export GetPluginFactory");
        };
        // SAFETY: `bundleEntry` has the documented signature and takes the bundle reference it was loaded from.
        let entry = unsafe { std::mem::transmute::<*mut c_void, BundleEntryFunction>(entry) };
        let bundle_reference = CFRetained::as_ptr(&bundle).as_ptr().cast::<c_void>();
        // SAFETY: the bundle is loaded and retained, as `bundleEntry` expects.
        if !unsafe { entry(bundle_reference) } {
            bail!("The plugin's bundleEntry failed");
        }
        // SAFETY: `GetPluginFactory` has the documented signature.
        let get_factory = unsafe { std::mem::transmute::<*mut c_void, GetFactoryFunction>(get_factory) };
        // SAFETY: the function is the factory export of the loaded bundle.
        let factory = unsafe { take_factory(get_factory) }?;
        Ok((Library { _bundle: bundle }, factory))
    }

    /// Returns an exported function of a loaded bundle.
    fn function(bundle: &CFBundle, name: &str) -> Option<*mut c_void> {
        let name = CFString::from_str(name);
        let pointer = bundle.function_pointer_for_name(Some(&name));
        (!pointer.is_null()).then_some(pointer)
    }
}

#[cfg(windows)]
mod platform {
    use std::path::{Path, PathBuf};

    use anyhow::{Context as _, Result, bail};
    use vst3::ComPtr;
    use vst3::Steinberg::IPluginFactory;

    use super::{GetFactoryFunction, take_factory};

    /// Signature of `InitDll`.
    type InitFunction = unsafe extern "system" fn() -> bool;

    /// Architecture folders inside a bundle that hold a library this process can load, in order of preference.
    #[cfg(target_arch = "x86_64")]
    const ARCHITECTURE_FOLDERS: &[&str] = &["x86_64-win"];

    /// Architecture folders inside a bundle that hold a library this process can load, in order of preference.
    #[cfg(target_arch = "aarch64")]
    const ARCHITECTURE_FOLDERS: &[&str] = &["arm64-win", "arm64x-win"];

    /// Architecture folders inside a bundle that hold a library this process can load, in order of preference.
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    const ARCHITECTURE_FOLDERS: &[&str] = &[];

    /// A loaded library.
    pub(super) struct Library {
        /// The library, kept loaded until the process exits.
        _library: libloading::Library,
    }

    /// Loads the library of a bundle and returns it with its factory.
    pub(super) fn load(path: &Path) -> Result<(Library, ComPtr<IPluginFactory>)> {
        let library_path = library_path(path)?;
        // SAFETY: loading a plugin library runs its initializers, which is what hosting a plugin means.
        let library = unsafe { libloading::Library::new(&library_path) }
            .with_context(|| format!("Could not load the plugin library {}", library_path.display()))?;
        // SAFETY: `InitDll` has the documented signature when present.
        if let Ok(init) = unsafe { library.get::<InitFunction>(b"InitDll\0") } {
            // SAFETY: the library is loaded, and `InitDll` takes no arguments.
            if !unsafe { init() } {
                bail!("The plugin's InitDll failed");
            }
        }
        // SAFETY: `GetPluginFactory` has the documented signature.
        let get_factory = unsafe { library.get::<GetFactoryFunction>(b"GetPluginFactory\0") }
            .context("The plugin does not export GetPluginFactory")?;
        // SAFETY: the function is the factory export of the loaded library.
        let factory = unsafe { take_factory(*get_factory) }?;
        Ok((Library { _library: library }, factory))
    }

    /// Returns the library inside a bundle folder, or the path itself for a single-file plugin.
    fn library_path(path: &Path) -> Result<PathBuf> {
        if path.is_file() {
            return Ok(path.to_path_buf());
        }
        let name = path.file_name().context("Invalid plugin bundle path")?;
        for folder in ARCHITECTURE_FOLDERS {
            let candidate = path.join("Contents").join(folder).join(name);
            if candidate.is_file() {
                return Ok(candidate);
            }
        }
        bail!("The plugin bundle has no library for this platform: {}", path.display())
    }
}

#[cfg(not(any(target_os = "macos", windows)))]
mod platform {
    use std::path::Path;

    use anyhow::{Result, bail};
    use vst3::ComPtr;
    use vst3::Steinberg::IPluginFactory;

    /// Placeholder for platforms without plugin loading.
    pub(super) struct Library;

    /// Plugin loading is only supported on macOS and Windows.
    pub(super) fn load(_path: &Path) -> Result<(Library, ComPtr<IPluginFactory>)> {
        bail!("VST3 plugins are not supported on this platform")
    }
}

#[cfg(test)]
mod test_module_loading {
    use audiobatch_test_plugin::{Kind, VENDOR, create_bundle};

    use super::*;
    use crate::instance::{PluginInstance, ProcessMode};
    use crate::test_helpers::{class_id, on_ui};

    #[test]
    fn loads_a_bundle_and_lists_its_classes() {
        let folder = tempfile::tempdir().expect("temporary folder");
        let bundle = create_bundle(folder.path(), "ListClasses").expect("test plugin bundle");
        let summary = on_ui(move |ui| {
            Module::load(ui, &bundle).map(|module| {
                let effects: Vec<String> = module
                    .classes()
                    .iter()
                    .filter(|class| class.is_audio_effect())
                    .map(|class| class.name.clone())
                    .collect();
                (module.factory_info().vendor.clone(), effects, module.classes().len())
            })
        })
        .expect("bundle loads");
        let names: Vec<String> = Kind::ALL.iter().map(|kind| kind.name().to_owned()).collect();
        assert_eq!(summary, (VENDOR.to_owned(), names, Kind::ALL.len() + 2));
    }

    #[test]
    fn a_loaded_bundle_is_reused_and_creates_working_instances() {
        let folder = tempfile::tempdir().expect("temporary folder");
        let bundle = create_bundle(folder.path(), "Reuse").expect("test plugin bundle");
        let result = on_ui(move |ui| {
            let first = Module::load(ui, &bundle)?;
            let second = Module::load(ui, &bundle)?;
            let mut instance = PluginInstance::create(ui, &first, class_id(Kind::Delay), None)?;
            instance.configure_main_buses(ui, 2)?;
            instance.setup(ui, ProcessMode::Offline, 44_100.0, 128)?;
            instance.activate(ui)?;
            anyhow::Ok((Arc::ptr_eq(&first, &second), instance.latency_samples(ui)))
        })
        .expect("instance from the bundle");
        assert_eq!(result, (true, audiobatch_test_plugin::DELAY_LATENCY));
    }

    #[test]
    fn missing_and_invalid_bundles_fail_to_load() {
        let folder = tempfile::tempdir().expect("temporary folder");
        let missing = folder.path().join("Missing.vst3");
        let empty = folder.path().join("Empty.vst3");
        std::fs::create_dir_all(empty.join("Contents")).expect("empty bundle");
        let results = on_ui(move |ui| [Module::load(ui, &missing).is_err(), Module::load(ui, &empty).is_err()]);
        assert_eq!(results, [true, true]);
    }
}
