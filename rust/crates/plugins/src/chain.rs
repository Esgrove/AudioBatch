//! The saved plugin chain: which plugins process files, in which order, and with which settings.
//!
//! The chain is saved as `plugin-chain.json` in the data directory, replaced atomically.
//! Each slot keeps the plugin's description and its component and controller state,
//! stored as base64 text, so the settings come back with the chain on the next start.

use std::fs;
use std::path::Path;

use anyhow::{Context as _, Result};
use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as BASE64;
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use vst3_host::PluginState;

use crate::library::{PluginDescription, load_recovering, read_versioned_json, write_versioned_json};

/// Format version of `plugin-chain.json`, raised when a change cannot be read by older versions.
const CHAIN_FORMAT_VERSION: u32 = 1;

/// One plugin of the chain.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChainSlot {
    /// The plugin.
    pub plugin: PluginDescription,
    /// Whether the plugin processes files, so a slot can be bypassed without losing its settings.
    pub enabled: bool,
    /// Saved state of the plugin's audio component, empty for the plugin's defaults.
    #[serde(
        default,
        serialize_with = "serialize_base64",
        deserialize_with = "deserialize_base64"
    )]
    pub component_state: Vec<u8>,
    /// Saved state of the plugin's edit controller, empty for the plugin's defaults.
    #[serde(
        default,
        serialize_with = "serialize_base64",
        deserialize_with = "deserialize_base64"
    )]
    pub controller_state: Vec<u8>,
}

/// The plugins files are processed with, in order.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct PluginChain {
    /// The slots, in processing order. A damaged slot is skipped when the chain is read.
    #[serde(default, deserialize_with = "deserialize_slots_skipping_damaged")]
    pub slots: Vec<ChainSlot>,
}

impl ChainSlot {
    /// Creates an enabled slot with the plugin's default settings.
    #[must_use]
    pub const fn new(plugin: PluginDescription) -> Self {
        Self {
            plugin,
            enabled: true,
            component_state: Vec::new(),
            controller_state: Vec::new(),
        }
    }

    /// Returns the saved settings as a plugin state.
    #[must_use]
    pub fn state(&self) -> PluginState {
        PluginState {
            component: self.component_state.clone(),
            controller: self.controller_state.clone(),
        }
    }

    /// Replaces the saved settings.
    pub fn set_state(&mut self, state: PluginState) {
        self.component_state = state.component;
        self.controller_state = state.controller;
    }
}

impl PluginChain {
    /// Reads the chain from a file. A missing file gives an empty chain, and damaged slots are skipped.
    ///
    /// # Errors
    ///
    /// Returns an error when the file exists but cannot be read or parsed.
    pub fn load_from(path: &Path) -> Result<Self> {
        match fs::read_to_string(path) {
            Ok(text) => read_versioned_json(&text, CHAIN_FORMAT_VERSION, path)
                .with_context(|| format!("Invalid plugin chain {}", path.display())),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(Self::default()),
            Err(error) => Err(error).with_context(|| format!("Could not read the plugin chain {}", path.display())),
        }
    }

    /// Reads the chain from a file, recovering from a file that cannot be parsed.
    /// Such a file is moved aside, so saving the empty chain that replaces it cannot destroy it,
    /// and the returned message says where it went.
    ///
    /// # Errors
    ///
    /// Returns an error when the file exists but cannot be read.
    pub fn load_recovering(path: &Path) -> Result<(Self, Option<String>)> {
        load_recovering(path, Self::load_from, "plugin chain")
    }

    /// Writes the chain to a file, replacing it atomically and creating its directory when needed.
    ///
    /// # Errors
    ///
    /// Returns an error when the directory or the file cannot be written.
    pub fn save_to(&self, path: &Path) -> Result<()> {
        write_versioned_json(path, CHAIN_FORMAT_VERSION, self)
    }

    /// Returns the slots that process files, in order.
    pub fn enabled_slots(&self) -> impl Iterator<Item = &ChainSlot> {
        self.slots.iter().filter(|slot| slot.enabled)
    }

    /// Returns true when at least one slot processes files.
    #[must_use]
    pub fn has_enabled_slots(&self) -> bool {
        self.slots.iter().any(|slot| slot.enabled)
    }
}

/// Reads the slots of a chain or preset, leaving out every slot that cannot be read,
/// such as one with invalid base64 state, so one damaged slot does not lose the whole chain.
pub(crate) fn deserialize_slots_skipping_damaged<'de, D: Deserializer<'de>>(
    deserializer: D,
) -> Result<Vec<ChainSlot>, D::Error> {
    let values = Vec::<serde_json::Value>::deserialize(deserializer)?;
    Ok(values
        .into_iter()
        .filter_map(|value| serde_json::from_value(value).ok())
        .collect())
}

/// Writes bytes as base64 text.
fn serialize_base64<S: Serializer>(bytes: &[u8], serializer: S) -> Result<S::Ok, S::Error> {
    serializer.serialize_str(&BASE64.encode(bytes))
}

/// Reads bytes from base64 text.
fn deserialize_base64<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Vec<u8>, D::Error> {
    let text = String::deserialize(deserializer)?;
    BASE64.decode(text.as_bytes()).map_err(serde::de::Error::custom)
}

