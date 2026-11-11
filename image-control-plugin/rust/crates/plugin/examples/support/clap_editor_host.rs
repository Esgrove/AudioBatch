//! Dedicated Clack GUI host that loads only the caller's authorized packaged CLAP artifact.

#![expect(
    unsafe_code,
    reason = "the explicitly selected dynamic CLAP library is loaded once through Clack's owned entry"
)]

use std::ffi::CString;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

use anyhow::{Context as _, Result, ensure};
use clack_extensions::gui::{GuiSize, HostGui, HostGuiImpl};
use clack_host::entry::LibraryEntry;
use clack_host::prelude::*;

#[path = "artifacts.rs"]
mod artifacts;

/// Independent persisted identity, not obtained by querying an installed-plugin registry.
const PLUGIN_ID: &std::ffi::CStr = c"com.esgrove.imagecontrol-rs-own";

/// GUI-only host never activates an audio processor or opens physical audio devices.
pub struct EditorHost;

/// Thread-safe host notifications use atomics and never touch `AppKit` from arbitrary plugin threads.
#[derive(Default)]
pub struct Shared {
    /// Deferred CLAP main-thread work serviced between bounded native event turns.
    callback: AtomicBool,
    /// An unexpected restart is a failure in this inactive GUI-only lifecycle.
    restart: AtomicBool,
    /// The plugin may request processing for pending edits, without obligating this host to open hardware.
    process: AtomicBool,
    /// Connection loss or unexpected native closure must not masquerade as a successful lifecycle.
    closed: AtomicBool,
}

impl HostHandlers for EditorHost {
    type Shared<'a> = Shared;
    type MainThread<'a> = ();
    type AudioProcessor<'a> = ();

    /// Advertises the real GUI extension independently of the existing audio-contract host.
    fn declare_extensions(builder: &mut HostExtensions<Self>, _shared: &Self::Shared<'_>) {
        builder.register::<HostGui>();
    }
}

impl SharedHandler<'_> for Shared {
    /// Retains restart requests for failure reporting on the native owner thread.
    fn request_restart(&self) {
        self.restart.store(true, Ordering::Release);
    }

    /// Records processing requests without starting an unrelated audio callback.
    fn request_process(&self) {
        self.process.store(true, Ordering::Release);
    }

    /// Defers plugin main-thread work until no native GUI call is on this host's stack.
    fn request_callback(&self) {
        self.callback.store(true, Ordering::Release);
    }
}

impl HostGuiImpl for Shared {
    /// The smoke drives explicit host dimensions and does not cache optional resize hints.
    fn resize_hints_changed(&self) {}

    /// This host controls geometry explicitly, so plugin-originated requests are safely declined.
    fn request_resize(&self, _new_size: GuiSize) -> Result<(), HostError> {
        Err(HostError::Message("editor smoke controls parent geometry explicitly"))
    }

    /// Only the main-thread test sequence controls visibility.
    fn request_show(&self) -> Result<(), HostError> {
        Err(HostError::Message("editor smoke controls visibility explicitly"))
    }

    /// Only the main-thread test sequence controls visibility.
    fn request_hide(&self) -> Result<(), HostError> {
        Err(HostError::Message("editor smoke controls visibility explicitly"))
    }

    /// Reports unexpected connection loss without native operations in a thread-safe callback.
    fn closed(&self, _was_destroyed: bool) {
        self.closed.store(true, Ordering::Release);
    }
}

/// Main-thread-owned plugin instance, with library lifetime managed by Clack.
pub type Instance = PluginInstance<EditorHost>;

/// Requires an absolute authorized build-tree package and a canonical binary that cannot escape its bundle.
pub fn checked_bundle(path: &Path) -> Result<PathBuf> {
    let bundle = artifacts::checked_artifact(path).context("CLAP must be an explicit authorized build artifact")?;
    ensure!(
        bundle.is_dir() && bundle.extension().is_some_and(|extension| extension == "clap"),
        "expected a macOS .clap package directory"
    );
    let binary = artifacts::checked_artifact(&bundle.join("Contents/MacOS/ImageControl RS"))?;
    ensure!(
        binary.is_file() && binary.starts_with(&bundle),
        "CLAP binary is missing or escaped its package"
    );
    Ok(bundle)
}

/// Instantiates the actual packaged plugin through its dynamic factory, never an in-process Rust entry.
pub fn load(bundle: &Path) -> Result<Instance> {
    let bundle = checked_bundle(bundle)?;
    let binary = bundle.join("Contents/MacOS/ImageControl RS");
    let path = CString::new(bundle.to_str().context("CLAP package path must be UTF-8")?)?;
    // SAFETY: the caller explicitly selects this authorized build-tree native library.
    // Clack owns the loaded entry and keeps it live until every plugin instance has been retired.
    let entry = unsafe { PluginEntry::load_from(LibraryEntry::load_from_path(binary)?, &path)? };
    let factory = entry.get_plugin_factory().context("packaged CLAP plugin factory")?;
    let mut descriptors = factory.plugin_descriptors();
    let descriptor = descriptors.next().context("packaged CLAP descriptor")?;
    ensure!(
        descriptors.next().is_none(),
        "package must advertise exactly one plugin"
    );
    ensure!(descriptor.id() == Some(PLUGIN_ID), "wrong packaged CLAP identity");
    ensure!(
        descriptor.name() == Some(c"ImageControl RS"),
        "wrong packaged CLAP name"
    );
    ensure!(descriptor.vendor() == Some(c"Esgrove"), "wrong packaged CLAP vendor");
    let info = HostInfo::new(
        "ImageControl native CLAP editor smoke",
        "Esgrove",
        "https://esgrove.com",
        "1",
    )?;
    Ok(Instance::new(|()| Shared::default(), |_| (), &entry, PLUGIN_ID, &info)?)
}

/// Services one deferred callback per native turn, with the process watchdog bounding recursive plugin requests.
pub fn service_callbacks(instance: &mut Instance) -> Result<()> {
    if instance.access_shared_handler(|shared| shared.callback.swap(false, Ordering::AcqRel)) {
        instance.call_on_main_thread_callback();
    }
    instance.access_shared_handler(|shared| {
        ensure!(
            !shared.closed.load(Ordering::Acquire),
            "plugin unexpectedly closed its GUI"
        );
        ensure!(
            !shared.restart.load(Ordering::Acquire),
            "inactive editor unexpectedly requested a restart"
        );
        Ok(())
    })
}
