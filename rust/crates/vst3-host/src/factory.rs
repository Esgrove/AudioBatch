//! Plugin class identifiers and the class descriptions a plugin factory lists.
//!
//! `ClassId` formats a class id the way the VST SDK and `moduleinfo.json` do:
//! 32 hexadecimal digits that read the same on every platform,
//! which is the COM byte order on Windows and the plain byte order elsewhere.
//! `ClassInfo` is what a factory reports about one class, from the richest interface it implements.

#![expect(
    unsafe_code,
    reason = "reads class descriptions through the plugin factory COM interfaces"
)]

use std::fmt;
use std::str::FromStr;

use serde::{Deserialize, Serialize};
use thiserror::Error;
use vst3::ComPtr;
use vst3::Steinberg::{
    IPluginFactory, IPluginFactory2, IPluginFactory2Trait as _, IPluginFactory3, IPluginFactory3Trait as _,
    IPluginFactoryTrait as _, PClassInfo, PClassInfo2, PClassInfoW, PFactoryInfo, TUID, kResultOk,
};

use crate::strings::{from_char8, from_utf16};

/// Category of the classes that process audio.
pub const AUDIO_EFFECT_CATEGORY: &str = "Audio Module Class";

/// Category of edit controller classes.
pub const CONTROLLER_CATEGORY: &str = "Component Controller Class";

/// Why a class identifier could not be parsed.
#[derive(Debug, Clone, PartialEq, Eq, Error)]
pub enum ClassIdError {
    /// The identifier does not contain exactly 32 ASCII digits.
    #[error("Invalid class id: \"{text}\"")]
    InvalidFormat {
        /// The unmodified input.
        text: String,
    },
    /// A pair of digits is not hexadecimal.
    #[error("Invalid class id: \"{text}\"")]
    InvalidHex {
        /// The unmodified input.
        text: String,
        /// The hexadecimal parsing failure.
        #[source]
        source: std::num::ParseIntError,
    },
}

/// Identifier of a plugin class, a 16-byte VST3 TUID.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(into = "String", try_from = "String")]
pub struct ClassId(pub [u8; 16]);

/// What a factory reports about one plugin class.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClassInfo {
    /// Class identifier.
    pub id: ClassId,
    /// Class category, such as `AUDIO_EFFECT_CATEGORY`.
    pub category: String,
    /// Display name.
    pub name: String,
    /// Vendor, the class vendor when it has one, otherwise the factory vendor.
    pub vendor: String,
    /// Version text.
    pub version: String,
    /// Subcategories separated by "|", such as "Fx|Delay".
    pub subcategories: String,
    /// SDK version the class was built with, such as "VST 3.7.9".
    pub sdk_version: String,
}

/// What a factory reports about itself.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FactoryInfo {
    /// Vendor of the module.
    pub vendor: String,
    /// Vendor website.
    pub url: String,
}

impl ClassId {
    /// Returns the class id of a raw TUID.
    #[must_use]
    pub fn from_tuid(tuid: &TUID) -> Self {
        Self(tuid.map(|byte| byte as u8))
    }

    /// Returns the raw TUID of the class id.
    #[must_use]
    pub(crate) fn to_tuid(self) -> TUID {
        self.0.map(|byte| std::ffi::c_char::from_ne_bytes([byte]))
    }

    /// Returns the bytes in the order they are formatted, which differs from the stored order on Windows.
    fn display_order(self) -> [u8; 16] {
        if cfg!(windows) {
            let bytes = self.0;
            let mut ordered = bytes;
            for (target, source) in [3, 2, 1, 0, 5, 4, 7, 6].into_iter().enumerate() {
                if let (Some(slot), Some(byte)) = (ordered.get_mut(target), bytes.get(source)) {
                    *slot = *byte;
                }
            }
            ordered
        } else {
            self.0
        }
    }
}

impl fmt::Display for ClassId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        for byte in self.display_order() {
            write!(formatter, "{byte:02X}")?;
        }
        Ok(())
    }
}

impl fmt::Debug for ClassId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "ClassId({self})")
    }
}

