#!/bin/bash
# Installs only independently named Rust formats, never replacing the C++ ImageControl plugins.
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$SCRIPT_DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build and install ImageControl RS VST3 and CLAP, plus Audio Unit on macOS.
Release is the default. Close hosts using this Rust plugin before replacing an installed build.

Default installation folders:
    macOS:   ~/Library/Audio/Plug-Ins/{VST3,CLAP,Components}
    Windows: %LOCALAPPDATA%/Programs/Common/{VST3,CLAP} (per-user, no elevation)

OPTIONS:
    -h | --help
        Display these instructions without building or installing.

    -d | --debug
        Build and install Debug instead of Release.

    -N | --no-build
        Install existing packaged formats without rebuilding.

    -D | --destination <dir>
        Install into this VST3 directory instead of the default.

    -C | --clap-destination <dir>
        Install into this CLAP directory instead of the default.

    -U | --au-destination <dir>
        Install into this Audio Unit directory instead of the default (macOS only).

    -V | --validate
        Validate the installed VST3, the byte-identical build-tree CLAP, and on macOS the registered AU with strict auval.

    -L | --validation-level <5-10>
        Set pluginval strictness and enable installed validation.

Built formats are under repository-root rust/target/bundled/ unless CARGO_TARGET_DIR is supplied.
The installed plugins are named ImageControl RS, separate from C++ ImageControl.
macOS installation restarts the Audio Unit registry. Local builds are ad-hoc signed, not Developer ID notarized.
"
parse_arguments() {
    BUILD_OPTIONS=()
    NO_BUILD=false
    VALIDATE=false
    VALIDATION_LEVEL=5
    DESTINATION=
    CLAP_DESTINATION=
    AU_DESTINATION=
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                printf '%s\n' "$USAGE"
                exit 0
                ;;
            -d | --debug) BUILD_OPTIONS+=(--debug) ;;
            -N | --no-build) NO_BUILD=true ;;
            -V | --validate) VALIDATE=true ;;
            -D | --destination | -C | --clap-destination | -U | --au-destination)
                [ $# -ge 2 ] && [ -n "$2" ] || print_error_and_exit "$1 requires a directory. See --help."
                case "$1" in
                    -D | --destination) DESTINATION="$2" ;;
                    -C | --clap-destination) CLAP_DESTINATION="$2" ;;
                    -U | --au-destination) AU_DESTINATION="$2" ;;
                esac
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

resolve_destinations() {
    case "$BASH_PLATFORM" in
        mac)
            DESTINATION="${DESTINATION:-$HOME/Library/Audio/Plug-Ins/VST3}"
            CLAP_DESTINATION="${CLAP_DESTINATION:-$HOME/Library/Audio/Plug-Ins/CLAP}"
            AU_DESTINATION="${AU_DESTINATION:-$HOME/Library/Audio/Plug-Ins/Components}"
            ;;
        windows)
            [ -z "$AU_DESTINATION" ] || print_error_and_exit "--au-destination is supported only on macOS."
            if [ -z "$DESTINATION" ] || [ -z "$CLAP_DESTINATION" ]; then
                [ -n "${LOCALAPPDATA:-}" ] || print_error_and_exit "LOCALAPPDATA is unset. Supply --destination and --clap-destination."
                require_command cygpath
                local common
                common="$(cygpath -u "$LOCALAPPDATA")/Programs/Common"
                DESTINATION="${DESTINATION:-$common/VST3}"
                CLAP_DESTINATION="${CLAP_DESTINATION:-$common/CLAP}"
            fi
            ;;
        *) print_error_and_exit "Rust ImageControl installation supports macOS and Windows." ;;
    esac
    SOURCES=("$BUNDLE" "$CLAP_BUNDLE")
    INSTALLED=("$DESTINATION/ImageControl RS.vst3" "$CLAP_DESTINATION/ImageControl RS.clap")
    if [ "$BASH_PLATFORM" = mac ]; then
        SOURCES+=("$AU_BUNDLE")
        INSTALLED+=("$AU_DESTINATION/ImageControl RS.component")
    fi
}

