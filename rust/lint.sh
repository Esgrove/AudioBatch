#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Lint the Rust application and the fuzz crate with cargo clippy, denying all warnings.
Applies the machine-applicable fixes, also in a dirty working tree.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --check
        Only report the warnings without applying fixes.
        Exit with code 1 if clippy reports any warning. Intended for CI.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    CHECK=false

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -c | --check)
                CHECK=true
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
}

lint_sources() {
    parse_arguments "$@"
    require_command cargo "Cargo not found in path. Maybe install rustup?"

    cd "$REPO"

    FIX_ARGS=()
    if [ "$CHECK" = false ]; then
        FIX_ARGS=(--fix --allow-dirty)
    fi

    print_magenta "Running cargo clippy on the application packages..."
    cargo clippy "${APPLICATION_PACKAGE_ARGS[@]}" --all-targets "${FIX_ARGS[@]}" -- -D warnings

    # The fuzz crate is its own workspace, and its targets build on stable outside cargo-fuzz.
    print_magenta "Running cargo clippy on the fuzz crate..."
    cargo clippy --manifest-path fuzz/Cargo.toml --all-targets "${FIX_ARGS[@]}" -- -D warnings

    print_green "Clippy found no warnings"
}

lint_sources "$@"
