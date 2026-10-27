//! Conversions of VST3 enum constants to the integer types the interface methods take.
//!
//! The bindings give enum constants the platform's default C enum type,
//! which is unsigned on macOS and signed on Windows, while the methods take fixed integer types.
//! Converting through `i64` works on both platforms without casts that clippy rejects on one of them.

/// Converts an enum constant to `i32`.
pub fn to_i32(value: impl Into<i64>) -> i32 {
    i32::try_from(value.into()).unwrap_or(i32::MAX)
}

/// Converts an enum constant to `u32`.
pub fn to_u32(value: impl Into<i64>) -> u32 {
    u32::try_from(value.into()).unwrap_or(u32::MAX)
}
