#!/bin/bash
# Builds and packages the own-wrapper Iced/wgpu formats without installing them.
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=image-control-plugin/rust/common.sh
source "$SCRIPT_DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build ImageControl RS VST3, CLAP, and on macOS Audio Unit without installing them.
Release is the default. Artifacts are under repository-root rust/target/bundled/ unless CARGO_TARGET_DIR is supplied.

OPTIONS:
    -h | --help
        Display these instructions and exit without building.

    -d | --debug
        Build Debug instead of Release.

    -V | --validate
        Validate the build-tree VST3 with pluginval at strictness 5 and run direct CLAP/Audio Unit host contracts.
        This never installs plugins, restarts the Audio Unit registry, or runs auval.

    -t | --test
        Run the plugin and DSP tests plus packaged-format contracts after building.
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

build_bundles() {
    cargo run --manifest-path "$PRIMARY_MANIFEST" -p image-control-xtask -- bundle image-control-plugin-rs "${BUILD_PROFILE[@]}"
    image_control_check_bundles "$BUNDLE" "$CLAP_BUNDLE" "$AU_BUNDLE"
    if [ "$BASH_PLATFORM" = mac ]; then
        require_command codesign
        local artifact
        for artifact in "$BUNDLE" "$CLAP_BUNDLE" "$AU_BUNDLE"; do
            codesign --force --sign - --timestamp=none "$artifact"
            codesign --verify --deep --strict "$artifact"
        done
    fi
}

run_tests() {
    IMAGE_CONTROL_OWN_BUNDLE="$BUNDLE" IMAGE_CONTROL_CLAP_BUNDLE="$CLAP_BUNDLE" IMAGE_CONTROL_AU_BUNDLE="$AU_BUNDLE" \
        cargo test --manifest-path "$PRIMARY_MANIFEST" -p image-control-dsp -p image-control-plugin-rs \
        -p image-control-xtask "${BUILD_PROFILE[@]}" --all-features
}

validate_formats() {
    if [ "$VALIDATE" = true ]; then
        plugin_validate_vst3 "$BUNDLE" 5
        if [ "$TEST" = false ]; then
            image_control_validate_clap "$CLAP_BUNDLE" "${BUILD_PROFILE[@]}"
        fi
    fi
    if [ "$BASH_PLATFORM" = mac ] && { [ "$TEST" = true ] || [ "$VALIDATE" = true ]; }; then
        local profile=debug
        if [ ${#BUILD_PROFILE[@]} -ne 0 ]; then
            profile=release
        fi
        image_control_validate_audio_unit "$AU_BUNDLE" "$profile"
    fi
}

build_plugin() {
    parse_arguments "$@"
    image_control_resolve_target_directory
    cd "$SCRIPT_DIR"
    local PRIMARY_MANIFEST="$REPO_ROOT/Cargo.toml"
    local BUNDLE="$CARGO_TARGET_DIR/bundled/ImageControl RS.vst3"
    local CLAP_BUNDLE="$CARGO_TARGET_DIR/bundled/ImageControl RS.clap"
    local AU_BUNDLE="$CARGO_TARGET_DIR/bundled/ImageControl RS.component"
    build_bundles
    if [ "$TEST" = true ]; then
        run_tests
    fi
    validate_formats
    printf '%s\n' "$BUNDLE" "$CLAP_BUNDLE"
    if [ "$BASH_PLATFORM" = mac ]; then
        printf '%s\n' "$AU_BUNDLE"
    fi
}

build_plugin "$@"
