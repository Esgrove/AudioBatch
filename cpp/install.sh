#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Install AudioBatch.
On macOS, installs the CLI binary and GUI app.
On Windows, installs the CLI binary and GUI app.

The CLI binary is installed to ~/.local/bin.
On macOS, the GUI app is copied to /Applications.
On Windows, the GUI app is installed to %LOCALAPPDATA%/Programs/AudioBatch,
with a Start Menu shortcut.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --cli-only
        Install only the CLI binary.

    -a | --app-only
        Install only the GUI app.

    -r | --reuse
        Skip CMake generation and reuse the existing release build directory.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    INSTALL_CLI=true
    INSTALL_APP=true
    REUSE_BUILD_DIR=false

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                echo "$USAGE"
                exit 1
                ;;
            -c | --cli-only)
                INSTALL_CLI=true
                INSTALL_APP=false
                ;;
            -a | --app-only)
                INSTALL_CLI=false
                INSTALL_APP=true
                ;;
            -r | --reuse)
                REUSE_BUILD_DIR=true
                ;;
            -v | --verbose)
                set -x
                ;;
            *)
                print_error "Unknown option: $1"
                print_usage_and_exit
                ;;
        esac
        shift
    done

    CLI_BINARY_NAME="audiobatch"
    GUI_APP_BUNDLE="AudioBatch.app"
    INSTALL_DIR="$HOME/.local/bin"
    BUILD_DIR="$REPO/cmake-build-${BASH_PLATFORM}-release"

    if [ "$BASH_PLATFORM" = "windows" ]; then
        CLI_BINARY_NAME="audiobatch.exe"
    fi
}

find_cli_binary() {
    # Check repo root first, then build directory
    if [ -f "$REPO/$CLI_BINARY_NAME" ]; then
        echo "$REPO/$CLI_BINARY_NAME"
    elif [ "$BASH_PLATFORM" = "windows" ]; then
        local build_path="$BUILD_DIR/AudioBatchCli_artefacts/Release/$CLI_BINARY_NAME"
        if [ -f "$build_path" ]; then
            echo "$build_path"
        fi
    else
        local build_path="$BUILD_DIR/AudioBatchCli_artefacts/Release/$CLI_BINARY_NAME"
        if [ -f "$build_path" ]; then
            echo "$build_path"
        fi
    fi
}

find_gui_app() {
    # Check repo root first, then build directory
    if [ -d "$REPO/$GUI_APP_BUNDLE" ]; then
        echo "$REPO/$GUI_APP_BUNDLE"
    else
        local build_path="$BUILD_DIR/AudioBatch_artefacts/Release/$GUI_APP_BUNDLE"
        if [ -d "$build_path" ]; then
            echo "$build_path"
        fi
    fi
}

find_windows_gui_executable() {
    # Check repo root first, then build directory.
    if [ -f "$REPO/AudioBatchApp.exe" ]; then
        echo "$REPO/AudioBatchApp.exe"
    else
        local build_path="$BUILD_DIR/AudioBatch_artefacts/Release/AudioBatch.exe"
        if [ -f "$build_path" ]; then
            echo "$build_path"
        fi
    fi
}

install_cli() {
    local cli_source
    cli_source=$(find_cli_binary)
    if [ -z "$cli_source" ]; then
        print_error_and_exit "CLI binary '$CLI_BINARY_NAME' not found. Build the project first."
    fi

    print_magenta "Installing CLI binary..."
    echo "Source: $cli_source"

    if [ ! -d "$INSTALL_DIR" ]; then
        print_yellow "Creating $INSTALL_DIR..."
        mkdir -p "$INSTALL_DIR"
    fi

    mv -f "$cli_source" "$INSTALL_DIR/$CLI_BINARY_NAME"
    chmod 755 "$INSTALL_DIR/$CLI_BINARY_NAME"
    print_green "Installed CLI binary to $INSTALL_DIR/$CLI_BINARY_NAME"
}

install_windows_gui_app() {
    local app_source
    app_source=$(find_windows_gui_executable)
    if [ -z "$app_source" ]; then
        print_error_and_exit "GUI app 'AudioBatchApp.exe' not found. Build the project first."
    fi

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
    app_install_dir="$local_app_data/Programs/AudioBatch"
    start_menu_dir="$roaming_app_data/Microsoft/Windows/Start Menu/Programs/AudioBatch"
    local app_destination="$app_install_dir/AudioBatchApp.exe"
    local shortcut_destination="$start_menu_dir/AudioBatch.lnk"
    local windows_app_destination
    local windows_shortcut_destination

    windows_app_destination=$(cygpath -w "$app_destination")
    windows_shortcut_destination=$(cygpath -w "$shortcut_destination")

    print_magenta "Installing Windows GUI app..."
    echo "Source:                         $(cygpath -w "$app_source")"
    mkdir -p "$app_install_dir" "$start_menu_dir"
    cp -f "$app_source" "$app_destination"

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

install_gui_app() {
    local app_source
    app_source=$(find_gui_app)
    if [ -z "$app_source" ]; then
        print_error_and_exit "GUI app '$GUI_APP_BUNDLE' not found. Build the project first."
    fi

    print_magenta "Installing GUI app..."
    echo "Source: $app_source"

    if [ -d "/Applications/$GUI_APP_BUNDLE" ]; then
        print_yellow "Removing existing /Applications/$GUI_APP_BUNDLE..."
        rm -rf "/Applications/$GUI_APP_BUNDLE"
    fi

    mv -f "$app_source" "/Applications/$GUI_APP_BUNDLE"
    echo "Installed GUI app to /Applications/$GUI_APP_BUNDLE"
}

init_options "$@"

if [ "$BASH_PLATFORM" != "mac" ] && [ "$BASH_PLATFORM" != "windows" ]; then
    print_error_and_exit "Platform is not supported yet"
fi

cd "$REPO"

# Build release artifacts first
print_magenta "Running release build..."
if [ "$REUSE_BUILD_DIR" = true ]; then
    "$DIR/build.sh" -b Release --reuse
else
    "$DIR/build.sh" -b Release
fi

if [ "$INSTALL_CLI" = true ]; then
    install_cli
fi

if [ "$INSTALL_APP" = true ]; then
    if [ "$BASH_PLATFORM" = "windows" ]; then
        install_windows_gui_app
    else
        install_gui_app
    fi
fi

print_green "Installation complete"
