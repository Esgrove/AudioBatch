#!/bin/bash
# Shares Cargo artifact selection and direct packaged-format checks across Rust ImageControl entry points.

# shellcheck source=../common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

# Resolves Cargo's target directory before changing the caller's working directory.
# Exporting an absolute path keeps nested Cargo commands and installer subprocesses on the same artifact root.
# This does not create directories or authorize an implicit installed-plugin search.
image_control_resolve_target_directory() {
    local directory="${CARGO_TARGET_DIR:-$REPO_ROOT/rust/target}"
    case "$directory" in
        [a-zA-Z]:[\\/]*)
            require_command cygpath
            directory=$(cygpath -u "$directory")
            ;;
        /*) ;;
        *) directory="$PWD/$directory" ;;
    esac
    if [ "$BASH_PLATFORM" = windows ]; then
        require_command cygpath
        directory=$(cygpath -m "$directory")
    fi
    export CARGO_TARGET_DIR="$directory"
}

# Checks every format before testing or staging, including the single-file CLAP delivered on Windows.
image_control_check_bundles() {
    local vst3="$1" clap="$2" audio_unit="${3:-}"
    [ -d "$vst3/Contents" ] || print_error_and_exit "Built VST3 bundle is missing: $vst3. Run build.sh first."
    case "$BASH_PLATFORM" in
        mac)
            [ -d "$clap/Contents" ] || print_error_and_exit "Built CLAP bundle is missing: $clap. Run build.sh first."
            [ -d "$audio_unit/Contents" ] || print_error_and_exit "Built Audio Unit bundle is missing: $audio_unit. Run build.sh first."
            ;;
        windows)
            [ -f "$clap" ] && [ ! -L "$clap" ] || print_error_and_exit "Built CLAP file is missing: $clap. Run build.sh first."
            ;;
        *) print_error_and_exit "Rust ImageControl supports macOS and Windows." ;;
    esac
}

# Runs the native CLAP contract tier against the explicit package, never an installed-plugin search.
image_control_validate_clap() {
    local bundle="$1"
    shift
    IMAGE_CONTROL_CLAP_BUNDLE="$bundle" cargo test --manifest-path "$REPO_ROOT/Cargo.toml" \
        -p image-control-plugin-rs "$@" --all-features --test clap_contracts
}

# Loads the exact Audio Unit bundle with a direct native host, without registration or auval.
# An explicit prebuilt host can be supplied for CI. Otherwise only the dedicated CMake host target is built.
image_control_validate_audio_unit() {
    local bundle="$1" profile="${2:-debug}" host="${IMAGE_CONTROL_AU_HOST:-}" directory architecture
    if [ -z "$host" ]; then
        require_command cmake
        architecture=$(uname -m)
        case "$architecture" in
            arm64 | aarch64) architecture=arm64 ;;
            x86_64) ;;
            *) print_error_and_exit "Unsupported native Audio Unit architecture: $architecture" ;;
        esac
        directory="${IMAGE_CONTROL_AU_BUILD_DIR:-$CARGO_TARGET_DIR/image-control-au/$architecture/$profile}"
        # The host loads the explicit packaged AU, so a preconfigured Release host also serves standalone Debug tests.
        if [ -z "${IMAGE_CONTROL_AU_BUILD_DIR:-}" ] && [ "$profile" = debug ] && [ ! -f "$directory/CMakeCache.txt" ]; then
            directory="$CARGO_TARGET_DIR/image-control-au/$architecture/release"
        fi
        [ -f "$directory/CMakeCache.txt" ] || print_error_and_exit "Audio Unit build tree is missing: $directory. Run build.sh first."
        cmake -S "$REPO/rust/au" -B "$directory" -DIMAGE_CONTROL_AU_BUILD_TESTS=ON
        cmake --build "$directory" --target ImageControlRSAUHost
        host="$directory/ImageControlRSAUHost"
    fi
    [ -x "$host" ] || print_error_and_exit "Native Audio Unit host is missing or not executable: $host"
    IMAGE_CONTROL_AU_BUNDLE="$bundle" "$host" --bundle "$bundle"
    if [ -n "${directory:-}" ]; then
        require_command uv
        uv run --script "$SCRIPT_DIR/au/tests/audit_upstream.py" \
            --source "$directory/_deps/image_control_clap_wrapper-src"
    fi
}
