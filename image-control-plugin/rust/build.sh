#!/bin/bash
# Builds the root-workspace Iced effect or the isolated raw prototype without installing either plugin.
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Build the own-wrapper ImageControl RS VST3 with an Iced/wgpu editor, without installing it.
Release is the default. Primary artifacts are under rust/target/bundled/ at the repository root.

OPTIONS:
    -h | --help    Display these instructions and exit without building.
    --debug       Build Debug instead of Release.
    --validate    Validate the build-tree VST3 with pluginval at strictness 5.
    --test        Run the selected plugin's tests after building.
    --raw         Build the separate raw VST3 prototype instead of the primary Iced plugin.
    --editor      Enable the legacy prototype's GPUI editor when used with --raw.
"
for argument in "$@"; do
    case "$argument" in
        -h | --help)
                     printf '%s\n' "$USAGE"
                                             exit 0
                                                    ;;
        --) break ;;
    esac
done

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PLUGIN_DIR=$(dirname "$SCRIPT_DIR")
# shellcheck source=../common.sh
source "$PLUGIN_DIR/common.sh"
cd "$SCRIPT_DIR"
PRIMARY_MANIFEST="$REPO_ROOT/Cargo.toml"
PROFILE=release
FEATURES=()
VALIDATE=false
TEST=false
RAW=false
for argument in "$@"; do
    case "$argument" in
        --debug) PROFILE=debug ;;
        --editor) FEATURES=(--features editor) ;;
        --raw) RAW=true ;;
        --validate) VALIDATE=true ;;
        --test) TEST=true ;;
        *) print_error_and_exit "Usage: ./build.sh [--debug] [--editor] [--raw] [--test] [--validate]" ;;
    esac
done
BUILD_PROFILE=()
if [ "$PROFILE" = release ]; then BUILD_PROFILE=(--release); fi
if [ "$RAW" = true ]; then
    export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$SCRIPT_DIR/target}"
    cargo build --manifest-path "$SCRIPT_DIR/Cargo.toml" -p image-control-vst3 "${BUILD_PROFILE[@]}" "${FEATURES[@]}"
    if [ "$BASH_PLATFORM" = mac ]; then
        LIBRARY="$CARGO_TARGET_DIR/$PROFILE/libimage_control_vst3.dylib"
    else
        LIBRARY="$CARGO_TARGET_DIR/$PROFILE/image_control_vst3.dll"
    fi
    cargo run --manifest-path "$SCRIPT_DIR/Cargo.toml" -p image-control-vst3 --example bundle "${BUILD_PROFILE[@]}" "${FEATURES[@]}" -- "$LIBRARY" "$CARGO_TARGET_DIR/$PROFILE"
    BUNDLE="$CARGO_TARGET_DIR/$PROFILE/ImageControl RS.vst3"
else
    cargo run --manifest-path "$PRIMARY_MANIFEST" -p image-control-xtask -- bundle image-control-plugin-rs "${BUILD_PROFILE[@]}"
    BUNDLE="${CARGO_TARGET_DIR:-$REPO_ROOT/rust/target}/bundled/ImageControl RS.vst3"
fi
if [ "$BASH_PLATFORM" = mac ]; then
    codesign --force --sign - "$BUNDLE"
    codesign --verify --strict "$BUNDLE"
fi
if [ "$TEST" = true ]; then
    if [ "$RAW" = true ]; then
        cargo test --manifest-path "$SCRIPT_DIR/Cargo.toml" --workspace "${BUILD_PROFILE[@]}" --all-features
    else
        IMAGE_CONTROL_OWN_BUNDLE="$BUNDLE" cargo test --manifest-path "$PRIMARY_MANIFEST" \
            -p image-control-dsp -p image-control-plugin-rs -p image-control-xtask "${BUILD_PROFILE[@]}" --all-features
    fi
fi
if [ "$VALIDATE" = true ]; then
    plugin_validate_vst3 "$BUNDLE" 5
fi
printf '%s\n' "$BUNDLE"
