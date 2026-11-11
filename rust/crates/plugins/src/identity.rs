//! Format-aware plugin identities for persisted descriptions and runtime dispatch.
//!
//! Standalone identities encode CLAP ids with a prefix, so they never become VST3 ids by accident.
//! Descriptions instead carry an authoritative format alongside the original, unprefixed id string.

use std::fmt;
use std::str::FromStr;

use serde::{Deserialize, Deserializer, Serialize, Serializer};
use vst3_host::{ClassId, ClassIdError};

/// Prefix distinguishing standalone serialized CLAP identities from legacy VST3 hex strings.
const CLAP_ID_PREFIX: &str = "clap:";

/// The plugin API implemented by a bundle.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum PluginFormat {
    /// Steinberg VST3, also the format of descriptions saved before a format field existed.
    #[default]
    #[serde(rename = "VST3", alias = "Vst3", alias = "vst3")]
    Vst3,
    /// The CLAP audio plugin API.
    #[serde(rename = "CLAP", alias = "Clap", alias = "clap")]
    Clap,
}

/// A plugin class identity, including its API namespace.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum PluginId {
    /// A VST3 component class id.
    Vst3(ClassId),
    /// The exact, case-sensitive id reported by a CLAP descriptor.
    Clap(String),
}

/// An invalid plugin identity or a disagreement between an identity and its declared format.
#[derive(Debug, thiserror::Error)]
pub enum PluginIdError {
    /// A VST3 class id is not the required hexadecimal identifier.
    #[error(transparent)]
    Vst3(#[from] ClassIdError),
    /// A CLAP id is empty or contains a C string terminator.
    #[error("A CLAP plugin id must be nonempty and contain no NUL characters")]
    InvalidClap,
    /// A description claims a different format from the identity it contains.
    #[error("The {declared} plugin description contains a {actual} identity")]
    FormatMismatch {
        /// Format stored alongside the identity.
        declared: PluginFormat,
        /// Format carried by the identity.
        actual: PluginFormat,
    },
}

impl fmt::Display for PluginFormat {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Vst3 => "VST3",
            Self::Clap => "CLAP",
        })
    }
}

impl PluginId {
    /// Decodes an original plugin id using an authoritative description format.
    ///
    /// Hex-looking CLAP ids remain CLAP ids and are never interpreted as VST3 class ids.
    ///
    /// # Errors
    ///
    /// Returns an error for an invalid id of the selected format.
    pub fn from_format(format: PluginFormat, text: &str) -> Result<Self, PluginIdError> {
        match format {
            PluginFormat::Vst3 => Ok(Self::Vst3(text.parse()?)),
            PluginFormat::Clap if !text.is_empty() && !text.contains('\0') => Ok(Self::Clap(text.to_owned())),
            PluginFormat::Clap => Err(PluginIdError::InvalidClap),
        }
    }

    /// Returns the API namespace of this identity.
    #[must_use]
    pub const fn format(&self) -> PluginFormat {
        match self {
            Self::Vst3(_) => PluginFormat::Vst3,
            Self::Clap(_) => PluginFormat::Clap,
        }
    }

    /// Returns the VST3 component class id, without inventing one for another format.
    #[must_use]
    pub const fn vst3(&self) -> Option<ClassId> {
        match self {
            Self::Vst3(id) => Some(*id),
            Self::Clap(_) => None,
        }
    }

    /// Returns the original CLAP descriptor id, without interpreting its contents.
    #[must_use]
    pub fn clap(&self) -> Option<&str> {
        match self {
            Self::Clap(id) => Some(id),
            Self::Vst3(_) => None,
        }
    }