prepare_bundles() {
    if [ "$NO_BUILD" = false ]; then
        "$SCRIPT_DIR/build.sh" "${BUILD_OPTIONS[@]}"
    fi
    image_control_check_bundles "$BUNDLE" "$CLAP_BUNDLE" "$AU_BUNDLE"
    local index source installed
    for ((index = 0; index < ${#SOURCES[@]}; index++)); do
        source="${SOURCES[index]}"
        installed="${INSTALLED[index]}"
        [ ! -L "$installed" ] || print_error_and_exit "Refusing to replace a symbolic link: $installed"
        if [ -e "$installed" ]; then
            if [ -d "$source" ]; then
                [ -d "$installed" ] || print_error_and_exit "Installed path is not a bundle directory: $installed"
            else
                [ -f "$installed" ] || print_error_and_exit "Installed path is not a plugin file: $installed"
            fi
        fi
        if [ "$BASH_PLATFORM" = mac ]; then
            require_command codesign
            codesign --verify --deep --strict "$source"
        fi
    done
}

# Restores every published format on failure and retains the only previous copy if recovery is blocked.
cleanup_installation() {
    local status=$? index staging backup installed retain
    trap - EXIT
    for ((index = ${#STAGING[@]} - 1; index >= 0; index--)); do
        staging="${STAGING[index]}"
        [ -n "$staging" ] || continue
        backup="$staging/previous.${SOURCES[index]##*.}"
        installed="${INSTALLED[index]}"
        retain=false
        if [ "$COMMITTED" = false ]; then
            if [ "${PUBLISHED[index]:-false}" = true ]; then
                if ! rm -rf "$installed"; then
                    print_error "Could not remove incomplete installation: $installed"
                    retain=true
                fi
            fi
            if [ -e "$backup" ]; then
                if [ "$retain" = true ] || ! mv "$backup" "$installed"; then
                    print_error "Previous plugin retained for recovery: $backup"
                    retain=true
                fi
            fi
        fi
        if [ "$retain" = false ]; then
            rm -rf "$staging" || status=1
        else
            status=1
        fi
    done
    exit "$status"
}

# Stages and verifies every format before moving any installed plugin, including Windows single-file CLAP.
stage_bundles() {
    local index source directory staged
    STAGING=()
    PUBLISHED=()
    COMMITTED=false
    trap cleanup_installation EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    for ((index = 0; index < ${#SOURCES[@]}; index++)); do
        source="${SOURCES[index]}"
        directory="${INSTALLED[index]%/*}"
        mkdir -p "$directory"
        directory=$(cd "$directory" && pwd)
        INSTALLED[index]="$directory/${source##*/}"
        STAGING[index]=$(mktemp -d "$directory/.imagecontrol-rs-install.XXXXXX")
        staged="${STAGING[index]}/${source##*/}"
        cp -R "$source" "$staged"
        diff -r "$source" "$staged"
        if [ "$BASH_PLATFORM" = mac ]; then
            codesign --verify --deep --strict "$staged"
        fi
    done
}

publish_bundles() {
    local index source staging installed
    for ((index = 0; index < ${#SOURCES[@]}; index++)); do
        source="${SOURCES[index]}"
        staging="${STAGING[index]}"
        installed="${INSTALLED[index]}"
        if [ -e "$installed" ]; then
            mv "$installed" "$staging/previous.${source##*.}"
        fi
        PUBLISHED[index]=true
        mv "$staging/${source##*/}" "$installed"
        diff -r "$source" "$installed"
        if [ "$BASH_PLATFORM" = mac ]; then
            codesign --verify --deep --strict "$installed"
        fi
    done
    COMMITTED=true
    for installed in "${INSTALLED[@]}"; do
        print_green "Installed: $installed"
    done
}

# Polls only the Rust component identity after publication, then requires Apple's strict validation.
validate_audio_unit() {
    require_command auval
    local attempt listing line registered=false
    for ((attempt = 0; attempt < 30; attempt++)); do
        if listing=$(auval -a); then
            while IFS= read -r line; do
                if [[ "$line" =~ ^[[:space:]]*aufx[[:space:]]+Egir[[:space:]]+Esgr([[:space:]]|$) ]]; then
                    registered=true
                    break
                fi
            done <<< "$listing"
        fi
        [ "$registered" = false ] || break
        [ "$attempt" -eq 29 ] || sleep 1
    done
    [ "$registered" = true ] || print_error_and_exit "The Audio Unit did not register: aufx Egir Esgr"
    auval -strict -v aufx Egir Esgr
}

install_plugin() {
    parse_arguments "$@"
    image_control_resolve_target_directory
    local BUNDLE="$CARGO_TARGET_DIR/bundled/ImageControl RS.vst3"
    local CLAP_BUNDLE="$CARGO_TARGET_DIR/bundled/ImageControl RS.clap"
    local AU_BUNDLE="$CARGO_TARGET_DIR/bundled/ImageControl RS.component"
    resolve_destinations
    prepare_bundles
    stage_bundles
    publish_bundles
    if [ "$BASH_PLATFORM" = mac ]; then
        killall -q AudioComponentRegistrar || true
    fi
    if [ "$VALIDATE" = true ]; then
        plugin_validate_vst3 "${INSTALLED[0]}" "$VALIDATION_LEVEL"
        # The direct host authorizes build artifacts only. Publication verifies the installed CLAP is byte-identical.
        image_control_validate_clap "$CLAP_BUNDLE"
        if [ "$BASH_PLATFORM" = mac ]; then
            validate_audio_unit
        fi
    fi
}

install_plugin "$@"
