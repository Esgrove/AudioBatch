//! Process-isolated coverage of the delete environment option and its CLI override.
//!
//! Child environments avoid mutating process-wide variables while the test runner uses several threads.

use std::process::Command;

/// Returns the CLI exit code and stderr with a controlled delete environment value.
fn run(delete: Option<&str>, arguments: &[&str]) -> (i32, String) {
    let mut command = Command::new(env!("CARGO_BIN_EXE_audiobatch-rs"));
    command.env_remove("AUDIOBATCH_DELETE").args(arguments);
    if let Some(value) = delete {
        command.env("AUDIOBATCH_DELETE", value);
    }
    let output = command.output().expect("CLI runs");
    (
        output.status.code().expect("exit code"),
        String::from_utf8(output.stderr).expect("UTF-8 stderr"),
    )
}

#[test]
fn enabled_environment_requires_normalization() {
    for value in ["true", "1", "yes", "on"] {
        let (code, error) = run(Some(value), &["--jobs", "1"]);
        assert_eq!(code, 1, "{value}: {error}");
        assert!(error.contains("--delete can only be used with --normalize"), "{error}");
    }
}

#[test]
fn disabled_environment_leaves_other_validation_unchanged() {
    for value in ["false", "0", "no", "off"] {
        let (code, error) = run(Some(value), &["--force"]);
        assert_eq!(code, 1, "{value}: {error}");
        assert!(error.contains("--force can only be used with --normalize"), "{error}");
    }
}

#[test]
fn invalid_environment_is_rejected() {
    let (code, error) = run(Some("maybe"), &["--normalize"]);
    assert_eq!(code, 1, "{error}");
    assert!(error.contains("invalid value"), "{error}");
}

#[test]
fn cli_flag_overrides_a_disabled_environment() {
    let (code, error) = run(Some("false"), &["-D"]);
    assert_eq!(code, 1, "{error}");
    assert!(error.contains("--delete can only be used with --normalize"), "{error}");
}
