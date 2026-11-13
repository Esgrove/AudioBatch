#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=rust/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Install the Rust CLI to the Cargo bin directory and its shell completions.
On macOS, the GUI is installed as the AudioBatchRs.app bundle in /Applications.
On Windows, the GUI is installed to %LOCALAPPDATA%/Programs/AudioBatchRs, with a Start Menu shortcut.
The installer packages the GUI only on macOS and Windows.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -l | --cli-only
        Install only the CLI binary, which skips the slower GUI build.

    -a | --app-only
        Install only the GUI app, skipping the CLI and shell completions.

    -c | --clean
        Accepted for the root scripts. Only the C++ CMake build directory is cleaned.

    -r | --reuse
        Accepted for compatibility with the C++ installer, without changing Rust installation.
        Rust still builds and installs through Cargo's incremental build cache.

    -z | --notarize
        Sign with Developer ID, notarize, and staple the GUI app only (macOS only).
        Cannot be combined with --cli-only. CLI distribution is notarized through package-macos.sh.
        Requires APPLE_TEAM_ID / CODESIGN_IDENTITY and notarization credentials in the environment.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    INSTALL_CLI=true
    INSTALL_APP=true
    NOTARIZE=false

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -l | --cli-only)
                INSTALL_CLI=true
                INSTALL_APP=false
                ;;
            -a | --app-only)
                INSTALL_CLI=false
                INSTALL_APP=true
                ;;
            -c | --clean)
                # Only the C++ project has a CMake build directory to clean.
                ;;
            -z | --notarize)
                NOTARIZE=true
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

    if [ "$NOTARIZE" = true ] && [ "$INSTALL_APP" = false ]; then
        print_error_and_exit "--cli-only cannot be combined with --notarize. Use package-macos.sh for CLI distribution."
    fi
    if [ "$NOTARIZE" = true ] && [ "$BASH_PLATFORM" != "mac" ]; then
        print_error_and_exit "--notarize is only supported on macOS."
    fi
}

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
    copy_third_party_licenses "$app_install_dir/licenses"

    # PowerShell expands its own environment variables, not the calling shell.
    # shellcheck disable=SC2016
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

install_application() {
    parse_arguments "$@"
    if [ "$NOTARIZE" = true ]; then
        require_mac_developer_signing false
    fi
    require_command cargo "Cargo not found in path. Maybe install rustup?"

    cd "$REPO"

    APP_DESTINATION="/Applications/$GUI_APP_BUNDLE"

    print_magenta "Installing binaries..."
    if [ "$INSTALL_CLI" = true ]; then
        cargo install --force --locked --path crates/cli
        require_command "$CLI_BINARY_NAME" "$CLI_BINARY_NAME not found. Is the Cargo install directory in path?"
        local cli_installed_path
        cli_installed_path=$(command -v "$CLI_BINARY_NAME")
        # Cargo owns installation and registration. A PATH lookup is not authority to re-sign another installation.
        local usual_cargo_bin="${CARGO_INSTALL_ROOT:-${CARGO_HOME:-$HOME/.cargo}}/bin"
        if [ "$(dirname "$cli_installed_path")" != "$usual_cargo_bin" ]; then
            print_yellow "PATH selects $cli_installed_path. Check Cargo's reported destination for a shadowed installation."
        fi
        copy_third_party_licenses "$(dirname "$cli_installed_path")/audiobatch-rs-licenses"
    fi

    if [ "$INSTALL_APP" = true ]; then
        cargo build --release --locked --bin "$GUI_BINARY_NAME"
        if [ "$BASH_PLATFORM" = "mac" ]; then
            if [ "$NOTARIZE" = true ]; then
                local staged_dir
                staged_dir=$(mktemp -d "${TMPDIR:-/tmp}/audiobatch-rs-app.XXXXXXXX")
                create_mac_app_bundle ./target/release/"$GUI_BINARY_NAME" "$staged_dir/$GUI_APP_BUNDLE"
                notarize_mac_artifact "$staged_dir/$GUI_APP_BUNDLE" --staple
                rm -rf "${APP_DESTINATION:?}"
                mv -f "$staged_dir/$GUI_APP_BUNDLE" "$APP_DESTINATION"
                rm -rf "$staged_dir"
            else
                create_mac_app_bundle ./target/release/"$GUI_BINARY_NAME" "$APP_DESTINATION"
            fi
        elif [ "$BASH_PLATFORM" = "windows" ]; then
            install_windows_gui_app
        fi
    fi
    echo ""

    print_green "Installed binaries:"
    if [ "$INSTALL_CLI" = true ]; then
        require_command "$CLI_BINARY_NAME" "$CLI_BINARY_NAME not found. Is the Cargo install directory in path?"
        echo "$("$CLI_BINARY_NAME" --version) from $(command -v "$CLI_BINARY_NAME")"
    fi
    if [ "$INSTALL_APP" = true ]; then
        if [ "$BASH_PLATFORM" = "mac" ]; then
            echo "$("$APP_DESTINATION/Contents/MacOS/$GUI_BINARY_NAME" --version) in $APP_DESTINATION"
        elif [ "$BASH_PLATFORM" = "windows" ]; then
            cygpath -w "$APP_DESTINATION"
        else
            echo "$("$GUI_BINARY_NAME" --version) from $(command -v "$GUI_BINARY_NAME")"
        fi
    fi
    echo ""

    if [ "$INSTALL_CLI" = true ]; then
        "$DIR/completions.sh" --silent
    fi
}

install_application "$@"
