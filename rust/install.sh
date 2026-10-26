#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Install the Rust binaries to the Cargo bin directory and the shell completions for the CLI.
The GUI is installed as a plain binary that runs from the terminal, not as an app bundle.

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

print_magenta "Installing binaries..."
cargo install --force --locked --path crates/cli
if [ "$CLI_ONLY" = false ]; then
    cargo install --force --locked --path crates/gui
fi
echo ""

print_green "Installed binaries:"
require_command "$CLI_BINARY_NAME" "$CLI_BINARY_NAME not found. Is the Cargo install directory in path?"
echo "$("$CLI_BINARY_NAME" --version) from $(command -v "$CLI_BINARY_NAME")"
if [ "$CLI_ONLY" = false ]; then
    echo "$GUI_BINARY_NAME from $(command -v "$GUI_BINARY_NAME")"
fi
echo ""

"$DIR/completions.sh" --silent
