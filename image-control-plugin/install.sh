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
        Validate the installed VST3 with pluginval, and on macOS also the Audio Unit with auval.
        Use pluginval from PATH, or fetch and build the pinned version in ../.deps/ if missing.

    -L | --validation-level <5-10>
        Set pluginval strictness and enable validation. Default is 5.
        This level does not change auval's strict validation.

    -v | --verbose
        Display commands being executed."

for argument in "$@"; do
    case "$argument" in
        -h | --help)
            printf '%s\n' "$USAGE"
            exit 0
            ;;
        --) break ;;
    esac
done

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
            -L | --validation-level)
                if [ $# -lt 2 ]; then
                    print_error_and_exit "Missing value for --validation-level."
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

# Copy and verify beside the destination, then publish by rename with a recoverable previous installation.
# Status 2 prevents an elevated retry from hiding a publication or recovery failure.
install_replacement() (
    local source="$1" destination="$2" verify_signature="${3:-false}"
    local directory transaction staged backup publishing=false
    directory=$(dirname "$destination") || exit 1
    mkdir -p "$directory" || exit 1
    transaction=$(mktemp -d "$directory/.ImageControl-install.XXXXXXXX") || exit 1
    staged="$transaction/${destination##*/}"
    backup="$transaction/previous"

    # Invoked by the EXIT trap, including explicit failures while errexit is disabled by the caller.
    # shellcheck disable=SC2329
    finish_replacement() {
        local status=$?
        trap - EXIT
        if [ "$status" -ne 0 ] && { [ -e "$backup" ] || [ -L "$backup" ]; }; then
            # Never delete the transaction if recovery fails. It holds the only intact previous copy.
            if ! rm -rf "$destination" || ! mv "$backup" "$destination"; then
                print_error "Recovery failed. Previous installation retained at $backup"
                exit 2
            fi
            status=2
        elif [ "$status" -ne 0 ] && [ "$publishing" = true ]; then
            if ! rm -rf "$destination"; then
                print_error "Failed to remove incomplete installation at $destination"
                exit 2
            fi
            status=2
        fi
        if ! rm -rf "$transaction"; then
            print_error "Failed to clean installation transaction at $transaction"
            exit 2
        fi
        exit "$status"
    }
    trap finish_replacement EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    cp -R "$source" "$staged" || exit 1
    diff -r "$source" "$staged" || exit 1
    if [ "$verify_signature" = true ]; then
        codesign --verify --deep --strict "$staged" || exit 1
    fi
    if [ -e "$destination" ] || [ -L "$destination" ]; then
        mv "$destination" "$backup" || exit 1
    fi
    publishing=true
    mv "$staged" "$destination" || exit 1
    diff -r "$source" "$destination" || exit 1
    if [ "$verify_signature" = true ]; then
        codesign --verify --deep --strict "$destination" || exit 1
    fi
)

# PowerShell single-quoted literals represent apostrophes by doubling them, not by shell escaping.
powershell_literal() {
    local value
    value=${1//\'/\'\'}
    printf "'%s'" "$value"
}

# The elevated process uses native literal paths throughout and keeps its backup if rollback fails.
install_vst3_elevated() {
    print_yellow "Administrator rights are needed for $VST3_DIR, requesting elevation"
    local win_dir win_source win_log log_file ps_body ps_command ps_encoded
    win_dir=$(cygpath -w "$VST3_DIR")
    win_source=$(cygpath -w "$INSTALL_SOURCE")
    # The elevated window closes on its own, so its error is written to a log file and shown here.
    log_file=$(mktemp)
    win_log=$(cygpath -w "$log_file")
    read -r -d '' ps_body << 'POWERSHELL' || true
$ErrorActionPreference = 'Stop'
$transaction = $null
$failure = $null
$keepBackup = $false
$committed = $false
$publishing = $false
try {
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $transaction = Join-Path $directory ('.ImageControl-install.' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($transaction) | Out-Null
    $destination = Join-Path $directory 'ImageControl.vst3'
    $staged = Join-Path $transaction 'ImageControl.vst3'
    $backup = Join-Path $transaction 'previous'
    [IO.File]::Copy($source, $staged)
    $expected = (Get-FileHash -LiteralPath $source).Hash
    if ((Get-FileHash -LiteralPath $staged).Hash -ne $expected) { throw 'Staged copy verification failed' }
    if (Test-Path -LiteralPath $destination) {
        Move-Item -LiteralPath $destination -Destination $backup
    }
    $publishing = $true
    Move-Item -LiteralPath $staged -Destination $destination
    if ((Get-FileHash -LiteralPath $destination).Hash -ne $expected) { throw 'Published copy verification failed' }
    $committed = $true
} catch {
    $failure = $_.Exception.Message
    if (!$committed -and $backup -and (Test-Path -LiteralPath $backup)) {
        try {
            if (Test-Path -LiteralPath $destination) {
                Remove-Item -LiteralPath $destination -Recurse -Force
            }
            Move-Item -LiteralPath $backup -Destination $destination
        } catch {
            $keepBackup = $true
            $failure += "; Recovery failed. Previous installation retained at $backup. " + $_.Exception.Message
        }
    } elseif ($publishing -and (Test-Path -LiteralPath $destination)) {
        try { Remove-Item -LiteralPath $destination -Recurse -Force } catch {
            $keepBackup = $true
            $failure += "; Failed to remove incomplete installation at $destination. " + $_.Exception.Message
        }
    }
}
if ($transaction -and !$keepBackup) {
    try { Remove-Item -LiteralPath $transaction -Recurse -Force } catch {
        $failure += "; Failed to clean installation transaction at $transaction. " + $_.Exception.Message
    }
}
if ($failure) {
    $failure | Out-File -LiteralPath $log -Encoding utf8
    exit 1
}
POWERSHELL
    ps_command="\$directory = $(powershell_literal "$win_dir"); \$source = $(powershell_literal "$win_source"); \$log = $(powershell_literal "$win_log");
$ps_body"
    ps_encoded=$(printf '%s' "$ps_command" | iconv -f UTF-8 -t UTF-16LE | base64 | tr -d '\r\n')
    if ! powershell.exe -NoProfile -Command "\$ErrorActionPreference = 'Stop'; \$p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList '-NoProfile','-EncodedCommand','$ps_encoded'; exit \$p.ExitCode"; then
        if [ -s "$log_file" ]; then
            print_error "$(< "$log_file")"
            print_yellow "Close any DAW that has ImageControl loaded, such as Ableton Live, and try again"
        fi
        rm -f "$log_file"
        print_error_and_exit "Elevated install to $VST3_DIR failed or was cancelled"
    fi
    rm -f "$log_file"
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
    local status
    if install_replacement "$INSTALL_SOURCE" "$VST3_DIR/ImageControl.vst3"; then
        :
    else
        status=$?
        if [ "$BASH_PLATFORM" != "windows" ] || [ "$status" -eq 2 ]; then
            print_error_and_exit "Failed to install to $VST3_DIR"
        fi
        # There is no sudo on Windows, so staging is repeated in an elevated PowerShell after a safe failure.
        install_vst3_elevated
    fi
    print_green "Installed: $VST3_DIR/ImageControl.vst3"
}

install_audio_unit() {
    local components_dir="$HOME/Library/Audio/Plug-Ins/Components"
    install_replacement "$AU_BUNDLE" "$components_dir/ImageControl.component" true
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
