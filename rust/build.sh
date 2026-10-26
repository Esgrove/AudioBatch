#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Build the Rust binaries in release mode and move them to the Rust workspace root.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --cli-only
        Build only the CLI binary, which skips the slower GUI build.

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
            print_usage_and_exit
            ;;
        -c | --cli-only)
            CLI_ONLY=true
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

for executable in $executables; do
    print_green "${executable%.exe}"
    rm -f "$executable"
    mv ./target/release/"$executable" "$executable"
    # The GUI has no version option, and its help only prints the usage line.
    if [ "${executable%.exe}" = "$CLI_BINARY_NAME" ]; then
        ./"$executable" --version
        ./"$executable" -h || :
    else
        echo "$REPO/$executable"
    fi
    echo ""
done
