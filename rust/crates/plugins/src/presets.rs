//! Plugin chain presets: named copies of a chain, with each plugin's settings, that can be loaded back later.
//!
//! Each preset is a JSON file named after the preset in the `chain-presets` folder of the data directory,
//! replaced atomically, so a failed save never leaves a partial preset behind.
//! A preset holds the same slots as the saved chain, so loading one restores the plugins,
//! their order, whether each one is enabled, and the component and controller state of each.
//! Its format is separate from the C++ application's XML presets, which identify plugins by JUCE descriptions.

use std::cmp::Ordering;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result, bail};
use audiobatch_core::atomic_write::write_atomically;
use audiobatch_core::sorting::compare_natural;
use serde::{Deserialize, Serialize};

use crate::chain::{ChainSlot, PluginChain, deserialize_slots_skipping_damaged};

/// Version of the preset file format, raised when a change would make older versions misread a file.
pub const PRESET_FORMAT_VERSION: u32 = 1;

/// File extension of the preset files.
const PRESET_EXTENSION: &str = "json";

/// Characters that are not allowed in a preset name, because some file system rejects them in a file name.
const ILLEGAL_NAME_CHARACTERS: &[char] = &['"', '#', '@', ',', ';', ':', '<', '>', '*', '^', '|', '?', '\\', '/'];

/// Longest preset name in characters, which keeps the file name within every platform's limit.
const MAXIMUM_NAME_CHARACTERS: usize = 128;

/// A saved plugin chain with its name.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChainPreset {
    /// Display name, which is also the file name without the extension.
    pub name: String,
    /// Format version of the file.
    pub version: u32,
    /// The slots, in processing order. A damaged slot is skipped when the preset is read.
    #[serde(default, deserialize_with = "deserialize_slots_skipping_damaged")]
    pub slots: Vec<ChainSlot>,
}

/// A preset file found in the preset folder.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PresetInfo {
    /// Name of the preset.
    pub name: String,
    /// Path of the preset file.
    pub path: PathBuf,
}

/// The folder of saved presets, and the operations on the presets in it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PresetStore {
    /// Folder of the preset files, created by the first save.
    folder: PathBuf,
}

impl ChainPreset {
    /// Creates a preset of a chain with the current format version.
    #[must_use]
    pub fn new(name: String, chain: &PluginChain) -> Self {
        Self {
            name,
            version: PRESET_FORMAT_VERSION,
            slots: chain.slots.clone(),
        }
    }

    /// Returns the chain of the preset.
    #[must_use]
    pub fn chain(&self) -> PluginChain {
        PluginChain {
            slots: self.slots.clone(),
        }
    }
}

impl PresetStore {
    /// Creates a store for the presets in a folder.
    #[must_use]
    pub const fn new(folder: PathBuf) -> Self {
        Self { folder }
    }

    /// Returns the folder of the preset files.
    #[must_use]
    pub fn folder(&self) -> &Path {
        &self.folder
    }

    /// Returns the path of the named preset, or `None` when the name has no usable characters.
    #[must_use]
    pub fn path_for(&self, name: &str) -> Option<PathBuf> {
        let name = sanitize_preset_name(name);
        (!name.is_empty()).then(|| self.folder.join(format!("{name}.{PRESET_EXTENSION}")))
    }

    /// Returns true when a preset with the name exists.
    #[must_use]
    pub fn contains(&self, name: &str) -> bool {
        self.path_for(name).is_some_and(|path| path.is_file())
    }

    /// Lists the presets in natural order by name, ignoring case.
    /// The files are not parsed, so a damaged preset is listed and reports its problem when it is loaded.
    ///
    /// # Errors
    ///
    /// Returns an error when the folder exists but cannot be read. A missing folder gives an empty list.
    pub fn list(&self) -> Result<Vec<PresetInfo>> {
        let entries = match fs::read_dir(&self.folder) {
            Ok(entries) => entries,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
            Err(error) => {
                return Err(error)
                    .with_context(|| format!("Could not read the preset folder {}", self.folder.display()));
            }
        };
        let mut presets: Vec<PresetInfo> = entries
            .filter_map(Result::ok)
            .map(|entry| entry.path())
            .filter(|path| path.is_file() && path.extension().is_some_and(|extension| extension == PRESET_EXTENSION))
            .filter_map(|path| {
                let name = path.file_stem()?.to_string_lossy().into_owned();
                Some(PresetInfo { name, path })
            })
            .collect();
        presets.sort_by(|first, second| compare_names(&first.name, &second.name));
        Ok(presets)
    }

