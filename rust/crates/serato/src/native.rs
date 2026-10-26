//! Serato data stored as native FLAC and Ogg Vorbis comments instead of `GEOB` frames.
//!
//! Serato writes each tag of a FLAC file as a `SERATO_*` comment.
//! The value is base64 text of the same bytes a `GEOB` frame holds,
//! wrapped in the MIME type, an empty file name, and the frame description, each null terminated.
//! `decode_native_value` checks that wrapper and returns the frame payload,
//! so the tag can be written as a `GEOB` frame when a file is converted to a format that uses `ID3v2`.
//! Ported from `decodeTagBase64` and `appendSeratoFrame` in the C++ project.

use base64::Engine as _;
use base64::alphabet;
use base64::engine::{DecodePaddingMode, GeneralPurpose, GeneralPurposeConfig};

/// The native comment names Serato uses and the `GEOB` frame description each one corresponds to.
pub const NATIVE_FIELDS: [(&str, &str); 6] = [
    ("SERATO_OVERVIEW", "Serato Overview"),
    ("SERATO_ANALYSIS", "Serato Analysis"),
    ("SERATO_AUTOGAIN", "Serato Autotags"),
    ("SERATO_BEATGRID", "Serato BeatGrid"),
    ("SERATO_MARKERS_V2", "Serato Markers2"),
    ("SERATO_MARKERS", "Serato Markers_"),
];

/// MIME type of every Serato `GEOB` frame and of the wrapper in a native value.
pub const GEOB_MIME_TYPE: &str = "application/octet-stream";

/// Longest base64 text accepted, in bytes, which bounds the decoded size too.
const MAX_ENCODED_BYTES: usize = 4 * 1024 * 1024;

/// Base64 engine that accepts both padded and unpadded text.
const LENIENT_BASE64: GeneralPurpose = GeneralPurpose::new(
    &alphabet::STANDARD,
    GeneralPurposeConfig::new()
        .with_decode_padding_mode(DecodePaddingMode::Indifferent)
        .with_decode_allow_trailing_bits(true),
);

/// Returns the `GEOB` frame description for a native Serato comment name, compared without case.
#[must_use]
pub fn native_field_description(name: &str) -> Option<&'static str> {
    NATIVE_FIELDS
        .iter()
        .find(|(field, _)| field.eq_ignore_ascii_case(name))
        .map(|&(_, description)| description)
}

/// Decodes bounded base64 text, skipping ASCII whitespace and allowing omitted padding.
///
/// Returns `None` for empty text, characters outside the base64 alphabet, padding in the middle,
/// or a length that no base64 encoding produces.
#[must_use]
pub fn decode_tag_base64(text: &[u8]) -> Option<Vec<u8>> {
    if text.len() > MAX_ENCODED_BYTES {
        return None;
    }
    let mut cleaned: Vec<u8> = Vec::with_capacity(text.len());
    for &byte in text {
        if byte.is_ascii_whitespace() || byte == b'\x0b' {
            continue;
        }
        if !(byte.is_ascii_alphanumeric() || matches!(byte, b'+' | b'/' | b'=')) {
            return None;
        }
        cleaned.push(byte);
    }
    if cleaned.is_empty() {
        return None;
    }
    if let Some(padding) = cleaned.iter().position(|&byte| byte == b'=') {
        let padding_length = cleaned.len() - padding;
        let only_padding = cleaned.iter().skip(padding).all(|&byte| byte == b'=');
        if !cleaned.len().is_multiple_of(4) || padding_length > 2 || !only_padding {
            return None;
        }
    }
    if cleaned.len() % 4 == 1 {
        return None;
    }
    LENIENT_BASE64.decode(&cleaned).ok()
}

/// Decodes a native Serato comment value and returns the `GEOB` payload inside it.
///
/// The wrapper must hold the Serato MIME type, an empty file name, and the expected description.
/// Returns `None` for a malformed value, a different wrapper, or a wrapper without a payload.
#[must_use]
pub fn decode_native_value(value: &str, expected_description: &str) -> Option<Vec<u8>> {
    let decoded = decode_tag_base64(value.as_bytes())?;
    let mut remaining = decoded.as_slice();
    for expected in [GEOB_MIME_TYPE, "", expected_description] {
        let terminator = remaining.iter().position(|&byte| byte == 0)?;
        let (field, rest) = remaining.split_at(terminator);
        if field != expected.as_bytes() {
            return None;
        }
        remaining = rest.get(1..)?;
    }
    (!remaining.is_empty()).then(|| remaining.to_vec())
}

#[cfg(test)]
mod test_decode_tag_base64 {
    use super::*;

    #[test]
    fn decodes_padded_and_unpadded_text() {
        assert_eq!(decode_tag_base64(b"aGk="), Some(b"hi".to_vec()));
        assert_eq!(decode_tag_base64(b"aGk"), Some(b"hi".to_vec()));
    }

    #[test]
    fn skips_whitespace() {
        assert_eq!(decode_tag_base64(b"aG\nk=\r\n"), Some(b"hi".to_vec()));
    }

    #[test]
    fn rejects_malformed_text() {
        assert_eq!(decode_tag_base64(b""), None);
        assert_eq!(decode_tag_base64(b"a*Gk"), None);
        assert_eq!(decode_tag_base64(b"aG=k"), None);
        assert_eq!(decode_tag_base64(b"aGkaG"), None);
    }
}

#[cfg(test)]
mod test_decode_native_value {
    use super::*;

    /// Encodes a payload in the wrapper Serato uses for native comments.
    fn wrap(description: &str, payload: &[u8]) -> String {
        let mut bytes = Vec::new();
        for field in [GEOB_MIME_TYPE, "", description] {
            bytes.extend_from_slice(field.as_bytes());
            bytes.push(0);
        }
        bytes.extend_from_slice(payload);
        base64::engine::general_purpose::STANDARD.encode(bytes)
    }

    #[test]
    fn returns_the_payload_of_a_matching_wrapper() {
        let value = wrap("Serato Analysis", &[2, 1]);
        assert_eq!(decode_native_value(&value, "Serato Analysis"), Some(vec![2, 1]));
    }

    #[test]
    fn rejects_a_different_description() {
        let value = wrap("Serato Analysis", &[2, 1]);
        assert_eq!(decode_native_value(&value, "Serato BeatGrid"), None);
    }

    #[test]
    fn rejects_an_empty_payload() {
        let value = wrap("Serato Analysis", &[]);
        assert_eq!(decode_native_value(&value, "Serato Analysis"), None);
    }

    #[test]
    fn maps_native_names_without_case() {
        assert_eq!(native_field_description("serato_markers_v2"), Some("Serato Markers2"));
        assert_eq!(native_field_description("SERATO_OVERVIEW"), Some("Serato Overview"));
        assert_eq!(native_field_description("TITLE"), None);
    }
}
