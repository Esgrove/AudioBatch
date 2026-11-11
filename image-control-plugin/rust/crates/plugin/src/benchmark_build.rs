//! Compile-time benchmark provenance and runtime admission, independent of plugin processing and GPU availability.

/// Actual profile family supplied by Cargo, not inferred from debug assertions or output paths.
pub const PROFILE: &str = env!("IMAGECONTROL_BUILD_PROFILE");
/// Actual optimization level supplied by Cargo for the benchmarked plugin library.
pub const OPT_LEVEL: &str = env!("IMAGECONTROL_BUILD_OPT_LEVEL");
/// Effective final CPU target selected by Cargo's rustc flags.
pub const NATIVE_CPU: &str = env!("IMAGECONTROL_BUILD_NATIVE_CPU");

/// Admits timings only for Release-equivalent optimized builds with native CPU targeting.
/// Cargo's bench profile explicitly inherits release and is reported in the release profile family.
///
/// # Errors
/// Returns an actionable build command when development, unoptimized, or non-native artifacts are selected.
pub fn require_optimized_native() -> Result<(), &'static str> {
    validate(PROFILE, OPT_LEVEL, NATIVE_CPU, cfg!(debug_assertions))
}

/// Checks recorded build facts without consulting mutable runtime flags that cannot change a compiled artifact.
fn validate(profile: &str, optimization: &str, native: &str, debug_assertions: bool) -> Result<(), &'static str> {
    if profile != "release" || optimization != "3" || native != "true" || debug_assertions {
        return Err(
            "benchmarks require Release/bench opt-level=3, debug assertions off, and -C target-cpu=native; \
             use cargo bench or cargo run --release from the repository and remove conflicting RUSTFLAGS/profile overrides",
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_release_optimized_native_builds_are_admitted() {
        assert!(validate("release", "3", "true", false).is_ok());
        for (profile, optimization, native, debug) in [
            ("debug", "3", "true", false),
            ("release", "0", "true", false),
            ("release", "3", "false", false),
            ("release", "3", "true", true),
        ] {
            assert!(validate(profile, optimization, native, debug).is_err());
        }
    }
}
