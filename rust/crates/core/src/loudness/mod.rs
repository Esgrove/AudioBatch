//! Loudness kernels and differential tests against the `ebur128` reference.
//!
//! The true peak interpolator places independent phases in SIMD lanes without changing their tap order.
//! `BlockMeter` uses the same SIMD true peak path on every architecture, including x86.
//! Integrated and short-term loudness still use ebur128, preserving the exact golden history and cache blobs.
//! The SIMD kernels use `fearless_simd` with NEON on Apple Silicon and AVX2 on x86, detected once per process.

pub(crate) mod gating;
mod k_weighting;

#[doc(hidden)]
pub mod true_peak;

#[cfg(test)]
mod differential;
