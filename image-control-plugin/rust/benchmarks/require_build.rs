//! Benchmark startup guard shared by all `ImageControl` Criterion targets.

/// Rejects development or overridden non-native builds before setup and timing begin.
pub fn require_release_native() {
    assert!(
        env!("IMAGECONTROL_BUILD_PROFILE") == "release"
            && env!("IMAGECONTROL_BUILD_OPT_LEVEL") == "3"
            && env!("IMAGECONTROL_BUILD_NATIVE_CPU") == "true"
            && !cfg!(debug_assertions),
        "benchmarks require Release/bench opt-level=3 and -C target-cpu=native; remove conflicting build overrides",
    );
}
