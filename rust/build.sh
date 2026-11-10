#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Build the Rust application binaries and move them to rust/. Defaults to release mode.
On macOS, the GUI is packaged as the AudioBatchRs.app bundle with the app icon.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -l | --cli-only
        Build only the CLI binary, which skips the slower GUI build.

    -d | --debug
        Build the debug binaries instead of release binaries.

    -c | --clean
        Accepted for the root scripts. Only the C++ CMake build directory is cleaned.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

CLI_ONLY=false
BUILD_PROFILE="release"

while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help)
            print_usage_and_exit 0
            ;;
        -l | --cli-only)
            CLI_ONLY=true
            ;;
        -d | --debug)
            BUILD_PROFILE="debug"
            ;;
        -c | --clean)
            # Only the C++ project has a CMake build directory to clean.
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

require_command cargo "Cargo not found in path. Maybe install rustup?"

cd "$REPO"

PROFILE_ARGS=()
if [ "$BUILD_PROFILE" = "release" ]; then
    PROFILE_ARGS=(--release)
fi

print_magenta "Building $BUILD_PROFILE binaries..."
if [ "$CLI_ONLY" = true ]; then
    cargo build "${PROFILE_ARGS[@]}" --bin "$CLI_BINARY_NAME"
    executables=$(executable_name "$CLI_BINARY_NAME")
else
    cargo build "${PROFILE_ARGS[@]}" -p audiobatch-cli -p audiobatch-gui --bins
    executables=$(get_rust_executable_names)
fi

copy_third_party_licenses "$REPO/audiobatch-rs-licenses"

for executable in $executables; do
    rm -f "$executable"
    if [ "$executable" = "$GUI_BINARY_NAME" ] && [ "$BASH_PLATFORM" = "mac" ]; then
        print_green "$GUI_APP_BUNDLE"
        create_mac_app_bundle ./target/"$BUILD_PROFILE"/"$executable" "$REPO/$GUI_APP_BUNDLE"
        "$REPO/$GUI_APP_BUNDLE/Contents/MacOS/$GUI_BINARY_NAME" --version
        echo "$REPO/$GUI_APP_BUNDLE"
        echo ""
        continue
    fi
    print_green "${executable%.exe}"
    mv ./target/"$BUILD_PROFILE"/"$executable" "$executable"
    ./"$executable" --version
    # The help of the GUI only prints the usage line.
    if [ "${executable%.exe}" = "$CLI_BINARY_NAME" ]; then
        ./"$executable" -h || :
    else
        echo "$REPO/$executable"
    fi
    echo ""
done
