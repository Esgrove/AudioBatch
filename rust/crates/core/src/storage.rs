//! Resolves shared measurements and application-private storage without creating files or directories.
//! An explicit data root is absolute and never falls back to production storage after an error.

use std::ffi::OsStr;
use std::path::{Path, PathBuf};

/// Environment override for isolated application storage.
pub const DATA_ROOT_VARIABLE: &str = "AUDIOBATCH_DATA_ROOT";

/// Why an explicit storage root cannot safely be used.
#[derive(Debug, thiserror::Error)]
#[error("AUDIOBATCH_DATA_ROOT must name an absolute directory without NUL bytes: {}", root.display())]
pub struct StorageError {
    root: PathBuf,
}

/// Shared database and Rust-private storage locations captured once at startup.
#[derive(Debug, Clone)]
pub struct StoragePaths {
    database: PathBuf,
    data: PathBuf,
    logs: PathBuf,
}

impl StoragePaths {
    /// Resolves the environment override or preserves the platform's normal locations.
    ///
    /// # Errors
    /// Returns an error for an invalid explicit root, without resolving production paths.
    pub fn from_environment() -> Result<Self, StorageError> {
        Self::resolve(std::env::var_os(DATA_ROOT_VARIABLE).as_deref())
    }

    /// Resolves an explicit root without changing process-wide environment variables.
    ///
    /// # Errors
    /// Returns an error unless the root is an absolute directory path without NUL bytes.
    pub fn in_root(root: &Path) -> Result<Self, StorageError> {
        Self::resolve(Some(root.as_os_str()))
    }

    /// Returns the common analysis database path.
    #[must_use]
    pub fn database(&self) -> &Path {
        &self.database
    }

    /// Returns the Rust-specific settings, plugins, presets, and instance-lock directory.
    #[must_use]
    pub fn data(&self) -> &Path {
        &self.data
    }

    /// Returns the Rust-specific log directory.
    #[must_use]
    pub fn logs(&self) -> &Path {
        &self.logs
    }

    /// Resolves a supplied environment value, keeping validation independently testable.
    fn resolve(root: Option<&OsStr>) -> Result<Self, StorageError> {
        if let Some(root) = root {
            let root = Path::new(root);
            if !root.is_absolute() || root.as_os_str().as_encoded_bytes().contains(&0) || root.is_file() {
                return Err(StorageError {
                    root: root.to_path_buf(),
                });
            }
            let data = root.join("AudioBatch-rs");
            return Ok(Self {
                database: root.join("AudioBatch").join("analysis.db"),
                logs: data.join("logs"),
                data,
            });
        }
        let data = dirs::data_dir()
            .unwrap_or_else(std::env::temp_dir)
            .join("AudioBatch-rs");
        #[cfg(target_os = "macos")]
        let shared = dirs::home_dir().unwrap_or_else(std::env::temp_dir).join("Library");
        #[cfg(target_os = "windows")]
        let shared = dirs::config_dir().unwrap_or_else(std::env::temp_dir);
        #[cfg(not(any(target_os = "macos", target_os = "windows")))]
        let shared = dirs::data_dir().unwrap_or_else(std::env::temp_dir);
        #[cfg(target_os = "macos")]
        let logs = dirs::home_dir().map_or_else(|| data.join("logs"), |home| home.join("Library/Logs/AudioBatch-rs"));
        #[cfg(not(target_os = "macos"))]
        let logs = data.join("logs");
        Ok(Self {
            database: shared.join("AudioBatch/analysis.db"),
            data,
            logs,
        })
    }
}

#[cfg(test)]
mod test_storage {
    use super::*;

    /// Keeps the shared database and private application paths at their native locations when no root is supplied.
    #[test]
    fn absent_override_preserves_platform_locations() {
        let paths = StoragePaths::resolve(None).expect("platform paths");
        assert!(paths.database().ends_with("AudioBatch/analysis.db"));
        assert!(paths.data().ends_with("AudioBatch-rs"));
        #[cfg(target_os = "macos")]
        assert_eq!(
            paths.database(),
            dirs::home_dir().expect("home").join("Library/AudioBatch/analysis.db")
        );
    }
}
