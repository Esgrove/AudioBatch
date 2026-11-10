#!/bin/bash
# Formats first-party Rust, shell scripts, and prose without rewriting vendored dependencies.
set -eo pipefail

USAGE="Usage: $0 [--check]

Format primary Rust ImageControl crates, the legacy prototype, local shell scripts, and evaluation prose.

OPTIONS:
    -h | --help    Display these instructions without invoking formatters.
    --check       Check formatting without changing files.
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
case "${1:-}" in
    "")
        cargo fmt --manifest-path "$REPO_ROOT/Cargo.toml" "${PACKAGES[@]}"
        cargo fmt --manifest-path Cargo.toml --all
        ;;
    --check)
        cargo fmt --manifest-path "$REPO_ROOT/Cargo.toml" "${PACKAGES[@]}" --check
        cargo fmt --manifest-path Cargo.toml --all --check
        ;;
    *) print_error_and_exit "Usage: ./format.sh [--check]" ;;
esac
if command -v shfmt > /dev/null 2>&1; then
    if [ "${1:-}" = --check ]; then
        shfmt --diff ./*.sh
    else
        shfmt --write ./*.sh
    fi
fi
if command -v slb > /dev/null 2>&1; then
    if [ "${1:-}" = --check ]; then
        slb crates ./*.toml "$REPO_ROOT/image-control-plugin/docs/rust-evaluation.md"
    else
        slb --fix crates ./*.toml "$REPO_ROOT/image-control-plugin/docs/rust-evaluation.md"
    fi
fi
