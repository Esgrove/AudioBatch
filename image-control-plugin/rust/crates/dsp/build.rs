//! Captures compiled optimization provenance for framework-independent Criterion benchmark admission.

#[path = "../../benchmarks/build_flags.rs"]
mod build_flags;

/// Keeps DSP benchmark build evidence consistent with the native plugin renderer.
fn main() {
    build_flags::emit();
}
