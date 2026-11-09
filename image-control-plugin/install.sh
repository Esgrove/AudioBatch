#!/bin/bash
set -eo pipefail

# Builds ImageControl with build.sh and installs the bundles into the user's plugin folders.
# Optionally validates the installed plugins with pluginval and, on macOS, auval.

USAGE="Usage: $0 [OPTIONS]

Install the ImageControl VST3 plugin, and on macOS its Audio Unit.
The plugin is built first with build.sh, which builds Release into cmake-build-release/,
or cmake-build-ninja-release/ with --ninja on Windows.

The VST3 is copied into the standard VST3 folder,
~/Library/Audio/Plug-Ins/VST3 on macOS and C:\\Program Files\\Common Files\\VST3 on Windows,
which triggers a UAC prompt when the folder is not writable.
On macOS, the Audio Unit is copied into ~/Library/Audio/Plug-Ins/Components,
and the Audio Unit registry is restarted to pick up the new build.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -r | --reuse
        Skip CMake generation and reuse the existing build directory.

    -c | --clean
        Delete the CMake build directory before rebuilding.
        Cannot be used with --reuse.

    -n | --ninja
        Use Ninja as the build system (Windows only, macOS uses Ninja by default).

    -d | --debug
        Build and install the Debug build instead of Release.

    -V | --validate
        Validate the installed VST3 with pluginval if it is in PATH,
        and on macOS also the Audio Unit with auval.

    --validation-level <5-10>
        Set pluginval strictness and enable validation. Default is 5.
        This level does not change auval's strict validation.

    -v | --verbose
        Display commands being executed."

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    VALIDATE=false
    VALIDATION_LEVEL=5
    # Passed on to build.sh, which validates the combinations.
    BUILD_OPTIONS=()

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -r | --reuse)
                BUILD_OPTIONS+=(--reuse)
                ;;
            -c | --clean)
                BUILD_OPTIONS+=(--clean)
                ;;
            -n | --ninja)
                BUILD_OPTIONS+=(--ninja)
                ;;
            -d | --debug)
                BUILD_OPTIONS+=(--debug)
                ;;
            -V | --validate)
                VALIDATE=true
                ;;
            --validation-level)
                if [ $# -lt 2 ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                VALIDATION_LEVEL="$2"
                VALIDATE=true
                shift
                ;;
            -v | --verbose)
                set -x
                BUILD_OPTIONS+=(--verbose)
                ;;
            *)
                print_error "Unknown option: $1"
                print_usage_and_exit
                ;;
        esac
        shift
    done
    plugin_check_validation_level "$VALIDATION_LEVEL"
}

install_vst3() {
    if [ "$BASH_PLATFORM" = "windows" ]; then
        # The system-wide VST3 folder, which needs an elevated shell to write to.
        VST3_DIR="$(cygpath -u "$COMMONPROGRAMFILES")/VST3"
        # Windows hosts expect a plain .vst3 file, so the DLL inside the bundle is installed instead of the bundle folder.
        INSTALL_SOURCE="$BUNDLE/Contents/x86_64-win/ImageControl.vst3"
    else
        VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
        INSTALL_SOURCE="$BUNDLE"
    fi
    if ! (mkdir -p "$VST3_DIR" && rm -rf "${VST3_DIR:?}/ImageControl.vst3" && cp -R "$INSTALL_SOURCE" "$VST3_DIR/ImageControl.vst3") 2> /dev/null; then
        if [ "$BASH_PLATFORM" != "windows" ]; then
            print_error_and_exit "Failed to install to $VST3_DIR"
        fi
        # There is no sudo on Windows, so the copy is repeated in an elevated PowerShell that triggers a UAC prompt.
        print_yellow "Administrator rights are needed for $VST3_DIR, requesting elevation"
        local win_dir win_source win_log log_file ps_command ps_encoded
        win_dir=$(cygpath -w "$VST3_DIR")
        win_source=$(cygpath -w "$INSTALL_SOURCE")
        # The elevated window closes on its own, so its error is written to a log file and shown here.
        log_file=$(mktemp)
        win_log=$(cygpath -w "$log_file")
        ps_command="\$ErrorActionPreference = 'Stop'; try { New-Item -ItemType Directory -Force -Path '$win_dir' | Out-Null; Remove-Item -Recurse -Force '$win_dir\\ImageControl.vst3' -ErrorAction SilentlyContinue; Copy-Item -Force '$win_source' '$win_dir\\ImageControl.vst3' } catch { \$_.Exception.Message | Out-File -Encoding utf8 '$win_log'; exit 1 }"
        ps_encoded=$(printf '%s' "$ps_command" | iconv -f UTF-8 -t UTF-16LE | base64 -w0)
        if ! powershell.exe -NoProfile -Command "\$p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList '-NoProfile','-EncodedCommand','$ps_encoded'; exit \$p.ExitCode"; then
            if [ -s "$log_file" ]; then
                print_error "$(cat "$log_file")"
                print_yellow "Close any DAW that has ImageControl loaded, such as Ableton Live, and try again"
            fi
            rm -f "$log_file"
            print_error_and_exit "Elevated install to $VST3_DIR failed or was cancelled"
        fi
        rm -f "$log_file"
    fi
    print_green "Installed: $VST3_DIR/ImageControl.vst3"
}

install_audio_unit() {
    local components_dir="$HOME/Library/Audio/Plug-Ins/Components"
    mkdir -p "$components_dir"
    rm -rf "${components_dir:?}/ImageControl.component"
    cp -R "$AU_BUNDLE" "$components_dir/"
    # The Audio Unit registry caches components, so it is restarted to pick up the new build.
    killall -q AudioComponentRegistrar || true
    print_green "Installed: $components_dir/ImageControl.component"
}

validate_audio_unit() {
    require_command auval
    # The restarted registry takes a moment to list the new component, and auval fails until it does.
    local attempt
    for attempt in $(seq 1 30); do
        if auval -a 2> /dev/null | grep -q "aufx Egic Esgr"; then
            break
        fi
        if [ "$attempt" -eq 30 ]; then
            print_error_and_exit "The Audio Unit did not register: aufx Egic Esgr"
        fi
        sleep 1
    done
    print_magenta "Validating the Audio Unit with auval"
    auval -strict -v aufx Egic Esgr
}

init_options "$@"

if [ "$BASH_PLATFORM" != "mac" ] && [ "$BASH_PLATFORM" != "windows" ]; then
    print_error_and_exit "Platform is not supported yet"
fi

print_magenta "Running build.sh ${BUILD_OPTIONS[*]}"
"$DIR/build.sh" "${BUILD_OPTIONS[@]}"

# The build script resolves the bundle path for the selected build type.
# The Audio Unit is built next to the VST3 in the same artefacts folder.
BUNDLE=$("$DIR/build.sh" --print "${BUILD_OPTIONS[@]}")
AU_BUNDLE="$(dirname "$(dirname "$BUNDLE")")/AU/ImageControl.component"

install_vst3
if [ "$BASH_PLATFORM" = "mac" ]; then
    install_audio_unit
fi

if [ "$VALIDATE" = true ]; then
    if [ "$BASH_PLATFORM" = "mac" ]; then
        validate_audio_unit
    fi
    plugin_validate_vst3 "$VST3_DIR/ImageControl.vst3" "$VALIDATION_LEVEL"
fi

print_green "Installation complete"
