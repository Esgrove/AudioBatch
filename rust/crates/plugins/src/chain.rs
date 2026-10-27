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

use crate::library::{PluginDescription, write_json_atomically};

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
    /// The slots, in processing order.
    #[serde(default)]
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
    /// Reads the chain from a file. A missing file gives an empty chain.
    ///
    /// # Errors
    ///
    /// Returns an error when the file exists but cannot be read or parsed.
    pub fn load_from(path: &Path) -> Result<Self> {
        match fs::read_to_string(path) {
            Ok(text) => serde_json::from_str(&text).with_context(|| format!("Invalid plugin chain {}", path.display())),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(Self::default()),
            Err(error) => Err(error).with_context(|| format!("Could not read the plugin chain {}", path.display())),
        }
    }

    /// Writes the chain to a file, replacing it atomically and creating its directory when needed.
    ///
    /// # Errors
    ///
    /// Returns an error when the directory or the file cannot be written.
    pub fn save_to(&self, path: &Path) -> Result<()> {
        write_json_atomically(path, self)
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
    fn missing_file_is_an_empty_chain_and_invalid_state_is_an_error() {
        let folder = TempDir::new().expect("temp dir");
        let path = folder.path().join("plugin-chain.json");
        assert_eq!(PluginChain::load_from(&path).expect("loaded"), PluginChain::default());
        let mut json = serde_json::to_value(PluginChain {
            slots: vec![ChainSlot::new(description("Effect"))],
        })
        .expect("serialized");
        json["slots"][0]["component_state"] = "not base64!".into();
        fs::write(&path, json.to_string()).expect("written");
        assert!(PluginChain::load_from(&path).is_err());
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
}