    /// Saves a chain as the named preset, replacing a preset of the same name, and returns the preset file.
    ///
    /// # Errors
    ///
    /// Returns an error when the name has no usable characters, or the folder or the file cannot be written.
    pub fn save(&self, name: &str, chain: &PluginChain) -> Result<PathBuf> {
        let Some(path) = self.path_for(name) else {
            bail!("Enter a preset name");
        };
        let preset = ChainPreset::new(sanitize_preset_name(name), chain);
        write_atomically(&path, serde_json::to_string_pretty(&preset)?.as_bytes())
            .with_context(|| format!("Could not save the preset {}", path.display()))?;
        Ok(path)
    }

    /// Reads the named preset.
    ///
    /// # Errors
    ///
    /// Returns an error when the preset does not exist, cannot be parsed, or was saved by a newer version.
    pub fn load(&self, name: &str) -> Result<ChainPreset> {
        let Some(path) = self.path_for(name) else {
            bail!("Enter a preset name");
        };
        load_preset_file(&path)
    }

    /// Deletes the named preset. Returns true when a file was removed.
    ///
    /// # Errors
    ///
    /// Returns an error when the preset exists but cannot be deleted.
    pub fn delete(&self, name: &str) -> Result<bool> {
        let Some(path) = self.path_for(name) else {
            return Ok(false);
        };
        match fs::remove_file(&path) {
            Ok(()) => Ok(true),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
            Err(error) => Err(error).with_context(|| format!("Could not delete the preset {}", path.display())),
        }
    }
}

/// Turns a preset name into one that is valid as a file name on every platform.
///
/// Characters that file systems reject are removed, along with surrounding spaces and dots,
/// and the name is shortened to a length every platform accepts.
/// Returns an empty string when nothing usable is left.
#[must_use]
pub fn sanitize_preset_name(name: &str) -> String {
    let legal: String = name
        .chars()
        .filter(|character| !character.is_control() && !ILLEGAL_NAME_CHARACTERS.contains(character))
        .take(MAXIMUM_NAME_CHARACTERS)
        .collect();
    legal
        .trim_matches(|character: char| character == '.' || character.is_whitespace())
        .to_owned()
}

/// Reads a preset file.
///
/// # Errors
///
/// Returns an error when the file does not exist, cannot be parsed, or was saved by a newer version.
pub fn load_preset_file(path: &Path) -> Result<ChainPreset> {
    let text = match fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            bail!("The preset {} does not exist", path.display());
        }
        Err(error) => return Err(error).with_context(|| format!("Could not read the preset {}", path.display())),
    };
    let preset: ChainPreset =
        serde_json::from_str(&text).with_context(|| format!("{} is not a plugin chain preset", path.display()))?;
    if preset.version > PRESET_FORMAT_VERSION {
        bail!("{} was saved by a newer version of AudioBatch", path.display());
    }
    Ok(preset)
}

/// Orders preset names naturally, ignoring case, and by the exact name when they differ only in case.
fn compare_names(first: &str, second: &str) -> Ordering {
    compare_natural(&first.to_lowercase(), &second.to_lowercase()).then_with(|| first.cmp(second))
}

#[cfg(test)]
mod test_presets {
    use tempfile::TempDir;
    use vst3_host::{ClassId, PluginState};

    use super::*;
    use crate::library::PluginDescription;

    /// Returns a slot of a plugin with a component state and an enabled flag.
    fn slot(name: &str, component: &[u8], enabled: bool) -> ChainSlot {
        let mut slot = ChainSlot::new(PluginDescription {
            name: name.to_owned(),
            vendor: "Vendor".to_owned(),
            version: "1.0".to_owned(),
            category: "Fx".to_owned(),
            class_id: ClassId([5; 16]),
            sdk_version: "VST 3.8.0".to_owned(),
            bundle: PathBuf::from(format!("/plugins/{name}.vst3")),
            bundle_modified_ms: 0,
            is_instrument: false,
            main_input_channels: Some(2),
            main_output_channels: Some(2),
        });
        slot.set_state(PluginState {
            component: component.to_vec(),
            controller: b"controller".to_vec(),
        });
        slot.enabled = enabled;
        slot
    }

