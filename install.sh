#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Install AudioBatch.
On macOS, installs the CLI binary and GUI app.
On Windows, installs the CLI binary only.

The CLI binary is installed to ~/.local/bin.
On macOS, the GUI app is copied to /Applications.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --cli-only
        Install only the CLI binary.

    -a | --app-only
        Install only the GUI app (macOS only).

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
            -v | --verbose)
                set -x
                ;;
        esac
        shift
    done

    # GUI app install is only supported on macOS
    if [ "$BASH_PLATFORM" = "windows" ]; then
        if [ "$INSTALL_APP" = true ] && [ "$INSTALL_CLI" = false ]; then
            print_error_and_exit "--app-only is not supported on Windows"
        fi
        INSTALL_APP=false
    fi

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
    print_green "Installed GUI app to /Applications/$GUI_APP_BUNDLE"
}

init_options "$@"

if [ "$BASH_PLATFORM" != "mac" ] && [ "$BASH_PLATFORM" != "windows" ]; then
    print_error_and_exit "Platform is not supported yet"
fi

cd "$REPO"

# Build release artifacts first
print_magenta "Running release build..."
"$DIR/build.sh" -b Release

if [ "$INSTALL_CLI" = true ]; then
    install_cli
fi

if [ "$INSTALL_APP" = true ]; then
    install_gui_app
fi

echo ""
print_green "Installation complete!"

if [ "$INSTALL_CLI" = true ]; then
    local_cli="$INSTALL_DIR/$CLI_BINARY_NAME"
    if [ -x "$local_cli" ]; then
        print_magenta "Installed CLI version:"
        "$local_cli" --version
    fi
fi
