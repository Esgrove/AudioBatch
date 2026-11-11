//! The plugins found by scans, the bundles that crashed a scan, and the folders to scan.
//!
//! The list is saved as `plugins.json` in the data directory, replaced atomically,
//! so the GUI and the CLI never read a partly written file.
//! Its format is separate from the C++ application's `KnownPlugins.xml`, which identifies plugins by JUCE hashes.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use audiobatch_core::atomic_write::{AtomicWriteError, write_atomically};
use audiobatch_core::cache::data_directory;
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize, Serializer};
use vst3_host::ClassInfo;

use crate::identity::{PluginFormat, PluginId, PluginIdError};

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
const KNOWN_PLUGINS_FORMAT_VERSION: u32 = 2;

/// Name of the format version field in the JSON data files.
const VERSION_FIELD: &str = "version";

/// The VST3 format label retained for callers that specifically display VST3 plugins.
pub const FORMAT_NAME: &str = "VST3";

/// Failures while reading or writing saved plugin data.
#[derive(Debug, thiserror::Error)]
pub enum PluginDataError {
    /// The data file could not be read.
    #[error("Could not read the {description} {}", path.display())]
    Read {
        /// The saved data being read.
        description: &'static str,
        /// Location of the saved data.
        path: PathBuf,
        /// The file system failure.
        #[source]
        source: std::io::Error,
    },
    /// The contents are invalid or use an unsupported format.
    #[error("Invalid {description} {}", path.display())]
    Invalid {
        /// The saved data being decoded.
        description: &'static str,
        /// Location of the saved data.
        path: PathBuf,
        /// The malformed contents or unsupported version.
        #[source]
        source: VersionedJsonError,
    },
    /// An invalid file could not be preserved during recovery.
    #[error("The {description} {} could not be read or moved aside", path.display())]
    Recovery {
        /// The saved data being recovered.
        description: &'static str,
        /// Location of the invalid file.
        path: PathBuf,
        /// The failure to move the invalid file aside.
        #[source]
        source: std::io::Error,
    },
    /// Serialization failed before writing.
    #[error(transparent)]
    Serialize(#[from] serde_json::Error),
    /// Atomic replacement failed.
    #[error(transparent)]
    Write(#[from] AtomicWriteError),
}

/// Invalid JSON or a saved plugin data format newer than the reader supports.
#[derive(Debug, thiserror::Error)]
pub enum VersionedJsonError {
    /// JSON syntax or fields could not be decoded.
    #[error(transparent)]
    Json(#[from] serde_json::Error),
    /// The format is newer than the application supports.
    #[error("{} was saved by a newer version of AudioBatch, format {version} instead of {supported_version}", path.display())]
    NewerVersion {
        /// Location of the newer saved data.
        path: PathBuf,
        /// Format version found in the file.
        version: u64,
        /// Highest format version this reader supports.
        supported_version: u32,
    },
}

/// Loaded plugin data and an optional warning when malformed saved data was preserved separately.
#[derive(Debug)]
pub struct RecoveredData<T> {
    /// Loaded value, or the default after a recoverable parse failure.
    pub value: T,
    /// User-facing explanation of where malformed data was moved.
    pub warning: Option<String>,
}

/// One plugin class found in a bundle.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(try_from = "PluginDescriptionFields")]
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
    /// Format-qualified identity of the plugin's component.
    pub class_id: PluginId,
    /// Plugin API, defaulting to VST3 for descriptions saved without this field.
    #[serde(default)]
    pub format: PluginFormat,
    /// VST SDK version the plugin was built with.
    #[serde(default)]
    pub sdk_version: String,
    /// Path of the `.vst3` or `.clap` bundle.
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

/// Serialized description fields, with the format selecting the interpretation of the original id string.
#[derive(Serialize, Deserialize)]
struct PluginDescriptionFields {
    /// Display name.
    name: String,
    /// Vendor reported by the descriptor.
    #[serde(default)]
    vendor: String,
    /// Plugin version text.
    #[serde(default)]
    version: String,
    /// Categories separated by a pipe character.
    #[serde(default)]
    category: String,
    /// Original plugin id, kept compatible with legacy VST3 hex strings.
    class_id: String,
    /// Authoritative API namespace of the id.
    #[serde(default)]
    format: PluginFormat,
    /// VST SDK version, empty for other formats.
    #[serde(default)]
    sdk_version: String,
    /// Location of the plugin bundle.
    bundle: PathBuf,
    /// Last observed bundle modification time.
    #[serde(default)]
    bundle_modified_ms: i64,
    /// Whether the plugin generates audio rather than processing an input.
    #[serde(default)]
    is_instrument: bool,
    /// Main input channel count, when inspected.
    #[serde(default)]
    main_input_channels: Option<u32>,
    /// Main output channel count, when inspected.
    #[serde(default)]
    main_output_channels: Option<u32>,
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
    /// Creates a description from the original CLAP descriptor without altering its stable string id.
    #[must_use]
    pub fn from_clap_class(class: &clap_host::ClassInfo, bundle: &Path, bundle_modified_ms: i64) -> Self {
        Self {
            name: class.name.clone(),
            vendor: class.vendor.clone(),
            version: class.version.clone(),
            category: class.category.clone(),
            class_id: PluginId::Clap(class.id.clone()),
            format: PluginFormat::Clap,
            sdk_version: String::new(),
            bundle: bundle.to_path_buf(),
            bundle_modified_ms,
            is_instrument: class.features.iter().any(|feature| feature == "instrument"),
            main_input_channels: None,
            main_output_channels: None,
        }
    }

    /// Creates the description of a VST3 class in a bundle.
    #[must_use]
    pub fn from_class(class: &ClassInfo, bundle: &Path, bundle_modified_ms: i64) -> Self {
        Self {
            name: class.name.clone(),
            vendor: class.vendor.clone(),
            version: class.version.clone(),
            category: class.subcategories.clone(),
            class_id: class.id.into(),
            format: PluginFormat::Vst3,
            sdk_version: class.sdk_version.clone(),
            bundle: bundle.to_path_buf(),
            bundle_modified_ms,
            is_instrument: class.is_instrument(),
            main_input_channels: None,
            main_output_channels: None,
        }
    }
}

impl Serialize for PluginDescription {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        self.class_id
            .validate_format(self.format)
            .map_err(serde::ser::Error::custom)?;
        PluginDescriptionFields {
            name: self.name.clone(),
            vendor: self.vendor.clone(),
            version: self.version.clone(),
            category: self.category.clone(),
            class_id: self.class_id.to_string(),
            format: self.format,
            sdk_version: self.sdk_version.clone(),
            bundle: self.bundle.clone(),
            bundle_modified_ms: self.bundle_modified_ms,
            is_instrument: self.is_instrument,
            main_input_channels: self.main_input_channels,
            main_output_channels: self.main_output_channels,
        }
        .serialize(serializer)
    }
}

impl TryFrom<PluginDescriptionFields> for PluginDescription {
    type Error = PluginIdError;

    fn try_from(fields: PluginDescriptionFields) -> Result<Self, Self::Error> {
        Ok(Self {
            class_id: PluginId::from_format(fields.format, &fields.class_id)?,
            format: fields.format,
            name: fields.name,
            vendor: fields.vendor,
            version: fields.version,
            category: fields.category,
            sdk_version: fields.sdk_version,
            bundle: fields.bundle,
            bundle_modified_ms: fields.bundle_modified_ms,
            is_instrument: fields.is_instrument,
            main_input_channels: fields.main_input_channels,
            main_output_channels: fields.main_output_channels,
        })
    }
}

impl KnownPlugins {
    /// Reads the list from a file. A missing file gives an empty list.
    ///
    /// # Errors
    ///
    /// Returns an error when the file exists but cannot be read or parsed.
    pub fn load_from(path: &Path) -> Result<Self, PluginDataError> {
        match fs::read_to_string(path) {
            Ok(text) => read_versioned_json(&text, KNOWN_PLUGINS_FORMAT_VERSION, path).map_err(|source| {
                PluginDataError::Invalid {
                    description: "plugin list",
                    path: path.to_path_buf(),
                    source,
                }
            }),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(Self::default()),
            Err(source) => Err(PluginDataError::Read {
                description: "plugin list",
                path: path.to_path_buf(),
                source,
            }),
        }
    }

    /// Reads the list from a file, recovering from a file that cannot be parsed.
    /// Such a file is moved aside and an empty list is returned, so scans and folder edits keep working,
    /// and the returned message says where the file went.
    ///
    /// # Errors
    ///
    /// Returns an error when the file exists but cannot be read.
    pub fn load_recovering(path: &Path) -> Result<RecoveredData<Self>, PluginDataError> {
        load_recovering(path, Self::load_from, "plugin list")
    }

    /// Writes the list to a file, replacing it atomically and creating its directory when needed.
    ///
    /// # Errors
    ///
    /// Returns an error when the directory or the file cannot be written.
    pub fn save_to(&self, path: &Path) -> Result<(), PluginDataError> {
        write_versioned_json(path, KNOWN_PLUGINS_FORMAT_VERSION, self)
    }

    /// Returns the folders a scan searches: the custom folders, or the default ones when there are none.
    #[must_use]
    pub fn search_folders(&self) -> Vec<PathBuf> {
        if self.custom_folders.is_empty() {
            let mut folders = vst3_host::default_search_folders();
            folders.extend(clap_host::discovery::default_search_paths());
            let mut seen = std::collections::HashSet::new();
            folders.retain(|folder| seen.insert(folder.clone()));
            folders
        } else {
            self.custom_folders.clone()
        }
    }

    /// Returns the plugin with a format-qualified class id.
    #[must_use]
    pub fn find(&self, class_id: &PluginId) -> Option<&PluginDescription> {
        self.plugins.iter().find(|plugin| plugin.class_id == *class_id)
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
    load: impl Fn(&Path) -> Result<T, PluginDataError>,
    description: &'static str,
) -> Result<RecoveredData<T>, PluginDataError> {
    match load(path) {
        Ok(value) => Ok(RecoveredData { value, warning: None }),
        Err(
            error @ PluginDataError::Invalid {
                source: VersionedJsonError::Json(_),
                ..
            },
        ) => {
            let moved = unused_backup_path(path);
            let message = match fs::rename(path, &moved) {
                Ok(()) => format!(
                    "The {description} {} could not be read and was moved to {}: {}",
                    path.display(),
                    moved.display(),
                    error_chain(&error)
                ),
                Err(rename_error) => {
                    return Err(PluginDataError::Recovery {
                        description,
                        path: path.to_path_buf(),
                        source: rename_error,
                    });
                }
            };
            Ok(RecoveredData {
                value: T::default(),
                warning: Some(message),
            })
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
pub(crate) fn write_versioned_json(path: &Path, version: u32, value: &impl Serialize) -> Result<(), PluginDataError> {
    let versioned = Versioned { version, value };
    Ok(write_atomically(
        path,
        serde_json::to_string_pretty(&versioned)?.as_bytes(),
    )?)
}

/// Reads a value from JSON written by `write_versioned_json`.
/// A file without a version field is from before the field existed, and reads as version 1.
///
/// # Errors
///
/// Returns an error when the text cannot be parsed, or the file was written by a newer version of `AudioBatch`.
pub(crate) fn read_versioned_json<T: DeserializeOwned>(
    text: &str,
    supported_version: u32,
    path: &Path,
) -> Result<T, VersionedJsonError> {
    let json: serde_json::Value = serde_json::from_str(text)?;
    let version = json.get(VERSION_FIELD).and_then(serde_json::Value::as_u64).unwrap_or(1);
    if version > u64::from(supported_version) {
        return Err(VersionedJsonError::NewerVersion {
            path: path.to_path_buf(),
            version,
            supported_version,
        });
    }
    Ok(serde_json::from_value(json)?)
}

/// Formats a complete cause chain only at a warning or protocol boundary.
pub(crate) fn error_chain(error: &dyn std::error::Error) -> String {
    let mut message = error.to_string();
    let mut cause = error.source();
    while let Some(error) = cause {
        message.push_str(": ");
        message.push_str(&error.to_string());
        cause = error.source();
    }
    message
}

#[cfg(test)]
mod test_known_plugins {
    use std::error::Error as _;

    use super::*;

    /// Returns a description of a plugin in a bundle.
    fn description(name: &str, bundle: &str) -> PluginDescription {
        PluginDescription {
            name: name.to_owned(),
            vendor: "Vendor".to_owned(),
            version: "1.0".to_owned(),
            category: "Fx|Delay".to_owned(),
            class_id: vst3_host::ClassId([7; 16]).into(),
            format: PluginFormat::Vst3,
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
    fn clap_descriptors_preserve_original_identity_and_metadata() {
        let class = clap_host::ClassInfo {
            id: "07070707070707070707070707070707".to_owned(),
            name: "CLAP Delay".to_owned(),
            vendor: "Descriptor Vendor".to_owned(),
            version: "2.3.4".to_owned(),
            category: "delay".to_owned(),
            is_effect: true,
            features: vec!["audio-effect".to_owned(), "delay".to_owned()],
        };
        let bundle = Path::new("Effects.clap");
        let plugin = PluginDescription::from_clap_class(&class, bundle, 123);
        assert_eq!(plugin.class_id, PluginId::Clap(class.id.clone()));
        assert_eq!(plugin.format, PluginFormat::Clap);
        assert_eq!(plugin.name, class.name);
        assert_eq!(plugin.vendor, class.vendor);
        assert_eq!(plugin.version, class.version);
        assert_eq!(plugin.category, class.category);
        assert_eq!(plugin.bundle, bundle);
        assert_eq!(plugin.bundle_modified_ms, 123);
        assert_eq!(plugin.sdk_version, "");
        assert!(!plugin.is_instrument);
        assert_eq!(plugin.main_input_channels, None);
        assert_eq!(plugin.main_output_channels, None);
    }

    #[test]
    fn legacy_vst3_descriptions_default_the_format_and_optional_metadata() {
        let json = r#"{
            "plugins": [{
                "name": "Legacy", "class_id": "07070707070707070707070707070707", "bundle": "Legacy.vst3"
            }]
        }"#;
        let known: KnownPlugins = read_versioned_json(json, KNOWN_PLUGINS_FORMAT_VERSION, Path::new("plugins.json"))
            .expect("legacy plugin list");
        let plugin = known.plugins.first().expect("legacy plugin");
        assert_eq!(plugin.format, PluginFormat::Vst3);
        assert_eq!(plugin.class_id, vst3_host::ClassId([7; 16]).into());
        assert_eq!(plugin.vendor, "");
        assert_eq!(plugin.main_input_channels, None);
        let saved = serde_json::to_value(plugin).expect("description JSON");
        assert_eq!(saved["class_id"], "07070707070707070707070707070707");
        assert_eq!(saved["format"], "VST3");
    }

    #[test]
    fn clap_description_format_is_authoritative_even_for_hex_looking_ids() {
        let original = "07070707070707070707070707070707";
        let mut plugin = description("CLAP Effect", "Effect.clap");
        plugin.class_id = PluginId::Clap(original.to_owned());
        plugin.format = PluginFormat::Clap;
        let saved = serde_json::to_value(&plugin).expect("CLAP description JSON");
        assert_eq!(saved["class_id"], original);
        assert_eq!(saved["format"], "CLAP");
        assert_eq!(
            serde_json::from_value::<PluginDescription>(saved).expect("CLAP description"),
            plugin
        );
        assert_eq!(plugin.class_id.clap(), Some(original));
        assert_eq!(plugin.class_id.vst3(), None);
        let vst3 = description("VST3 Effect", "Effect.vst3");
        let known = KnownPlugins {
            plugins: vec![vst3.clone(), plugin.clone()],
            ..KnownPlugins::default()
        };
        assert_eq!(known.find(&plugin.class_id), Some(&plugin));
        assert_eq!(known.find(&vst3.class_id), Some(&vst3));
    }

    #[test]
    fn descriptions_reject_invalid_ids_and_inconsistent_in_memory_formats() {
        for json in [
            r#"{"name":"Invalid","bundle":"Effect.vst3","class_id":"org.example.effect"}"#,
            r#"{"name":"Invalid","bundle":"Effect.clap","class_id":"","format":"CLAP"}"#,
            r#"{"name":"Invalid","bundle":"Effect.clap","class_id":"id","format":"Unknown"}"#,
        ] {
            assert!(serde_json::from_str::<PluginDescription>(json).is_err(), "{json}");
        }
        let mut plugin = description("Invalid", "Effect.vst3");
        plugin.format = PluginFormat::Clap;
        assert!(serde_json::to_string(&plugin).is_err());
    }

    #[test]
    fn clap_lists_are_versioned_so_older_readers_reject_them_before_decoding() {
        let directory = tempfile::tempdir().expect("temporary folder");
        let path = directory.path().join("plugins.json");
        let mut plugin = description("CLAP Effect", "Effect.clap");
        plugin.class_id = PluginId::Clap("org.example.effect".to_owned());
        plugin.format = PluginFormat::Clap;
        let known = KnownPlugins {
            plugins: vec![plugin],
            ..KnownPlugins::default()
        };
        known.save_to(&path).expect("saved CLAP list");
        assert_eq!(KnownPlugins::load_from(&path).expect("loaded CLAP list"), known);
        let text = fs::read_to_string(&path).expect("plugin JSON");
        assert!(matches!(
            read_versioned_json::<KnownPlugins>(&text, 1, &path),
            Err(VersionedJsonError::NewerVersion {
                version: 2,
                supported_version: 1,
                ..
            })
        ));
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
        let error = KnownPlugins::load_from(&path).expect_err("invalid JSON");
        assert!(matches!(
            &error,
            PluginDataError::Invalid {
                source: VersionedJsonError::Json(_),
                ..
            }
        ));
        assert_eq!(error.to_string(), format!("Invalid plugin list {}", path.display()));
        assert!(
            error
                .source()
                .is_some_and(|source| source.downcast_ref::<VersionedJsonError>().is_some())
        );
        let RecoveredData {
            value: recovered,
            warning: message,
        } = KnownPlugins::load_recovering(&path).expect("recovers");
        assert_eq!(recovered, KnownPlugins::default());
        let message = message.expect("recovery warning");
        assert!(message.contains(&error_chain(&error)), "{message}");
        assert!(path.with_extension("invalid.json").is_file());
        assert!(!path.exists());
    }

    #[test]
    fn custom_folders_replace_the_defaults() {
        let mut known = KnownPlugins::default();
        let defaults = known.search_folders();
        for expected in vst3_host::default_search_folders()
            .into_iter()
            .chain(clap_host::discovery::default_search_paths())
        {
            assert!(defaults.contains(&expected));
        }
        assert_eq!(
            defaults.iter().collect::<std::collections::HashSet<_>>().len(),
            defaults.len()
        );
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
