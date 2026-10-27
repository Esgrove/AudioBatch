//! The plugins found by scans, the bundles that crashed a scan, and the folders to scan.
//!
//! The list is saved as `plugins.json` in the data directory, replaced atomically,
//! so the GUI and the CLI never read a partly written file.
//! Its format is separate from the C++ application's `KnownPlugins.xml`, which identifies plugins by JUCE hashes.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use anyhow::{Context as _, Result, bail};
use audiobatch_core::atomic_write::write_atomically;
use audiobatch_core::cache::data_directory;
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use vst3_host::{ClassId, ClassInfo, default_search_folders};

/// File name of the known plugins list in the data directory.
const KNOWN_PLUGINS_FILE_NAME: &str = "plugins.json";

/// File name of the lock that only one scan at a time can hold.
const SCAN_LOCK_FILE_NAME: &str = "plugin-scan.lock";

/// File name of the scan worker's log.
const WORKER_LOG_FILE_NAME: &str = "plugin-scan-worker.log";

/// File name of the saved plugin chain.
const CHAIN_FILE_NAME: &str = "plugin-chain.json";

/// Name of the folder of the saved chain presets.
const PRESETS_FOLDER_NAME: &str = "chain-presets";

/// Format version of `plugins.json`, raised when a change cannot be read by older versions.
const KNOWN_PLUGINS_FORMAT_VERSION: u32 = 1;

/// Name of the format version field in the JSON data files.
const VERSION_FIELD: &str = "version";

/// Format name shown for every plugin, since VST3 is the only supported format.
pub const FORMAT_NAME: &str = "VST3";

/// One plugin class found in a bundle.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PluginDescription {
    /// Display name.
    pub name: String,
    /// Vendor, or empty when the plugin reports none.
    #[serde(default)]
    pub vendor: String,
    /// Version text.
    #[serde(default)]
    pub version: String,
    /// Subcategories separated by "|", such as "Fx|Delay".
    #[serde(default)]
    pub category: String,
    /// Class id of the plugin's component.
    pub class_id: ClassId,
    /// VST SDK version the plugin was built with.
    #[serde(default)]
    pub sdk_version: String,
    /// Path of the `.vst3` bundle.
    pub bundle: PathBuf,
    /// Modification time of the bundle when it was scanned, in milliseconds since the Unix epoch.
    #[serde(default)]
    pub bundle_modified_ms: i64,
    /// Whether the plugin is an instrument rather than an effect.
    #[serde(default)]
    pub is_instrument: bool,
    /// Channel count of the main input bus, unknown when the scan did not load the plugin.
    #[serde(default)]
    pub main_input_channels: Option<u32>,
    /// Channel count of the main output bus, unknown when the scan did not load the plugin.
    #[serde(default)]
    pub main_output_channels: Option<u32>,
}

/// The known plugins and scan settings.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct KnownPlugins {
    /// Plugins found by scans.
    #[serde(default)]
    pub plugins: Vec<PluginDescription>,
    /// Bundles that crashed a scan, which later scans skip.
    #[serde(default)]
    pub blacklist: Vec<PathBuf>,
    /// Folders to scan instead of the default ones, when not empty.
    #[serde(default)]
    pub custom_folders: Vec<PathBuf>,
}

/// Where the plugin data files are.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PluginDataPaths {
    /// The known plugins list.
    pub known_plugins: PathBuf,
    /// The lock held during a scan.
    pub scan_lock: PathBuf,
    /// The log of the scan worker's stderr.
    pub worker_log: PathBuf,
    /// The plugin chain files are processed with.
    pub chain: PathBuf,
    /// The folder of the saved chain presets.
    pub presets: PathBuf,
}

/// A value written with its format version next to its own fields.
#[derive(Serialize)]
struct Versioned<'a, T> {
    /// Format version of the file.
    version: u32,
    /// The value, whose fields are written next to the version.
    #[serde(flatten)]
    value: &'a T,
}

impl PluginDescription {
    /// Creates the description of a class in a bundle.
    #[must_use]
    pub fn from_class(class: &ClassInfo, bundle: &Path, bundle_modified_ms: i64) -> Self {
        Self {
            name: class.name.clone(),
            vendor: class.vendor.clone(),
            version: class.version.clone(),
            category: class.subcategories.clone(),
            class_id: class.id,
            sdk_version: class.sdk_version.clone(),
            bundle: bundle.to_path_buf(),
            bundle_modified_ms,
            is_instrument: class.is_instrument(),
            main_input_channels: None,
            main_output_channels: None,
        }
    }
}

