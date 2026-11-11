#!/bin/bash
# Builds and packages the own-wrapper Iced/wgpu VST3 without installing it.
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$SCRIPT_DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build ImageControl RS VST3 with its Iced/wgpu editor, without installing it.
Release is the default. Artifacts are under rust/target/bundled/ at the repository root.

OPTIONS:
    -h | --help
        Display these instructions and exit without building.

    -d | --debug
        Build Debug instead of Release.

    -V | --validate
        Validate the build-tree VST3 with pluginval at strictness 5.

    -t | --test
        Run the plugin and DSP tests after building.
"

parse_arguments() {
    BUILD_PROFILE=(--release)
    VALIDATE=false
    TEST=false
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                printf '%s\n' "$USAGE"
                exit 0
                ;;
            -d | --debug) BUILD_PROFILE=() ;;
            -V | --validate) VALIDATE=true ;;
            -t | --test) TEST=true ;;
            *) print_error_and_exit "Unknown option: $1. See --help." ;;
        esac
        shift
    done
}

build_bundle() {
    cargo run --manifest-path "$PRIMARY_MANIFEST" -p image-control-xtask -- bundle image-control-plugin-rs "${BUILD_PROFILE[@]}"
    if [ "$BASH_PLATFORM" = mac ]; then
        codesign --force --sign - "$BUNDLE"
        codesign --verify --strict "$BUNDLE"
    fi
}

run_tests() {
    IMAGE_CONTROL_OWN_BUNDLE="$BUNDLE" cargo test --manifest-path "$PRIMARY_MANIFEST" \
        -p image-control-dsp -p image-control-plugin-rs -p image-control-xtask "${BUILD_PROFILE[@]}" --all-features
}

build_plugin() {
    parse_arguments "$@"
    image_control_resolve_target_directory
    cd "$SCRIPT_DIR"
    local PRIMARY_MANIFEST="$REPO_ROOT/Cargo.toml"
    local BUNDLE="${CARGO_TARGET_DIR:-$REPO_ROOT/rust/target}/bundled/ImageControl RS.vst3"
    build_bundle
    if [ "$TEST" = true ]; then
        run_tests
    fi
    if [ "$VALIDATE" = true ]; then
        plugin_validate_vst3 "$BUNDLE" 5
    fi
    printf '%s\n' "$BUNDLE"
}

build_plugin "$@"
