#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Build the Rust binaries in release mode and move them to the Rust workspace root.
On macOS, the GUI is packaged as the AudioBatchRs.app bundle with the app icon.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -l | --cli-only
        Build only the CLI binary, which skips the slower GUI build.

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

while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help)
            print_usage_and_exit 0
            ;;
        -l | --cli-only)
            CLI_ONLY=true
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

print_magenta "Building binaries..."
if [ "$CLI_ONLY" = true ]; then
    cargo build --release --bin "$CLI_BINARY_NAME"
    executables=$(executable_name "$CLI_BINARY_NAME")
else
    cargo build --release --workspace --bins
    executables=$(get_rust_executable_names)
fi

copy_third_party_licenses "$REPO/audiobatch-rs-licenses"

for executable in $executables; do
    rm -f "$executable"
    if [ "$executable" = "$GUI_BINARY_NAME" ] && [ "$BASH_PLATFORM" = "mac" ]; then
        print_green "$GUI_APP_BUNDLE"
        create_mac_app_bundle ./target/release/"$executable" "$REPO/$GUI_APP_BUNDLE"
        "$REPO/$GUI_APP_BUNDLE/Contents/MacOS/$GUI_BINARY_NAME" --version
        echo "$REPO/$GUI_APP_BUNDLE"
        echo ""
        continue
    fi
    print_green "${executable%.exe}"
    mv ./target/release/"$executable" "$executable"
    ./"$executable" --version
    # The help of the GUI only prints the usage line.
    if [ "${executable%.exe}" = "$CLI_BINARY_NAME" ]; then
        ./"$executable" -h || :
    else
        echo "$REPO/$executable"
    fi
    echo ""
done
