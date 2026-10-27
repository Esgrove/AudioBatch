//! The `moduleinfo.json` file that recent VST3 bundles carry, read without loading the plugin.
//!
//! The file lists the factory vendor and every class with its category and subcategories,
//! so a scan can describe the plugin without running any of its code.
//! The format is JSON5, which allows comments and trailing commas.
//! A file that strict JSON cannot parse is treated as missing, and the scan loads the module instead.

use std::path::{Path, PathBuf};

use serde::Deserialize;

use crate::factory::{ClassId, ClassInfo};

/// The parts of `moduleinfo.json` that describe the module.
#[derive(Debug, Deserialize)]
struct ModuleInfoFile {
    /// What the factory reports about itself.
    #[serde(rename = "Factory Info", default)]
    factory_info: FactoryInfoEntry,
    /// The classes of the module.
    #[serde(rename = "Classes", default)]
    classes: Vec<ClassEntry>,
}

/// The factory description in `moduleinfo.json`.
#[derive(Debug, Default, Deserialize)]
struct FactoryInfoEntry {
    /// Vendor of the module.
    #[serde(rename = "Vendor", default)]
    vendor: String,
}

/// One class in `moduleinfo.json`.
#[derive(Debug, Deserialize)]
struct ClassEntry {
    /// Class id as 32 hexadecimal digits.
    #[serde(rename = "CID")]
    id: String,
    /// Class category.
    #[serde(rename = "Category", default)]
    category: String,
    /// Display name.
    #[serde(rename = "Name", default)]
    name: String,
    /// Class vendor.
    #[serde(rename = "Vendor", default)]
    vendor: String,
    /// Version text.
    #[serde(rename = "Version", default)]
    version: String,
    /// SDK version.
    #[serde(rename = "SDKVersion", default)]
    sdk_version: String,
    /// Subcategories.
    #[serde(rename = "Sub Categories", default)]
    subcategories: Vec<String>,
}

/// Returns the classes that a bundle's `moduleinfo.json` lists,
/// or `None` when the bundle has no such file or it cannot be read as strict JSON.
#[must_use]
pub fn read_module_info(bundle: &Path) -> Option<Vec<ClassInfo>> {
    let text = module_info_paths(bundle)
        .into_iter()
        .find_map(|path| std::fs::read_to_string(path).ok())?;
    parse_module_info(&text)
}

/// Parses the text of a `moduleinfo.json` file.
#[must_use]
pub(crate) fn parse_module_info(text: &str) -> Option<Vec<ClassInfo>> {
    let file: ModuleInfoFile = serde_json::from_str(text).ok()?;
    let factory_vendor = file.factory_info.vendor;
    file.classes
        .into_iter()
        .map(|entry| {
            let id: ClassId = entry.id.parse().ok()?;
            Some(ClassInfo {
                id,
                category: entry.category,
                name: entry.name,
                vendor: if entry.vendor.is_empty() {
                    factory_vendor.clone()
                } else {
                    entry.vendor
                },
                version: entry.version,
                subcategories: entry.subcategories.join("|"),
                sdk_version: entry.sdk_version,
            })
        })
        .collect()
}

/// Returns where a bundle may keep its `moduleinfo.json`, the current location first.
fn module_info_paths(bundle: &Path) -> [PathBuf; 2] {
    let contents = bundle.join("Contents");
    [
        contents.join("Resources").join("moduleinfo.json"),
        contents.join("moduleinfo.json"),
    ]
}

#[cfg(test)]
mod test_module_info {
    use super::*;

    const EXAMPLE: &str = r#"{
        "Name": "Example",
        "Version": "1.0.0",
        "Factory Info": { "Vendor": "Example Audio", "URL": "", "E-Mail": "" },
        "Classes": [
            {
                "CID": "6E33225254224A00AA69301AF318797D",
                "Category": "Audio Module Class",
                "Name": "Example Delay",
                "Vendor": "",
                "Version": "1.2.3",
                "SDKVersion": "VST 3.7.9",
                "Sub Categories": ["Fx", "Delay"],
                "Class Flags": 0,
                "Cardinality": 2147483647
            },
            {
                "CID": "1BA8A477EE0A4A2D80F50D1413D2EAA0",
                "Category": "Component Controller Class",
                "Name": "Example Delay",
                "Vendor": "Example Audio",
                "Version": "1.2.3",
                "SDKVersion": "VST 3.7.9"
            }
        ]
    }"#;

    #[test]
    fn reads_classes_with_the_factory_vendor_as_fallback() {
        let classes = parse_module_info(EXAMPLE).expect("valid module info");
        assert_eq!(classes.len(), 2);
        let effect = classes.first().expect("effect class");
        assert!(effect.is_audio_effect());
        assert_eq!(effect.name, "Example Delay");
        assert_eq!(effect.vendor, "Example Audio");
        assert_eq!(effect.subcategories, "Fx|Delay");
        assert_eq!(effect.sdk_version, "VST 3.7.9");
        assert_eq!(effect.id.to_string(), "6E33225254224A00AA69301AF318797D");
    }

    #[test]
    fn json5_comments_are_treated_as_missing() {
        let text = format!("// generated\n{EXAMPLE}");
        assert!(parse_module_info(&text).is_none());
    }

    #[test]
    fn an_invalid_class_id_rejects_the_file() {
        let text = EXAMPLE.replace("6E33225254224A00AA69301AF318797D", "not a class id");
        assert!(parse_module_info(&text).is_none());
    }

    #[test]
    fn reads_the_file_from_the_resources_folder() {
        let folder = tempfile::tempdir().expect("temporary folder");
        let bundle = folder.path().join("Example.vst3");
        let resources = bundle.join("Contents").join("Resources");
        std::fs::create_dir_all(&resources).expect("bundle folders");
        std::fs::write(resources.join("moduleinfo.json"), EXAMPLE).expect("module info");
        assert_eq!(read_module_info(&bundle).map(|classes| classes.len()), Some(2));
        assert!(read_module_info(folder.path()).is_none());
    }
}