impl FromStr for ClassId {
    type Err = ClassIdError;

    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let digits = text.trim();
        if digits.len() != 32 || !digits.is_ascii() {
            return Err(ClassIdError::InvalidFormat { text: text.to_owned() });
        }
        let mut displayed = [0_u8; 16];
        for (index, slot) in displayed.iter_mut().enumerate() {
            let pair = digits
                .get(index * 2..index * 2 + 2)
                .ok_or_else(|| ClassIdError::InvalidFormat { text: text.to_owned() })?;
            *slot = u8::from_str_radix(pair, 16).map_err(|source| ClassIdError::InvalidHex {
                text: text.to_owned(),
                source,
            })?;
        }
        // The display reordering swaps bytes within groups, so applying it again restores the stored order.
        Ok(Self(Self(displayed).display_order()))
    }
}

impl From<ClassId> for String {
    fn from(id: ClassId) -> Self {
        id.to_string()
    }
}

impl TryFrom<String> for ClassId {
    type Error = ClassIdError;

    fn try_from(text: String) -> Result<Self, Self::Error> {
        text.parse()
    }
}

impl ClassInfo {
    /// Returns true for a class that processes audio.
    #[must_use]
    pub fn is_audio_effect(&self) -> bool {
        self.category == AUDIO_EFFECT_CATEGORY
    }

    /// Returns true when the subcategories mark the class as an instrument.
    #[must_use]
    pub fn is_instrument(&self) -> bool {
        self.subcategories.split('|').any(|token| token == "Instrument")
    }
}

/// Reads the factory's own description.
pub(crate) fn read_factory_info(factory: &ComPtr<IPluginFactory>) -> FactoryInfo {
    // SAFETY: an all-zero PFactoryInfo is a valid value of the plain C struct.
    let mut info: PFactoryInfo = unsafe { std::mem::zeroed() };
    // SAFETY: the factory pointer is valid while the ComPtr lives, and `info` is a writable struct of the right type.
    if unsafe { factory.getFactoryInfo(&raw mut info) } != kResultOk {
        return FactoryInfo::default();
    }
    FactoryInfo {
        vendor: from_char8(&info.vendor),
        url: from_char8(&info.url),
    }
}

/// Reads every class the factory lists, preferring the Unicode and extended descriptions.
pub(crate) fn read_classes(factory: &ComPtr<IPluginFactory>) -> Vec<ClassInfo> {
    let factory_vendor = read_factory_info(factory).vendor;
    let factory2 = factory.cast::<IPluginFactory2>();
    let factory3 = factory.cast::<IPluginFactory3>();
    // SAFETY: the factory pointer is valid while the ComPtr lives.
    let count = unsafe { factory.countClasses() };
    let mut classes = Vec::new();
    for index in 0..count.max(0) {
        let info = factory3
            .as_ref()
            .and_then(|factory3| read_class_unicode(factory3, index))
            .or_else(|| factory2.as_ref().and_then(|factory2| read_class2(factory2, index)))
            .or_else(|| read_class(factory, index));
        if let Some(mut info) = info {
            if info.vendor.is_empty() {
                info.vendor.clone_from(&factory_vendor);
            }
            classes.push(info);
        }
    }
    classes
}

/// Reads one class through `IPluginFactory3`.
fn read_class_unicode(factory: &ComPtr<IPluginFactory3>, index: i32) -> Option<ClassInfo> {
    // SAFETY: an all-zero PClassInfoW is a valid value of the plain C struct.
    let mut info: PClassInfoW = unsafe { std::mem::zeroed() };
    // SAFETY: the factory pointer is valid while the ComPtr lives, and `info` is a writable struct of the right type.
    let result = unsafe { factory.getClassInfoUnicode(index, &raw mut info) };
    (result == kResultOk).then(|| ClassInfo {
        id: ClassId::from_tuid(&info.cid),
        category: from_char8(&info.category),
        name: from_utf16(&info.name),
        vendor: from_utf16(&info.vendor),
        version: from_utf16(&info.version),
        subcategories: from_char8(&info.subCategories),
        sdk_version: from_utf16(&info.sdkVersion),
    })
}

