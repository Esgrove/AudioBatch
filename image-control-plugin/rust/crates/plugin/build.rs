//! Captures compiled optimization provenance for plugin benchmark admission and reports.

#[path = "../../benchmarks/build_flags.rs"]
mod build_flags;

/// Records the same effective build contract as the framework-independent DSP benchmarks.
fn main() {
    build_flags::emit();
}