#[cfg(test)]
mod test_plugin_chain {
    use std::path::PathBuf;

    use tempfile::TempDir;
    use vst3_host::ClassId;

    use super::*;

    /// Returns a description of a plugin.
    fn description(name: &str) -> PluginDescription {
        PluginDescription {
            name: name.to_owned(),
            vendor: "Vendor".to_owned(),
            version: "1.0".to_owned(),
            category: "Fx".to_owned(),
            class_id: ClassId([3; 16]),
            sdk_version: "VST 3.8.0".to_owned(),
            bundle: PathBuf::from("/plugins/Effect.vst3"),
            bundle_modified_ms: 0,
            is_instrument: false,
            main_input_channels: Some(2),
            main_output_channels: Some(2),
        }
    }

    #[test]
    fn round_trips_through_a_file_with_binary_state() {
        let folder = TempDir::new().expect("temp dir");
        let path = folder.path().join("nested").join("plugin-chain.json");
        let mut first = ChainSlot::new(description("First"));
        first.set_state(PluginState {
            component: vec![0, 1, 2, 255, 128],
            controller: b"controller".to_vec(),
        });
        let mut second = ChainSlot::new(description("Second"));
        second.enabled = false;
        let chain = PluginChain {
            slots: vec![first, second],
        };
        chain.save_to(&path).expect("saved");
        assert_eq!(PluginChain::load_from(&path).expect("loaded"), chain);
    }

    #[test]
    fn stores_state_as_base64_text() {
        let mut slot = ChainSlot::new(description("Effect"));
        slot.component_state = vec![0, 255];
        let json = serde_json::to_value(&slot).expect("serialized");
        assert_eq!(json["component_state"], "AP8=");
        assert_eq!(json["controller_state"], "");
    }

    #[test]
    fn missing_file_is_an_empty_chain_and_a_damaged_slot_is_skipped() {
        let folder = TempDir::new().expect("temp dir");
        let path = folder.path().join("plugin-chain.json");
        assert_eq!(PluginChain::load_from(&path).expect("loaded"), PluginChain::default());
        let mut json = serde_json::to_value(PluginChain {
            slots: vec![
                ChainSlot::new(description("Damaged")),
                ChainSlot::new(description("Kept")),
            ],
        })
        .expect("serialized");
        json["slots"][0]["component_state"] = "not base64!".into();
        fs::write(&path, json.to_string()).expect("written");
        let loaded = PluginChain::load_from(&path).expect("loaded");
        let names: Vec<&str> = loaded.slots.iter().map(|slot| slot.plugin.name.as_str()).collect();
        assert_eq!(names, ["Kept"]);
    }

    #[test]
    fn a_file_that_cannot_be_parsed_is_moved_aside() {
        let folder = TempDir::new().expect("temp dir");
        let path = folder.path().join("plugin-chain.json");
        fs::write(&path, b"{ not json").expect("written");

        let (chain, message) = PluginChain::load_recovering(&path).expect("recovers");

        assert_eq!(chain, PluginChain::default());
        let moved = folder.path().join("plugin-chain.invalid.json");
        assert_eq!(fs::read(&moved).expect("moved aside"), b"{ not json");
        assert!(!path.exists());
        assert!(
            message.is_some_and(|message| message.contains("plugin-chain.invalid.json")),
            "the message names the moved file"
        );
    }

    #[test]
    fn only_enabled_slots_process_files() {
        let mut bypassed = ChainSlot::new(description("Bypassed"));
        bypassed.enabled = false;
        let mut chain = PluginChain {
            slots: vec![bypassed, ChainSlot::new(description("Active"))],
        };
        let names: Vec<&str> = chain.enabled_slots().map(|slot| slot.plugin.name.as_str()).collect();
        assert_eq!(names, ["Active"]);
        assert!(chain.has_enabled_slots());
        chain.slots.pop();
        assert!(!chain.has_enabled_slots());
    }

    #[test]
    fn saves_the_format_version_and_reads_files_without_one() {
        let folder = TempDir::new().expect("temp dir");
        let path = folder.path().join("plugin-chain.json");
        let chain = PluginChain {
            slots: vec![ChainSlot::new(description("Versioned"))],
        };
        chain.save_to(&path).expect("saved");
        let json: serde_json::Value = serde_json::from_str(&fs::read_to_string(&path).expect("read")).expect("json");
        assert_eq!(json["version"], CHAIN_FORMAT_VERSION);

        let unversioned = serde_json::json!({"slots": json["slots"]});
        fs::write(&path, unversioned.to_string()).expect("written");
        assert_eq!(PluginChain::load_from(&path).expect("loaded"), chain);
    }

    #[test]
    fn refuses_a_chain_saved_by_a_newer_version_without_moving_it() {
        let folder = TempDir::new().expect("temp dir");
        let path = folder.path().join("plugin-chain.json");
        let newer = serde_json::json!({"version": CHAIN_FORMAT_VERSION + 1, "slots": []});
        fs::write(&path, newer.to_string()).expect("written");
        let error = PluginChain::load_recovering(&path).expect_err("newer");
        assert!(format!("{error:#}").contains("newer version"), "{error:#}");
        assert!(path.exists());
    }
}
