//! Records resolved Cargo optimization facts for the DSP and plugin benchmark entry points.

use std::env;

/// Emits compile-time provenance from Cargo's actual effective flags, never mutable runtime settings.
pub fn emit() {
    println!("cargo::rerun-if-env-changed=CARGO_ENCODED_RUSTFLAGS");
    println!("cargo::rerun-if-env-changed=PROFILE");
    println!("cargo::rerun-if-env-changed=OPT_LEVEL");
    let flags = env::var("CARGO_ENCODED_RUSTFLAGS").unwrap_or_default();
    let mut cpu = None;
    let mut optimization = env::var("OPT_LEVEL").unwrap_or_default();
    let mut codegen = false;
    for argument in flags.split('\u{1f}') {
        if argument == "-C" {
            codegen = true;
            continue;
        }
        let option = argument.strip_prefix("-C").or_else(|| codegen.then_some(argument));
        if let Some(value) = option.and_then(|option| option.strip_prefix("target-cpu=")) {
            cpu = Some(value);
        }
        if let Some(value) = option.and_then(|option| option.strip_prefix("opt-level=")) {
            optimization = value.into();
        }
        codegen = false;
    }
    let native = cpu == Some("native");
    let profile = env::var("PROFILE").unwrap_or_default();
    println!("cargo::rustc-env=IMAGECONTROL_BUILD_PROFILE={profile}");
    println!("cargo::rustc-env=IMAGECONTROL_BUILD_OPT_LEVEL={optimization}");
    println!("cargo::rustc-env=IMAGECONTROL_BUILD_NATIVE_CPU={native}");
}
