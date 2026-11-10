//! Portable conversion from platform C enum representations to the VST3 ABI's fixed integers.
//!
//! SDK enum constants are unsigned on macOS and signed on Windows.

/// Converts a nonnegative SDK enum discriminant to a signed ABI field.
pub(super) fn to_i32(value: impl Into<i64>) -> i32 {
    i32::try_from(value.into()).unwrap_or(i32::MAX)
}

/// Converts a nonnegative SDK enum discriminant to an unsigned ABI flag.
pub(super) fn to_u32(value: impl Into<i64>) -> u32 {
    u32::try_from(value.into()).unwrap_or(u32::MAX)
}
