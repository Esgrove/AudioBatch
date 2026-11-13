//! Serato data stored as native FLAC and Ogg Vorbis comments instead of `GEOB` frames.
//!
//! Serato writes each tag of a FLAC file as a `SERATO_*` comment.
//! The value is base64 text of the same bytes a `GEOB` frame holds,
//! wrapped in the MIME type, an empty file name, and the frame description, each null terminated.
//! `decode_native_value` checks that wrapper and returns the frame payload,
//! so the tag can be written as a `GEOB` frame when a file is converted to a format that uses `ID3v2`.
//! It removes a native export's dangling zero sextet and unambiguous one-byte analysis, raster, or grid padding here,
//! rather than weakening the raw tag parsers.

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

/// Largest decoded wrapper, matching the shared bounded tag payload contract.
const MAXIMUM_DECODED_BYTES: usize = 2 + crate::MAXIMUM_SLICES * crate::BAND_COUNT + 1024;

/// Largest native Base64 value, including a bounded allowance for whitespace.
const MAXIMUM_ENCODED_BYTES: usize = MAXIMUM_DECODED_BYTES.div_ceil(3) * 4 + 4096;

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
    if text.len() > MAXIMUM_ENCODED_BYTES {
        return None;
    }
    let mut cleaned: Vec<u8> = Vec::with_capacity(text.len());
    for &byte in text {
        if byte.is_ascii_whitespace() || byte == b'\x0b' {
            continue;
        }
        cleaned.push(byte);
    }
    decode_cleaned_base64(&cleaned)
}

/// Decodes already whitespace-stripped base64 bytes with strict alphabet and padding checks.
fn decode_cleaned_base64(cleaned: &[u8]) -> Option<Vec<u8>> {
    if cleaned.is_empty()
        || !cleaned
            .iter()
            .all(|&byte| byte.is_ascii_alphanumeric() || matches!(byte, b'+' | b'/' | b'='))
    {
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
    LENIENT_BASE64.decode(cleaned).ok()
}

/// Decodes a native Serato comment value and returns the `GEOB` payload inside it.
///
/// The wrapper must hold the Serato MIME type, an empty file name, and the expected description.
/// Returns `None` for a malformed value, a different wrapper, or a wrapper without a payload.
#[must_use]
pub fn decode_native_value(value: &str, expected_description: &str) -> Option<Vec<u8>> {
    if value.len() > MAXIMUM_ENCODED_BYTES {
        return None;
    }
    let mut text: Vec<u8> = value
        .bytes()
        .filter(|byte| !byte.is_ascii_whitespace() && *byte != b'\x0b')
        .collect();
    // Native Serato exports can append a single zero sextet to an otherwise complete encoding.
    if text.len() % 4 == 1 && text.last() == Some(&b'A') && !text.contains(&b'=') {
        text.pop();
    }
    let decoded = decode_cleaned_base64(&text)?;
    if decoded.len() > MAXIMUM_DECODED_BYTES {
        return None;
    }
    let mut remaining = decoded.as_slice();
    for expected in [GEOB_MIME_TYPE, "", expected_description] {
        let terminator = remaining.iter().position(|&byte| byte == 0)?;
        let (field, rest) = remaining.split_at(terminator);
        if field != expected.as_bytes() {
            return None;
        }
        remaining = rest.get(1..)?;
    }
    let padded = match expected_description {
        "Serato Analysis" => remaining.len() == 3,
        "Serato Overview" => remaining.starts_with(&[1, 5]) && remaining.len() > 18 && (remaining.len() - 2) % 16 == 1,
        "Serato BeatGrid" => {
            remaining.starts_with(&[1, 0])
                && remaining
                    .get(2..6)
                    .and_then(|bytes| bytes.try_into().ok())
                    .is_some_and(|count| {
                        usize::try_from(u32::from_be_bytes(count))
                            .ok()
                            .filter(|&count| count <= crate::MAXIMUM_RECORDS)
                            .and_then(|count| count.checked_mul(8))
                            .and_then(|length| length.checked_add(8))
                            == Some(remaining.len())
                    })
        }
        _ => false,
    };
    if padded && let Some((&0, payload)) = remaining.split_last() {
        remaining = payload;
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
        let padded = wrap("Serato Analysis", &[2, 1, 0]);
        assert_eq!(decode_native_value(&padded, "Serato Analysis"), Some(vec![2, 1]));
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

    #[test]
    fn native_zero_sextet_does_not_weaken_generic_base64() {
        let mut value = wrap("Serato Analysis", &[2, 1, 0]);
        assert!(!value.contains('='));
        value.push('A');
        assert_eq!(decode_tag_base64(value.as_bytes()), None);
        assert_eq!(decode_native_value(&value, "Serato Analysis"), Some(vec![2, 1]));
        value.pop();
        value.push('B');
        assert_eq!(decode_native_value(&value, "Serato Analysis"), None);
    }

    #[test]
    fn trims_only_structural_zero_padding_on_native_rasters_and_grids() {
        let overview = [&[1, 5][..], &[0; 16]].concat();
        let mut grid = vec![1, 0];
        grid.extend_from_slice(&1_u32.to_be_bytes());
        grid.extend_from_slice(&0_f32.to_be_bytes());
        grid.extend_from_slice(&126_f32.to_be_bytes());
        grid.push(0);
        for (description, payload) in [("Serato Overview", overview), ("Serato BeatGrid", grid)] {
            assert_eq!(
                decode_native_value(&wrap(description, &payload), description),
                Some(payload.clone())
            );
            let padded = [payload.as_slice(), &[0]].concat();
            assert_eq!(
                decode_native_value(&wrap(description, &padded), description),
                Some(payload.clone())
            );
            for suffix in [&[1][..], &[0, 0][..]] {
                let malformed = [payload.as_slice(), suffix].concat();
                assert_eq!(
                    decode_native_value(&wrap(description, &malformed), description),
                    Some(malformed)
                );
            }
        }
    }
}