impl KnownPlugins {
    /// Reads the list from a file. A missing file gives an empty list.
    ///
    /// # Errors
    ///
    /// Returns an error when the file exists but cannot be read or parsed.
    pub fn load_from(path: &Path) -> Result<Self> {
        match fs::read_to_string(path) {
            Ok(text) => read_versioned_json(&text, KNOWN_PLUGINS_FORMAT_VERSION, path)
                .with_context(|| format!("Invalid plugin list {}", path.display())),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(Self::default()),
            Err(error) => Err(error).with_context(|| format!("Could not read the plugin list {}", path.display())),
        }
    }

    /// Reads the list from a file, recovering from a file that cannot be parsed.
    /// Such a file is moved aside and an empty list is returned, so scans and folder edits keep working,
    /// and the returned message says where the file went.
    ///
    /// # Errors
    ///
    /// Returns an error when the file exists but cannot be read.
    pub fn load_recovering(path: &Path) -> Result<(Self, Option<String>)> {
        load_recovering(path, Self::load_from, "plugin list")
    }

    /// Writes the list to a file, replacing it atomically and creating its directory when needed.
    ///
    /// # Errors
    ///
    /// Returns an error when the directory or the file cannot be written.
    pub fn save_to(&self, path: &Path) -> Result<()> {
        write_versioned_json(path, KNOWN_PLUGINS_FORMAT_VERSION, self)
    }

    /// Returns the folders a scan searches: the custom folders, or the default ones when there are none.
    #[must_use]
    pub fn search_folders(&self) -> Vec<PathBuf> {
        if self.custom_folders.is_empty() {
            default_search_folders()
        } else {
            self.custom_folders.clone()
        }
    }

    /// Returns the plugin with a class id.
    #[must_use]
    pub fn find(&self, class_id: ClassId) -> Option<&PluginDescription> {
        self.plugins.iter().find(|plugin| plugin.class_id == class_id)
    }

    /// Returns true when a bundle is blacklisted.
    #[must_use]
    pub(crate) fn is_blacklisted(&self, bundle: &Path) -> bool {
        self.blacklist.iter().any(|entry| entry == bundle)
    }

    /// Removes a bundle from the blacklist, so the next scan tries it again. Returns true when it was listed.
    pub fn remove_from_blacklist(&mut self, bundle: &Path) -> bool {
        let count = self.blacklist.len();
        self.blacklist.retain(|entry| entry != bundle);
        self.blacklist.len() != count
    }

    /// Sorts the plugins by bundle and name and the blacklist by path, so the saved file stays stable.
    pub fn sort(&mut self) {
        self.plugins.sort_by(|first, second| {
            first
                .bundle
                .cmp(&second.bundle)
                .then_with(|| first.name.cmp(&second.name))
        });
        self.blacklist.sort();
        self.blacklist.dedup();
    }
}

impl PluginDataPaths {
    /// Returns the paths of the files inside a folder.
    #[must_use]
    pub fn in_folder(folder: &Path) -> Self {
        Self {
            known_plugins: folder.join(KNOWN_PLUGINS_FILE_NAME),
            scan_lock: folder.join(SCAN_LOCK_FILE_NAME),
            worker_log: folder.join(WORKER_LOG_FILE_NAME),
            chain: folder.join(CHAIN_FILE_NAME),
            presets: folder.join(PRESETS_FOLDER_NAME),
        }
    }
}

impl Default for PluginDataPaths {
    /// Returns the paths in the application data directory.
    fn default() -> Self {
        Self::in_folder(&data_directory())
    }
}

/// Returns the modification time of a bundle in milliseconds since the Unix epoch, or 0 when it is unknown.
#[must_use]
pub fn bundle_modified_ms(bundle: &Path) -> i64 {
    fs::metadata(bundle)
        .and_then(|metadata| metadata.modified())
        .ok()
        .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
        .and_then(|duration| i64::try_from(duration.as_millis()).ok())
        .unwrap_or_default()
}

/// Loads a JSON data file with `load`, and when it cannot be parsed,
/// moves it aside as `<name>.invalid.json` and returns the default value with a message that says so.
/// A file that cannot be read at all is still an error, since moving it would not help.
pub(crate) fn load_recovering<T: Default>(
    path: &Path,
    load: impl Fn(&Path) -> Result<T>,
    description: &str,
) -> Result<(T, Option<String>)> {
    match load(path) {
        Ok(value) => Ok((value, None)),
        Err(error) if error.chain().any(<dyn std::error::Error>::is::<serde_json::Error>) => {
            let moved = unused_backup_path(path);
            let message = match fs::rename(path, &moved) {
                Ok(()) => format!(
                    "The {description} {} could not be read and was moved to {}: {error:#}",
                    path.display(),
                    moved.display()
                ),
                Err(rename_error) => {
                    return Err(rename_error).with_context(|| {
                        format!("The {description} {} could not be read or moved aside", path.display())
                    });
                }
            };
            Ok((T::default(), Some(message)))
        }
        Err(error) => Err(error),
    }
}

