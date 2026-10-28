//! Compile time version and build metadata.
//!
//! `build.rs` sets the values through the `build-info` crate,
//! and the About dialog, the `--version` flag, and the log show them,
//! so a report can be tied back to the exact build it came from.

/// UTC time recorded when `build.rs` last ran, as "YYYY-MM-DD_HH:MM:SS".
pub const BUILD_TIME: &str = env!("BUILD_TIME");

/// Git branch the app was built from, "HEAD" on a detached checkout.
pub const GIT_BRANCH: &str = env!("GIT_BRANCH");

/// Short git commit hash the app was built from.
pub const GIT_COMMIT: &str = env!("GIT_COMMIT");

/// Package version from the workspace manifest.
pub const PACKAGE_VERSION: &str = env!("CARGO_PKG_VERSION");

/// Version of the Rust compiler that built the app.
pub const RUST_VERSION: &str = env!("RUST_VERSION");

/// One line identity of the build that `--version` prints:
/// `AudioBatchRs <version> <build time> <git branch> <git commit>`.
pub const VERSION_STRING: &str = concat!(env!("CARGO_BIN_NAME"), " ", env!("VERSION_INFO"));

/// Returns the multi-line message of the About dialog with the version, build, and system information.
pub fn about_message() -> String {
    let threads = std::thread::available_parallelism().map_or_else(|_| String::from("?"), |count| count.to_string());
    format!(
        "Version {PACKAGE_VERSION}\n{RUST_VERSION}\n\n\
         Batch audio analysis, normalization, and processing.\n\n\
         Date: {BUILD_TIME}\nCommit: {GIT_COMMIT}\nBranch: {GIT_BRANCH}\n\n\
         OS: {} ({})\nThreads: {threads}",
        std::env::consts::OS,
        std::env::consts::ARCH,
    )
}
