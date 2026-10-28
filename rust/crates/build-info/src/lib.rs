//! Build script helper that records the build metadata of a binary.
//!
//! The build scripts of the CLI and the GUI call [`emit`],
//! which sets the metadata as compile time environment variables that the binary reads with `env!`:
//!
//! - `BUILD_TIME`: the UTC time the build script last ran, as "YYYY-MM-DD_HH:MM:SS".
//! - `GIT_BRANCH`: the checked out branch, "HEAD" on a detached checkout.
//! - `GIT_COMMIT`: the short hash of the checked out commit.
//! - `RUST_VERSION`: the version line of the compiler.
//! - `VERSION_INFO`: `<version> <build time> <git branch> <git commit>`, what `--version` prints after the binary name.
//!
//! Cargo does not tell a build script the names of the binaries,
//! so a binary prepends its own name from `CARGO_BIN_NAME`, which Cargo sets when it compiles the binary.
//!
//! Values that cannot be read, such as the commit when git is not installed, are "unknown".
//! The build script reruns when the checked out commit or the sources of its crate change,
//! so the build time is when the sources or the commit last changed, not when the binary was linked.

use std::process::Command;

use jiff::Timestamp;

/// Value recorded for metadata that could not be read.
const UNKNOWN: &str = "unknown";

/// Sets the build metadata of the calling package for the compiler,
/// and makes the calling build script rerun when the commit or the crate sources change.
///
/// # Panics
///
/// Panics when called outside a build script, where Cargo does not set `CARGO_PKG_VERSION`.
pub fn emit() {
    let version = std::env::var("CARGO_PKG_VERSION").expect("CARGO_PKG_VERSION not set");
    let git_commit = command_output("git", &["rev-parse", "--short", "HEAD"]);
    let git_branch = command_output("git", &["rev-parse", "--abbrev-ref", "HEAD"]);
    let rustc = std::env::var("RUSTC").unwrap_or_else(|_| String::from("rustc"));
    let rust_version = command_output(&rustc, &["--version"]);
    // jiff instead of the `date` command, which Windows does not have.
    let build_time = Timestamp::now().strftime("%Y-%m-%d_%H:%M:%S").to_string();
    let version_info = format!("{version} {build_time} {git_branch} {git_commit}");

    println!("cargo::rustc-env=BUILD_TIME={build_time}");
    println!("cargo::rustc-env=GIT_BRANCH={git_branch}");
    println!("cargo::rustc-env=GIT_COMMIT={git_commit}");
    println!("cargo::rustc-env=RUST_VERSION={rust_version}");
    println!("cargo::rustc-env=VERSION_INFO={version_info}");

    println!("cargo::rerun-if-changed=src");
    rerun_when_commit_changes();
}

/// Reruns the build script when HEAD moves, by watching HEAD, the branch it points to, and the packed refs.
/// The paths come from git, so they also resolve inside a worktree, where `.git` is a file.
fn rerun_when_commit_changes() {
    let mut watched = vec![String::from("HEAD"), String::from("packed-refs")];
    let symbolic_ref = command_output("git", &["symbolic-ref", "-q", "HEAD"]);
    if symbolic_ref != UNKNOWN {
        watched.push(symbolic_ref);
    }
    for name in watched {
        let path = command_output("git", &["rev-parse", "--git-path", &name]);
        if path != UNKNOWN {
            println!("cargo::rerun-if-changed={path}");
        }
    }
}

/// Runs a command and returns its trimmed standard output,
/// or "unknown" when it cannot run, fails, or prints nothing.
fn command_output(program: &str, arguments: &[&str]) -> String {
    Command::new(program)
        .args(arguments)
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_string())
        .filter(|text| !text.is_empty())
        .unwrap_or_else(|| String::from(UNKNOWN))
}
