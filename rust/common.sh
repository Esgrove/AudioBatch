#!/bin/bash
set -eo pipefail

# The Rust workspace root, which is the directory of this script.
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export REPO

# shellcheck source=../common.sh
source "$REPO/../common.sh"

# The command line binary, which supports the completion subcommand.
CLI_BINARY_NAME="audiobatch-rs"
# The GUI binary, which has its own minimal argument parsing.
GUI_BINARY_NAME="AudioBatchRs"
export CLI_BINARY_NAME GUI_BINARY_NAME

# Add the .exe suffix to an executable name on Windows
executable_name() {
    if [ "$BASH_PLATFORM" = "windows" ]; then
        echo "$1.exe"
    else
        echo "$1"
    fi
}

# Get all Rust executable names from the [[bin]] sections of the workspace crates
get_rust_executable_names() {
    local executables
    executables=$(awk -F'=' '/\[\[bin\]\]/,/name/ {if($1 ~ /name/) print $2}' "$REPO"/crates/*/Cargo.toml | tr -d ' "')
    if [ "$BASH_PLATFORM" = "windows" ]; then
        # shellcheck disable=SC2001
        executables=$(echo "$executables" | sed 's/$/.exe/')
    fi
    echo "$executables"
}