/// Returns `<name>.invalid.json` next to the file, or `<name>.invalid-2.json` and so on when that exists,
/// so moving a second damaged file aside keeps the first one.
fn unused_backup_path(path: &Path) -> PathBuf {
    let first = path.with_extension("invalid.json");
    if !first.exists() {
        return first;
    }
    (2..=u32::MAX)
        .map(|number| path.with_extension(format!("invalid-{number}.json")))
        .find(|candidate| !candidate.exists())
        .unwrap_or(first)
}

/// Writes a value as pretty JSON with a format version field next to its own fields,
/// replacing the file atomically and creating its directory when needed, so a reader never sees a partly written file.
///
/// # Errors
///
/// Returns an error when the directory or the file cannot be written.
pub(crate) fn write_versioned_json(path: &Path, version: u32, value: &impl Serialize) -> Result<()> {
    let versioned = Versioned { version, value };
    write_atomically(path, serde_json::to_string_pretty(&versioned)?.as_bytes())
}

/// Reads a value from JSON written by `write_versioned_json`.
/// A file without a version field is from before the field existed, and reads as version 1.
///
/// # Errors
///
/// Returns an error when the text cannot be parsed, or the file was written by a newer version of `AudioBatch`.
pub(crate) fn read_versioned_json<T: DeserializeOwned>(text: &str, supported_version: u32, path: &Path) -> Result<T> {
    let json: serde_json::Value = serde_json::from_str(text)?;
    let version = json.get(VERSION_FIELD).and_then(serde_json::Value::as_u64).unwrap_or(1);
    if version > u64::from(supported_version) {
        bail!(
            "{} was saved by a newer version of AudioBatch, format {version} instead of {supported_version}",
            path.display()
        );
    }
    Ok(serde_json::from_value(json)?)
}

#[cfg(test)]
mod test_known_plugins {
    use super::*;

    /// Returns a description of a plugin in a bundle.
    fn description(name: &str, bundle: &str) -> PluginDescription {
        PluginDescription {
            name: name.to_owned(),
            vendor: "Vendor".to_owned(),
            version: "1.0".to_owned(),
            category: "Fx|Delay".to_owned(),
            class_id: ClassId([7; 16]),
            sdk_version: "VST 3.8.0".to_owned(),
            bundle: PathBuf::from(bundle),
            bundle_modified_ms: 1_700_000_000_000,
            is_instrument: false,
            main_input_channels: Some(2),
            main_output_channels: None,
        }
    }

    #[test]
    fn saved_list_loads_back() {
        let folder = tempfile::tempdir().expect("temporary folder");
        let path = folder.path().join("nested").join(KNOWN_PLUGINS_FILE_NAME);
        let known = KnownPlugins {
            plugins: vec![description("Delay", "/plugins/Delay.vst3")],
            blacklist: vec![PathBuf::from("/plugins/Crash.vst3")],
            custom_folders: vec![PathBuf::from("/plugins")],
        };
        known.save_to(&path).expect("saved");
        assert_eq!(KnownPlugins::load_from(&path).expect("loaded"), known);
    }

    #[test]
    fn missing_file_is_empty_and_invalid_file_is_an_error() {
        let folder = tempfile::tempdir().expect("temporary folder");
        let path = folder.path().join(KNOWN_PLUGINS_FILE_NAME);
        assert_eq!(
            KnownPlugins::load_from(&path).expect("missing file"),
            KnownPlugins::default()
        );
        fs::write(&path, "{ not json").expect("invalid file");
        assert!(KnownPlugins::load_from(&path).is_err());
        let (recovered, message) = KnownPlugins::load_recovering(&path).expect("recovers");
        assert_eq!(recovered, KnownPlugins::default());
        assert!(message.is_some());
        assert!(path.with_extension("invalid.json").is_file());
        assert!(!path.exists());
    }

    #[test]
    fn custom_folders_replace_the_defaults() {
        let mut known = KnownPlugins::default();
        assert_eq!(known.search_folders(), default_search_folders());
        known.custom_folders = vec![PathBuf::from("/custom")];
        assert_eq!(known.search_folders(), vec![PathBuf::from("/custom")]);
    }

    #[test]
    fn blacklist_entries_can_be_removed() {
        let mut known = KnownPlugins {
            blacklist: vec![
                PathBuf::from("/b.vst3"),
                PathBuf::from("/a.vst3"),
                PathBuf::from("/b.vst3"),
            ],
            ..KnownPlugins::default()
        };
        known.sort();
        assert_eq!(
            known.blacklist,
            vec![PathBuf::from("/a.vst3"), PathBuf::from("/b.vst3")]
        );
        assert!(known.is_blacklisted(Path::new("/a.vst3")));
        assert!(known.remove_from_blacklist(Path::new("/a.vst3")));
        assert!(!known.remove_from_blacklist(Path::new("/a.vst3")));
        assert!(!known.is_blacklisted(Path::new("/a.vst3")));
    }
}
