#!/bin/bash
# Installs only the independently named Rust effect, never replacing the C++ ImageControl plugin.
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$SCRIPT_DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build and install ImageControl RS as a VST3 for use in a real plugin host.
Release is the default. This is the Rust own-wrapper Iced/wgpu implementation, not the C++ ImageControl plugin.
No Audio Unit is installed. Close hosts using this Rust plugin before replacing an installed build.

Default installation folders:
    macOS:   ~/Library/Audio/Plug-Ins/VST3
    Windows: %LOCALAPPDATA%/Programs/Common/VST3 (per-user, no elevation)

OPTIONS:
    -h | --help
        Display these instructions without building or installing.

    -d | --debug
        Build and install Debug instead of Release.

    -N | --no-build
        Install the existing primary bundle without rebuilding.

    -D | --destination <dir>
        Install into this VST3 directory instead of the default.

    -V | --validate
        Validate the installed VST3 with pluginval at strictness 5.

    -L | --validation-level <5-10>
        Set pluginval strictness and enable installed validation.

The built bundle is under repository-root rust/target/bundled/ unless CARGO_TARGET_DIR is supplied.
The installed plugin is named ImageControl RS. Rescan VST3 plugins in the host after installation.
Local macOS builds use ad-hoc signing, not Developer ID notarization.
The own VST3 wrapper is checked separately from the abandoned Nice experiment. See docs/rust-evaluation.md.
"
parse_arguments() {
    BUILD_OPTIONS=()
    NO_BUILD=false
    VALIDATE=false
    VALIDATION_LEVEL=5
    DESTINATION=
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                printf '%s\n' "$USAGE"
                exit 0
                ;;
            -d | --debug) BUILD_OPTIONS+=(--debug) ;;
            -N | --no-build) NO_BUILD=true ;;
            -V | --validate) VALIDATE=true ;;
            -D | --destination)
                [ $# -ge 2 ] && [ -n "$2" ] || print_error_and_exit "--destination requires a directory. See --help."
                DESTINATION="$2"
                shift
                ;;
            -L | --validation-level)
                [ $# -ge 2 ] || print_error_and_exit "--validation-level requires an integer from 5 to 10."
                VALIDATION_LEVEL="$2"
                plugin_check_validation_level "$VALIDATION_LEVEL"
                VALIDATE=true
                shift
                ;;
            *) print_error_and_exit "Unknown argument: $1. See --help." ;;
        esac
        shift
    done
}

resolve_destination() {
    case "$BASH_PLATFORM" in
        mac) DESTINATION="${DESTINATION:-$HOME/Library/Audio/Plug-Ins/VST3}" ;;
        windows)
            if [ -z "$DESTINATION" ]; then
                [ -n "${LOCALAPPDATA:-}" ] || print_error_and_exit "LOCALAPPDATA is unset. Supply --destination."
                require_command cygpath
                DESTINATION="$(cygpath -u "$LOCALAPPDATA")/Programs/Common/VST3"
            fi
            ;;
        *) print_error_and_exit "Rust ImageControl installation supports macOS and Windows." ;;
    esac
}

prepare_bundle() {
    if [ "$NO_BUILD" = false ]; then
        "$SCRIPT_DIR/build.sh" "${BUILD_OPTIONS[@]}"
    fi
    [ -d "$BUNDLE/Contents" ] || print_error_and_exit "Built VST3 bundle is missing: $BUNDLE. Run build.sh first."
    if [ "$BASH_PLATFORM" = mac ]; then
        require_command codesign
        codesign --verify --strict "$BUNDLE"
    fi
}

# Preserves an old bundle if publication or rollback fails. Never removes the only recoverable copy.
cleanup_installation() {
    if [ -d "$STAGING/previous.vst3" ]; then
        print_error "Previous plugin retained for recovery: $STAGING/previous.vst3"
    else
        rm -rf "$STAGING"
    fi
}

stage_bundle() {
    mkdir -p "$DESTINATION"
    DESTINATION=$(cd "$DESTINATION" && pwd)
    INSTALLED="$DESTINATION/ImageControl RS.vst3"
    [ ! -L "$INSTALLED" ] || print_error_and_exit "Refusing to replace a symbolic link: $INSTALLED"
    [ ! -e "$INSTALLED" ] || [ -d "$INSTALLED" ] || print_error_and_exit "Installed VST3 path is not a bundle directory: $INSTALLED"
    # EXIT runs after the workflow returns, so its cleanup path must remain in script scope.
    STAGING=$(mktemp -d "$DESTINATION/.imagecontrol-rs-install.XXXXXX")
    trap cleanup_installation EXIT
    cp -R "$BUNDLE" "$STAGING/ImageControl RS.vst3"
    if [ "$BASH_PLATFORM" = mac ]; then
        codesign --verify --strict "$STAGING/ImageControl RS.vst3"
    fi
}

publish_bundle() {
    if [ -d "$INSTALLED" ]; then
        mv "$INSTALLED" "$STAGING/previous.vst3"
    fi
    if ! mv "$STAGING/ImageControl RS.vst3" "$INSTALLED"; then
        if [ -d "$STAGING/previous.vst3" ]; then
            mv "$STAGING/previous.vst3" "$INSTALLED" || print_error "Rollback failed. The previous plugin remains in $STAGING."
        fi
        print_error_and_exit "Could not publish the installed plugin. Close the host and try again."
    fi
    if [ -d "$STAGING/previous.vst3" ]; then
        rm -rf "$STAGING/previous.vst3"
    fi
    print_green "Installed: $INSTALLED"
}

install_plugin() {
    parse_arguments "$@"
    image_control_resolve_target_directory
    local INSTALLED
    local TARGET_DIRECTORY="${CARGO_TARGET_DIR:-$REPO_ROOT/rust/target}"
    local BUNDLE="$TARGET_DIRECTORY/bundled/ImageControl RS.vst3"
    resolve_destination
    prepare_bundle
    stage_bundle
    publish_bundle
    if [ "$VALIDATE" = true ]; then
        plugin_validate_vst3 "$INSTALLED" "$VALIDATION_LEVEL"
    fi
    print_green "Rescan VST3 plugins in your host and select ImageControl RS."
}

install_plugin "$@"