    /// Returns a store in a nested folder of the temporary directory, which the first save creates.
    fn store(directory: &TempDir) -> PresetStore {
        PresetStore::new(directory.path().join("data").join("chain-presets"))
    }

    #[test]
    fn names_are_sanitized_into_file_names() {
        assert_eq!(sanitize_preset_name("  Mastering  "), "Mastering");
        assert_eq!(sanitize_preset_name("Bus: A/B"), "Bus AB");
        assert_eq!(sanitize_preset_name(" .hidden. "), "hidden");
        assert_eq!(sanitize_preset_name(" ... "), "");
        assert_eq!(sanitize_preset_name("tab\tname\n"), "tabname");
        assert_eq!(
            sanitize_preset_name(&"x".repeat(300)).chars().count(),
            MAXIMUM_NAME_CHARACTERS
        );
        let presets = PresetStore::new(PathBuf::from("/presets"));
        assert_eq!(presets.path_for("  "), None);
        assert_eq!(presets.path_for("Master"), Some(PathBuf::from("/presets/Master.json")));
    }

    #[test]
    fn a_saved_preset_loads_back_with_its_order_flags_and_states() {
        let directory = TempDir::new().expect("temp dir");
        let presets = store(&directory);
        let chain = PluginChain {
            slots: vec![slot("Width", &[0, 1, 255], false), slot("Gain", &[], true)],
        };
        let path = presets.save(" Two Slots ", &chain).expect("saved");
        assert_eq!(path, presets.folder().join("Two Slots.json"));
        assert!(presets.contains("Two Slots"));

        let loaded = presets.load("Two Slots").expect("loaded");
        assert_eq!(loaded.name, "Two Slots");
        assert_eq!(loaded.version, PRESET_FORMAT_VERSION);
        assert_eq!(loaded.chain(), chain);
    }

    #[test]
    fn presets_are_listed_in_natural_order_and_replaced_by_name() {
        let directory = TempDir::new().expect("temp dir");
        let presets = store(&directory);
        assert_eq!(presets.list().expect("listed"), Vec::new());

        let first = PluginChain {
            slots: vec![slot("Gain", b"one", true)],
        };
        let second = PluginChain {
            slots: vec![slot("Gain", b"two", true), slot("Width", b"three", true)],
        };
        for name in ["preset 10", "preset 9", "alpha", "Beta"] {
            presets.save(name, &first).expect("saved");
        }
        fs::write(presets.folder().join("notes.txt"), "not a preset").expect("written");
        let names: Vec<String> = presets
            .list()
            .expect("listed")
            .into_iter()
            .map(|preset| preset.name)
            .collect();
        assert_eq!(names, ["alpha", "Beta", "preset 9", "preset 10"]);

        presets.save("alpha", &second).expect("replaced");
        assert_eq!(presets.list().expect("listed").len(), 4);
        assert_eq!(presets.load("alpha").expect("loaded").chain(), second);
    }

    #[test]
    fn deleting_removes_the_file_once() {
        let directory = TempDir::new().expect("temp dir");
        let presets = store(&directory);
        presets.save("Gone", &PluginChain::default()).expect("saved");
        assert!(presets.delete("Gone").expect("deleted"));
        assert!(!presets.delete("Gone").expect("already gone"));
        assert!(!presets.delete(" ").expect("no usable name"));
        assert!(!presets.contains("Gone"));
    }

    #[test]
    fn unusable_names_and_files_are_refused_with_a_reason() {
        let directory = TempDir::new().expect("temp dir");
        let presets = store(&directory);
        let error = presets
            .save(" / ", &PluginChain::default())
            .expect_err("no usable name");
        assert_eq!(error.to_string(), "Enter a preset name");
        let missing = presets.load("Missing").expect_err("missing");
        assert!(missing.to_string().contains("does not exist"), "{missing}");

        fs::create_dir_all(presets.folder()).expect("folder");
        fs::write(presets.folder().join("Broken.json"), "not json").expect("written");
        let broken = presets.load("Broken").expect_err("broken");
        assert!(broken.to_string().contains("is not a plugin chain preset"), "{broken}");

        let newer = serde_json::json!({"name": "Newer", "version": PRESET_FORMAT_VERSION + 1, "slots": []});
        fs::write(presets.folder().join("Newer.json"), newer.to_string()).expect("written");
        let newer = presets.load("Newer").expect_err("newer");
        assert!(newer.to_string().contains("newer version"), "{newer}");
    }
}
