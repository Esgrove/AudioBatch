//! Our own EBU R128 loudness and true peak meter, replacing the `ebur128` crate piece by piece.
//!
//! Each part ports the ebur128 algorithm with the same coefficients and the same order of operations,
//! so the measured values stay bit-identical to what the analysis produced before,
//! which the golden tests and the differential tests in `differential.rs` enforce.
//! The short-term loudness is the only exception, summed from 100 ms block energies instead of all samples,
//! and compared within a stated tolerance.
//! The SIMD kernels run through `fearless_simd`, at the best level detected once per process.

#[cfg(test)]
mod differential;