    /// Checks that this identity is valid and agrees with the description's format.
    ///
    /// # Errors
    ///
    /// Returns an error for an invalid CLAP id or a different declared format.
    pub fn validate_format(&self, declared: PluginFormat) -> Result<(), PluginIdError> {
        if self.format() != declared {
            return Err(PluginIdError::FormatMismatch {
                declared,
                actual: self.format(),
            });
        }
        if let Self::Clap(id) = self
            && (id.is_empty() || id.contains('\0'))
        {
            return Err(PluginIdError::InvalidClap);
        }
        Ok(())
    }
}

impl fmt::Display for PluginId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Vst3(id) => id.fmt(formatter),
            Self::Clap(id) => formatter.write_str(id),
        }
    }
}

impl From<ClassId> for PluginId {
    fn from(id: ClassId) -> Self {
        Self::Vst3(id)
    }
}

impl FromStr for PluginId {
    type Err = PluginIdError;

    fn from_str(text: &str) -> Result<Self, Self::Err> {
        text.strip_prefix(CLAP_ID_PREFIX).map_or_else(
            || Self::from_format(PluginFormat::Vst3, text),
            |id| Self::from_format(PluginFormat::Clap, id),
        )
    }
}

impl Serialize for PluginId {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        self.validate_format(self.format()).map_err(serde::ser::Error::custom)?;
        match self {
            Self::Vst3(id) => serializer.collect_str(id),
            Self::Clap(id) => serializer.serialize_str(&format!("{CLAP_ID_PREFIX}{id}")),
        }
    }
}

impl<'de> Deserialize<'de> for PluginId {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        String::deserialize(deserializer)?
            .parse()
            .map_err(serde::de::Error::custom)
    }
}

#[cfg(test)]
mod test_identity {
    use std::collections::HashSet;

    use super::*;

    #[test]
    fn vst3_serialization_preserves_legacy_hex_strings() {
        let id = PluginId::from(ClassId([7; 16]));
        let text = serde_json::to_string(&id).expect("serialized identity");
        assert_eq!(text, format!("\"{}\"", ClassId([7; 16])));
        assert_eq!(serde_json::from_str::<PluginId>(&text).expect("identity"), id);
        assert_eq!(id.vst3(), Some(ClassId([7; 16])));
        assert_eq!(id.clap(), None);
    }

    #[test]
    fn clap_serialization_keeps_hex_and_prefix_looking_ids_unambiguous() {
        for original in [
            "07070707070707070707070707070707",
            "org.example.delay",
            "clap:org.example.effect",
        ] {
            let id = PluginId::Clap(original.to_owned());
            let text = serde_json::to_string(&id).expect("serialized identity");
            assert!(text.starts_with("\"clap:"));
            assert_eq!(serde_json::from_str::<PluginId>(&text).expect("identity"), id);
            assert_eq!(id.clap(), Some(original));
            assert_eq!(id.vst3(), None);
        }
    }

    #[test]
    fn identity_namespaces_have_distinct_equality_and_hashes() {
        let vst3 = PluginId::from(ClassId([7; 16]));
        let clap = PluginId::Clap(vst3.to_string());
        assert_ne!(vst3, clap);
        assert_eq!(HashSet::from([vst3.clone(), clap.clone(), vst3, clap]).len(), 2);
    }

    #[test]
    fn invalid_ids_and_format_mismatches_are_rejected() {
        for original in ["", "invalid\0id"] {
            assert!(PluginId::from_format(PluginFormat::Clap, original).is_err());
            assert!(serde_json::to_string(&PluginId::Clap(original.to_owned())).is_err());
        }
        assert!(PluginId::from_format(PluginFormat::Vst3, "org.example.delay").is_err());
        assert!(matches!(
            PluginId::Clap("org.example.delay".to_owned()).validate_format(PluginFormat::Vst3),
            Err(PluginIdError::FormatMismatch { .. })
        ));
        assert_eq!(PluginFormat::default(), PluginFormat::Vst3);
        assert_eq!(PluginFormat::Vst3.to_string(), "VST3");
        assert_eq!(PluginFormat::Clap.to_string(), "CLAP");
    }
}
