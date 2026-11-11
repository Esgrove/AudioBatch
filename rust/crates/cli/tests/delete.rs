//! Process-isolated coverage of the delete environment option and its CLI override.
//!
//! Child environments avoid mutating process-wide variables while the test runner uses several threads.

use std::process::{Command, Output};

/// Captures the CLI subprocess output with a controlled delete environment value.
fn run(delete: Option<&str>, arguments: &[&str]) -> Output {
    let mut command = Command::new(env!("CARGO_BIN_EXE_audiobatch-rs"));
    command.env_remove("AUDIOBATCH_DELETE").args(arguments);
    if let Some(value) = delete {
        command.env("AUDIOBATCH_DELETE", value);
    }
    command.output().expect("CLI runs")
}

#[test]
fn enabled_environment_requires_normalization() {
    for value in ["true", "1", "yes", "on"] {
        let output = run(Some(value), &["--jobs", "1"]);
        let error = String::from_utf8(output.stderr).expect("UTF-8 stderr");
        assert_eq!(output.status.code(), Some(1), "{value}: {error}");
        assert!(error.contains("--delete can only be used with --normalize"), "{error}");
    }
}

#[test]
fn disabled_environment_leaves_other_validation_unchanged() {
    for value in ["false", "0", "no", "off"] {
        let output = run(Some(value), &["--force"]);
        let error = String::from_utf8(output.stderr).expect("UTF-8 stderr");
        assert_eq!(output.status.code(), Some(1), "{value}: {error}");
        assert!(error.contains("--force can only be used with --normalize"), "{error}");
    }
}

#[test]
fn invalid_environment_is_rejected() {
    let output = run(Some("maybe"), &["--normalize"]);
    let error = String::from_utf8(output.stderr).expect("UTF-8 stderr");
    assert_eq!(output.status.code(), Some(1), "{error}");
    assert!(error.contains("invalid value"), "{error}");
}

#[test]
fn cli_flag_overrides_a_disabled_environment() {
    let output = run(Some("false"), &["-D"]);
    let error = String::from_utf8(output.stderr).expect("UTF-8 stderr");
    assert_eq!(output.status.code(), Some(1), "{error}");
    assert!(error.contains("--delete can only be used with --normalize"), "{error}");
}
