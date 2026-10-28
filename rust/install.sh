#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Install the Rust CLI to the Cargo bin directory and its shell completions.
On macOS, the GUI is installed as the AudioBatchRs.app bundle in /Applications.
On Windows, the GUI is installed to %LOCALAPPDATA%/Programs/AudioBatchRs, with a Start Menu shortcut.
On other platforms, it is installed as a plain binary next to the CLI.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --cli-only
        Install only the CLI binary, which skips the slower GUI build.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

CLI_ONLY=false

while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help)
            print_usage_and_exit 0
            ;;
        -c | --cli-only)
            CLI_ONLY=true
            ;;
        -v | --verbose)
            set -x
            ;;
        -r | --reuse)
            # Ignored here, for compatibility with cpp/install.sh
            ;;
        *)
            print_error "Unknown option: $1"
            print_usage_and_exit
            ;;
    esac
    shift
done

install_windows_gui_app() {
    if [ -z "${LOCALAPPDATA:-}" ] || [ -z "${APPDATA:-}" ]; then
        print_error_and_exit "LOCALAPPDATA and APPDATA must be set to install the Windows GUI app."
    fi
    require_command cygpath "cygpath is required to install the Windows GUI app. Run this script from Git Bash."
    require_command powershell.exe "PowerShell is required to create the Windows Start Menu shortcut."

    local local_app_data
    local roaming_app_data
    local app_install_dir
    local start_menu_dir
    local_app_data=$(cygpath -u "$LOCALAPPDATA")
    roaming_app_data=$(cygpath -u "$APPDATA")
    app_install_dir="$local_app_data/Programs/$GUI_BINARY_NAME"
    start_menu_dir="$roaming_app_data/Microsoft/Windows/Start Menu/Programs/$GUI_BINARY_NAME"
    APP_DESTINATION="$app_install_dir/$GUI_BINARY_NAME.exe"
    local shortcut_destination="$start_menu_dir/$GUI_BINARY_NAME.lnk"
    local windows_app_destination
    local windows_shortcut_destination
    windows_app_destination=$(cygpath -w "$APP_DESTINATION")
    windows_shortcut_destination=$(cygpath -w "$shortcut_destination")

    print_magenta "Installing Windows GUI app..."
    mkdir -p "$app_install_dir" "$start_menu_dir"
    cp -f "./target/release/$GUI_BINARY_NAME.exe" "$APP_DESTINATION"

    AUDIOBATCH_TARGET_PATH="$windows_app_destination" \
        AUDIOBATCH_SHORTCUT_PATH="$windows_shortcut_destination" \
        powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command '
            $ErrorActionPreference = "Stop"
            $shell = New-Object -ComObject WScript.Shell
            $shortcut = $shell.CreateShortcut($env:AUDIOBATCH_SHORTCUT_PATH)
            $shortcut.TargetPath = $env:AUDIOBATCH_TARGET_PATH
            $shortcut.WorkingDirectory = Split-Path -Parent $env:AUDIOBATCH_TARGET_PATH
            $shortcut.IconLocation = $env:AUDIOBATCH_TARGET_PATH
            $shortcut.Save()
        '

    echo "Installed GUI app to:           $windows_app_destination"
    echo "Created Start Menu shortcut at: $windows_shortcut_destination"
}

require_command cargo "Cargo not found in path. Maybe install rustup?"

cd "$REPO"

APP_DESTINATION="/Applications/$GUI_APP_BUNDLE"

print_magenta "Installing binaries..."
cargo install --force --locked --path crates/cli

if [ "$CLI_ONLY" = false ]; then
    cargo build --release --locked --bin "$GUI_BINARY_NAME"
    if [ "$BASH_PLATFORM" = "mac" ]; then
        create_mac_app_bundle ./target/release/"$GUI_BINARY_NAME" "$APP_DESTINATION"
    elif [ "$BASH_PLATFORM" = "windows" ]; then
        install_windows_gui_app
    fi
fi
echo ""

print_green "Installed binaries:"
require_command "$CLI_BINARY_NAME" "$CLI_BINARY_NAME not found. Is the Cargo install directory in path?"
echo "$("$CLI_BINARY_NAME" --version) from $(command -v "$CLI_BINARY_NAME")"
if [ "$CLI_ONLY" = false ]; then
    if [ "$BASH_PLATFORM" = "mac" ]; then
        echo "$("$APP_DESTINATION/Contents/MacOS/$GUI_BINARY_NAME" --version) in $APP_DESTINATION"
    elif [ "$BASH_PLATFORM" = "windows" ]; then
        echo "$(cygpath -w "$APP_DESTINATION")"
    else
        echo "$("$GUI_BINARY_NAME" --version) from $(command -v "$GUI_BINARY_NAME")"
    fi
fi
echo ""

"$DIR/completions.sh" --silent
