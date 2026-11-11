#!/bin/bash
# Formats the root-workspace Rust plugin, its scripts, and its evaluation prose.
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Format Rust ImageControl crates, local shell scripts, and evaluation prose.

OPTIONS:
    -h | --help
        Display these instructions without invoking formatters.

    -c | --check
        Check formatting without changing files.
"
for argument in "$@"; do
    case "$argument" in
        -h | --help)
            printf '%s\n' "$USAGE"
            exit 0
            ;;
        --) break ;;
    esac
done

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PLUGIN_DIR=$(dirname "$SCRIPT_DIR")
# shellcheck source=../common.sh
source "$PLUGIN_DIR/common.sh"
cd "$SCRIPT_DIR"
PACKAGES=(-p image-control-dsp -p image-control-plugin-rs -p image-control-xtask)
CHECK=false
for argument in "$@"; do
    case "$argument" in
        -c | --check) CHECK=true ;;
        *) print_error_and_exit "Unknown option: $argument. See --help." ;;
    esac
done
if [ "$CHECK" = true ]; then
    cargo fmt --manifest-path "$REPO_ROOT/Cargo.toml" "${PACKAGES[@]}" --check
else
    cargo fmt --manifest-path "$REPO_ROOT/Cargo.toml" "${PACKAGES[@]}"
fi
if command -v shfmt > /dev/null 2>&1; then
    if [ "$CHECK" = true ]; then
        shfmt --diff ./*.sh
    else
        shfmt --write ./*.sh
    fi
fi
if command -v slb > /dev/null 2>&1; then
    if [ "$CHECK" = true ]; then
        slb crates ./*.toml README.md ARCHITECTURE.md "$SCRIPT_DIR/docs/rust-evaluation.md"
    else
        slb --fix crates ./*.toml README.md ARCHITECTURE.md "$SCRIPT_DIR/docs/rust-evaluation.md"
    fi
fi
