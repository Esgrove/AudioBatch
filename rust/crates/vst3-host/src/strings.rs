//! Conversions between Rust strings and the fixed-size string fields of the VST3 structs.
//!
//! VST3 uses null-terminated `char8` arrays for ASCII and UTF-8 text, such as class categories,
//! and null-terminated UTF-16 arrays for names that can hold any character.

use std::ffi::c_char;

/// Returns the text of a null-terminated `char8` array, replacing invalid UTF-8.
#[must_use]
pub fn from_char8(characters: &[c_char]) -> String {
    let bytes: Vec<u8> = characters
        .iter()
        .map(|character| *character as u8)
        .take_while(|byte| *byte != 0)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

/// Returns the text of a null-terminated UTF-16 array, replacing invalid code units.
#[must_use]
pub fn from_utf16(characters: &[u16]) -> String {
    let length = characters
        .iter()
        .position(|character| *character == 0)
        .unwrap_or(characters.len());
    String::from_utf16_lossy(characters.get(..length).unwrap_or_default())
}

/// Copies text into a `char8` array, truncating it so the array always ends with a null.
#[cfg(test)]
pub fn to_char8(text: &str, target: &mut [c_char]) {
    let capacity = target.len().saturating_sub(1);
    let mut written = 0;
    for (slot, byte) in target.iter_mut().zip(text.bytes().take(capacity)) {
        *slot = c_char::from_ne_bytes([byte]);
        written += 1;
    }
    for slot in target.iter_mut().skip(written) {
        *slot = 0;
    }
}

/// Copies text into a UTF-16 array, truncating it so the array always ends with a null.
pub fn to_utf16(text: &str, target: &mut [u16]) {
    let capacity = target.len().saturating_sub(1);
    let mut written = 0;
    for (slot, unit) in target.iter_mut().zip(text.encode_utf16().take(capacity)) {
        *slot = unit;
        written += 1;
    }
    for slot in target.iter_mut().skip(written) {
        *slot = 0;
    }
}

#[cfg(test)]
mod test_strings {
    use super::*;

    #[test]
    fn char8_round_trip() {
        let mut buffer = [0 as c_char; 16];
        to_char8("Fx|Delay", &mut buffer);
        assert_eq!(from_char8(&buffer), "Fx|Delay");
    }

    #[test]
    fn char8_truncates_and_keeps_the_null() {
        let mut buffer = [1 as c_char; 4];
        to_char8("abcdef", &mut buffer);
        assert_eq!(from_char8(&buffer), "abc");
        assert_eq!(buffer.last().copied(), Some(0));
    }

    #[test]
    fn utf16_round_trip_with_non_ascii_text() {
        let mut buffer = [0_u16; 32];
        to_utf16("Röhre Ünïcode", &mut buffer);
        assert_eq!(from_utf16(&buffer), "Röhre Ünïcode");
    }

    #[test]
    fn utf16_without_null_reads_the_whole_array() {
        let buffer = [u16::from(b'a'), u16::from(b'b')];
        assert_eq!(from_utf16(&buffer), "ab");
    }
}
