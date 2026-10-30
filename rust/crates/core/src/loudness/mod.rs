//! Loudness kernels and differential tests against the `ebur128` reference.
//!
//! The true peak interpolator places independent phases in SIMD lanes without changing their tap order.
//! `BlockMeter` uses the same SIMD true peak path on every architecture, including x86.
//! K weighting accumulates weighted energy while filtering, and fixed block windows feed short-term and gated loudness.
//! Ebur128 is a test-only reference, with loudness checked within 0.001 LU and at the displayed precision.
//! The SIMD kernels use `fearless_simd` with NEON on Apple Silicon and AVX2 on x86, detected once per process.

pub(crate) mod gating;
mod k_weighting;

#[doc(hidden)]
pub mod true_peak;

#[cfg(test)]
mod differential;
