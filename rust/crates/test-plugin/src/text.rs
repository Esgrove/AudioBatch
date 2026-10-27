//! Copying text into the fixed-size string fields of the VST3 structs.

use std::ffi::c_char;

/// Copies text into a `char8` array, truncating it so the array always ends with a null.
pub fn copy_char8(text: &str, target: &mut [c_char]) {
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
pub fn copy_utf16(text: &str, target: &mut [u16]) {
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
