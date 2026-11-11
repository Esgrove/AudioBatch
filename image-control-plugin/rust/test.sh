#!/bin/bash
# Runs the own-wrapper plugin contracts against root-workspace packages and a build-tree VST3.
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$SCRIPT_DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Run primary Rust ImageControl tests without physical audio devices or installed plugins.
Choose at most one optional verification mode.

OPTIONS:
    -h | --help
        Display these instructions without running tests.

    -m | --miri
        Check primary parameter/controller/admission contracts under nightly Miri.

    -l | --licenses
        Audit the primary plugin's dependency licenses with cargo-deny.
"
parse_arguments() {
    MODE=native
    local mode_selected=false
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                printf '%s\n' "$USAGE"
                exit 0
                ;;
            "" | -m | --miri | -l | --licenses)
                [ "$mode_selected" = false ] || print_error_and_exit "Choose at most one verification mode. See --help."
                mode_selected=true
                case "$1" in
                    -m | --miri) MODE=miri ;;
                    -l | --licenses) MODE=licenses ;;
                esac
                ;;
            *) print_error_and_exit "Unknown option: $1. See --help." ;;
        esac
        shift
    done
}

run_native_tests() {
    local BUNDLE="${IMAGE_CONTROL_OWN_BUNDLE:-${CARGO_TARGET_DIR:-$REPO_ROOT/rust/target}/bundled/ImageControl RS.vst3}"
    [ -d "$BUNDLE" ] || print_error_and_exit "Build the primary VST3 before native tests: ./build.sh --debug"
    IMAGE_CONTROL_OWN_BUNDLE="$BUNDLE" cargo test --manifest-path "$ROOT_MANIFEST" "${PACKAGES[@]}" --all-features
    cargo run --manifest-path "$ROOT_MANIFEST" -p image-control-plugin-rs --example realtime_host -- --case native-editor-lifecycle --bundle "$BUNDLE"
}

run_miri_tests() {
    cargo +nightly miri test --manifest-path "$ROOT_MANIFEST" -p image-control-plugin-rs --no-default-features --test parameters --test controller_contracts
    cargo +nightly miri test --manifest-path "$ROOT_MANIFEST" -p image-control-plugin-rs --no-default-features --lib -- host::component::component_tests::admission_releases_after_nested_rejection_and_unwind
    # Full partitioned FIR preparation is covered natively, not interpreted for minutes under Miri.
    cargo +nightly miri test --manifest-path "$ROOT_MANIFEST" -p image-control-dsp --lib -- test_processing::malformed_controls_and_rate_are_rejected
}

test_plugin() {
    parse_arguments "$@"
    image_control_resolve_target_directory
    cd "$SCRIPT_DIR"
    local ROOT_MANIFEST="$REPO_ROOT/Cargo.toml"
    local PACKAGES=(-p image-control-dsp -p image-control-plugin-rs -p image-control-xtask)
    case "$MODE" in
        native) run_native_tests ;;
        miri) run_miri_tests ;;
        licenses) cargo deny --manifest-path "$SCRIPT_DIR/crates/plugin/Cargo.toml" check licenses ;;
    esac
}

test_plugin "$@"