/// Reads one class through `IPluginFactory2`.
fn read_class2(factory: &ComPtr<IPluginFactory2>, index: i32) -> Option<ClassInfo> {
    // SAFETY: an all-zero PClassInfo2 is a valid value of the plain C struct.
    let mut info: PClassInfo2 = unsafe { std::mem::zeroed() };
    // SAFETY: the factory pointer is valid while the ComPtr lives, and `info` is a writable struct of the right type.
    let result = unsafe { factory.getClassInfo2(index, &raw mut info) };
    (result == kResultOk).then(|| ClassInfo {
        id: ClassId::from_tuid(&info.cid),
        category: from_char8(&info.category),
        name: from_char8(&info.name),
        vendor: from_char8(&info.vendor),
        version: from_char8(&info.version),
        subcategories: from_char8(&info.subCategories),
        sdk_version: from_char8(&info.sdkVersion),
    })
}

/// Reads one class through the basic `IPluginFactory`.
fn read_class(factory: &ComPtr<IPluginFactory>, index: i32) -> Option<ClassInfo> {
    // SAFETY: an all-zero PClassInfo is a valid value of the plain C struct.
    let mut info: PClassInfo = unsafe { std::mem::zeroed() };
    // SAFETY: the factory pointer is valid while the ComPtr lives, and `info` is a writable struct of the right type.
    let result = unsafe { factory.getClassInfo(index, &raw mut info) };
    (result == kResultOk).then(|| ClassInfo {
        id: ClassId::from_tuid(&info.cid),
        category: from_char8(&info.category),
        name: from_char8(&info.name),
        vendor: String::new(),
        version: String::new(),
        subcategories: String::new(),
        sdk_version: String::new(),
    })
}

#[cfg(test)]
mod test_class_id {
    use std::error::Error as _;

    use super::*;

    #[test]
    fn formats_the_four_uid_words_on_every_platform() {
        let id = ClassId::from_tuid(&vst3::uid(0x6E33_2252, 0x5422_4A00, 0xAA69_301A, 0xF318_797D));
        assert_eq!(id.to_string(), "6E33225254224A00AA69301AF318797D");
    }

    #[test]
    fn parses_what_it_formats() {
        let id = ClassId::from_tuid(&vst3::uid(0x0102_0304, 0x0506_0708, 0x090A_0B0C, 0x0D0E_0F10));
        let parsed: ClassId = id.to_string().parse().expect("valid id");
        assert_eq!(parsed, id);
        assert_eq!(parsed.to_tuid(), id.to_tuid());
    }

    #[test]
    fn rejects_malformed_text() {
        assert!("1234".parse::<ClassId>().is_err());
        assert!("ZZ332252542244A00AA69301AF31879".parse::<ClassId>().is_err());
        assert!(format!("{}é", "0".repeat(30)).parse::<ClassId>().is_err());
    }

    #[test]
    fn malformed_ids_keep_the_input_and_hexadecimal_source() {
        let short = "1234".parse::<ClassId>().expect_err("short id");
        assert!(matches!(short, ClassIdError::InvalidFormat { .. }));
        assert_eq!(short.to_string(), "Invalid class id: \"1234\"");
        let text = "ZZ33225254224A00AA69301AF318797D";
        let hex = text.parse::<ClassId>().expect_err("invalid hexadecimal");
        assert!(matches!(hex, ClassIdError::InvalidHex { .. }));
        assert_eq!(hex.to_string(), format!("Invalid class id: \"{text}\""));
        assert!(
            hex.source()
                .expect("hexadecimal source")
                .is::<std::num::ParseIntError>()
        );
    }

    #[test]
    fn serializes_as_a_string() {
        let id = ClassId::from_tuid(&vst3::uid(1, 2, 3, 4));
        let json = serde_json::to_string(&id).expect("serialize");
        assert_eq!(json, "\"00000001000000020000000300000004\"");
        let back: ClassId = serde_json::from_str(&json).expect("deserialize");
        assert_eq!(back, id);
    }
}
