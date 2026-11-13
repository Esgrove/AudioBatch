#!/bin/bash
# Requires both explicit build-tree plugins and runs their shared audio contract in a bounded test process.
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=image-control-plugin/rust/common.sh
source "$SCRIPT_DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Compare the packaged C++ and Rust ImageControl plugins without installation or audio hardware.
Build both plugins and the ImageControlParity target first.

OPTIONS:
    -h | --help                 Display instructions without checking tools or building.
    -C | --cpp <path>           C++ build-tree VST3 bundle, required.
    -r | --rust <path>          Rust build-tree VST3 bundle, defaults to the last packaged build.
    -e | --executable <path>    Built ImageControlParity executable, required.
"

parse_arguments() {
    CPP_BUNDLE="${IMAGE_CONTROL_CPP_BUNDLE:-}"
    RUST_BUNDLE="${IMAGE_CONTROL_OWN_BUNDLE:-}"
    USE_DEFAULT_RUST_BUNDLE=true
    [ -z "$RUST_BUNDLE" ] || USE_DEFAULT_RUST_BUNDLE=false
    EXECUTABLE="${IMAGE_CONTROL_PARITY_EXECUTABLE:-}"
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                printf '%s\n' "$USAGE"
                exit 0
                ;;
            -C | --cpp | -r | --rust | -e | --executable)
                [ $# -ge 2 ] || print_error_and_exit "$1 requires a path. See --help."
                if [ "$2" = -h ] || [ "$2" = --help ]; then
                    shift
                    continue
                fi
                if [ "$1" = -C ] || [ "$1" = --cpp ]; then
                    CPP_BUNDLE="$2"
                elif [ "$1" = -r ] || [ "$1" = --rust ]; then
                    RUST_BUNDLE="$2"
                    USE_DEFAULT_RUST_BUNDLE=false
                else
                    EXECUTABLE="$2"
                fi
                shift
                ;;
            *) print_error_and_exit "Unknown argument: $1. See --help." ;;
        esac
        shift
    done
}

check_artifacts() {
    [ -n "$CPP_BUNDLE" ] || print_error_and_exit "Give --cpp <build-tree.vst3>."
    [ -n "$EXECUTABLE" ] || print_error_and_exit "Give --executable <ImageControlParity>."
    [ -d "$CPP_BUNDLE/Contents" ] || print_error_and_exit "C++ build-tree VST3 is missing: $CPP_BUNDLE"
    [ -d "$RUST_BUNDLE/Contents" ] || print_error_and_exit "Rust build-tree VST3 is missing: $RUST_BUNDLE"
    [ -f "$EXECUTABLE" ] || print_error_and_exit "Parity executable is missing: $EXECUTABLE"
}

compare_plugins() {
    parse_arguments "$@"
    image_control_resolve_target_directory
    cd "$SCRIPT_DIR"
    if [ "$USE_DEFAULT_RUST_BUNDLE" = true ]; then
        RUST_BUNDLE="${CARGO_TARGET_DIR:-$REPO_ROOT/rust/target}/bundled/ImageControl RS.vst3"
    fi
    check_artifacts
    export IMAGE_CONTROL_CPP_BUNDLE="$CPP_BUNDLE"
    export IMAGE_CONTROL_OWN_BUNDLE="$RUST_BUNDLE"
    export IMAGE_CONTROL_PARITY_EXECUTABLE="$EXECUTABLE"
    cargo test --manifest-path "$REPO_ROOT/Cargo.toml" -p image-control-plugin-rs \
        --test cross_implementation -- --nocapture
}

compare_plugins "$@"
