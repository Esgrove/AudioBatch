#!/bin/bash
# Formats the root-workspace Rust plugin, its scripts, and its evaluation prose.
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PLUGIN_DIR=$(dirname "$SCRIPT_DIR")
# shellcheck source=../common.sh
source "$PLUGIN_DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Format Rust ImageControl crates, local shell scripts, and evaluation prose.

OPTIONS:
    -h | --help
        Display these instructions without invoking formatters.

    -c | --check
        Check formatting without changing files.
"
parse_arguments() {
    CHECK=false
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                printf '%s\n' "$USAGE"
                exit 0
                ;;
            -c | --check) CHECK=true ;;
            *) print_error_and_exit "Unknown option: $1. See --help." ;;
        esac
        shift
    done
}

format_rust() {
    if [ "$CHECK" = true ]; then
        cargo fmt --manifest-path "$REPO_ROOT/Cargo.toml" "${PACKAGES[@]}" --check
    else
        cargo fmt --manifest-path "$REPO_ROOT/Cargo.toml" "${PACKAGES[@]}"
    fi
}

format_shell_scripts() {
    if command -v shfmt > /dev/null 2>&1; then
        if [ "$CHECK" = true ]; then
            shfmt --diff ./*.sh
        else
            shfmt --write ./*.sh
        fi
    fi
}

format_prose() {
    if command -v slb > /dev/null 2>&1; then
        if [ "$CHECK" = true ]; then
            slb crates ./*.toml README.md ARCHITECTURE.md "$SCRIPT_DIR/docs/rust-evaluation.md"
        else
            slb --fix crates ./*.toml README.md ARCHITECTURE.md "$SCRIPT_DIR/docs/rust-evaluation.md"
        fi
    fi
}

# Checks only first-party AU sources, never fetched wrapper or SDK files.
format_audio_unit_sources() {
    local arguments=()
    if [ "$CHECK" = true ]; then arguments=(--dry-run --Werror); else arguments=(-i); fi
    if command -v clang-format > /dev/null 2>&1; then
        clang-format "${arguments[@]}" --style=file au/tests/*.mm
    fi
    if command -v cmake-format > /dev/null 2>&1; then
        if [ "$CHECK" = true ]; then
            cmake-format --check au/CMakeLists.txt au/realtime.cmake
        else
            cmake-format --in-place au/CMakeLists.txt au/realtime.cmake
        fi
    fi
}

format_plugin() {
    parse_arguments "$@"
    cd "$SCRIPT_DIR"
    local PACKAGES=(-p image-control-dsp -p image-control-plugin-rs -p image-control-xtask)
    format_rust
    format_shell_scripts
    format_audio_unit_sources
    format_prose
}

format_plugin "$@"
