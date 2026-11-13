//! CLAP entry loading and owned plugin descriptors for isolated discovery workers.

#![expect(unsafe_code, reason = "loads native CLAP entries under the CLAP ABI contract")]

use std::collections::HashMap;
use std::ffi::CStr;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock, PoisonError};

use clack_host::entry::{PluginEntry, PluginEntryError};
use thiserror::Error;

use crate::UiThread;

/// Loaded disk-backed CLAP modules cached by path, retained for the process lifetime like VST3 modules.
static LOADED_MODULES: OnceLock<Mutex<HashMap<PathBuf, Arc<Module>>>> = OnceLock::new();

/// A failure to load a CLAP entry or read its plugin factory.
#[derive(Debug, Error)]
pub enum ModuleError {
    /// The entry could not be loaded or initialized.
    #[error("Could not load CLAP plugin {path}: {source}")]
    Load {
        /// Requested plugin path.
        path: PathBuf,
        /// Concrete Clack entry failure.
        #[source]
        source: PluginEntryError,
    },
    /// The entry does not advertise a plugin factory.
    #[error("The CLAP entry has no plugin factory")]
    MissingFactory,
    /// The factory supplied an invalid mandatory identity.
    #[error("CLAP descriptor {index} has no valid UTF-8 id or name")]
    InvalidDescriptor {
        /// Factory descriptor index.
        index: usize,
    },
}

/// One plugin class advertised by a CLAP entry.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClassInfo {
    /// Stable CLAP identifier, not a VST class id.
    pub id: String,
    /// Display name.
    pub name: String,
    /// Plugin vendor.
    pub vendor: String,
    /// Plugin version.
    pub version: String,
    /// Primary classification suitable for the plugin library.
    pub category: String,
    /// Whether the descriptor advertises an audio effect rather than an instrument.
    pub is_effect: bool,
    /// All advertised CLAP features in their original order.
    pub features: Vec<String>,
}

/// A loaded CLAP entry, cached by path for disk bundles to avoid repeated native loading and unloading.
pub struct Module {
    pub(crate) entry: PluginEntry,
    path: PathBuf,
    classes: Vec<ClassInfo>,
}

impl Module {
    /// Loads an explicitly selected CLAP entry on the UI thread, reusing an already loaded disk bundle.
    /// On macOS Clack resolves the bundle executable through Foundation's Info.plist-aware bundle API.
    ///
    /// # Errors
    /// Returns a concrete entry error or an invalid factory/descriptor error.
    pub fn load(ui: &UiThread, path: &Path) -> Result<Arc<Self>, ModuleError> {
        let modules = LOADED_MODULES.get_or_init(Mutex::default);
        if let Some(module) = modules.lock().unwrap_or_else(PoisonError::into_inner).get(path) {
            return Ok(Arc::clone(module));
        }
        // SAFETY: The caller selects native plugin code under the CLAP ABI contract.
        // Scanning callers isolate this inherently untrusted native execution in a worker process.
        let entry = unsafe { PluginEntry::load(path) }.map_err(|source| ModuleError::Load {
            path: path.to_path_buf(),
            source,
        })?;
        let module = Self::from_entry(ui, path, entry)?;
        modules
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .insert(path.to_path_buf(), Arc::clone(&module));
        Ok(module)
    }

    /// Wraps a Clack entry, including safe in-process test entries, and validates its descriptors.
    ///
    /// # Errors
    /// Returns an invalid factory or descriptor error.
    pub fn from_entry(_ui: &UiThread, path: impl AsRef<Path>, entry: PluginEntry) -> Result<Arc<Self>, ModuleError> {
        let path = path.as_ref();
        let factory = entry.get_plugin_factory().ok_or(ModuleError::MissingFactory)?;
        let mut classes = Vec::new();
        for (index, descriptor) in factory.plugin_descriptors().enumerate() {
            let id = descriptor.id().and_then(|value| value.to_str().ok());
            let name = descriptor.name().and_then(|value| value.to_str().ok());
            let (Some(id), Some(name)) = (id, name) else {
                return Err(ModuleError::InvalidDescriptor { index });
            };
            let features: Vec<_> = descriptor.features().map(text).collect();
            let is_effect = features.iter().any(|feature| feature == "audio-effect")
                && !features.iter().any(|feature| feature == "instrument");
            classes.push(ClassInfo {
                id: id.to_owned(),
                name: name.to_owned(),
                vendor: descriptor.vendor().map_or_else(String::new, text),
                version: descriptor.version().map_or_else(String::new, text),
                category: if is_effect { "Audio Module Class" } else { "Instrument" }.to_owned(),
                is_effect,
                features,
            });
        }
        Ok(Arc::new(Self {
            entry,
            path: path.to_path_buf(),
            classes,
        }))
    }

    /// Returns descriptors without instantiating any plugin.
    #[must_use]
    pub fn classes(&self, _ui: &UiThread) -> &[ClassInfo] {
        &self.classes
    }

    /// Returns the path selected for this module.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }
}

/// Converts optional presentation metadata lossily, without changing the strict stable id.
fn text(value: &CStr) -> String {
    value.to_string_lossy().into_owned()
}
