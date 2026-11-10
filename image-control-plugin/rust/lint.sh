#!/bin/bash
# Checks primary ImageControl targets without pulling the application's optional platform backends into its scope.
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Lint primary Rust ImageControl crates with strict Clippy warnings.
The default applies supported fixes. --check reports without applying fixes.

OPTIONS:
    -h | --help
        Display these instructions without invoking Cargo.

    -c | --check
        Check all primary plugin targets and features without fixes.
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
    cargo clippy --manifest-path "$REPO_ROOT/Cargo.toml" "${PACKAGES[@]}" --all-targets --all-features -- -D warnings
else
    cargo clippy --manifest-path "$REPO_ROOT/Cargo.toml" --fix --allow-dirty "${PACKAGES[@]}" --all-targets --all-features -- -D warnings
fi
