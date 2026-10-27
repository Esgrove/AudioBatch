#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Run the workspace tests with code coverage using cargo-llvm-cov and cargo-nextest.
Prints a per-file summary by default.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --clean
        Remove the previous coverage data before running.

    -l | --lcov
        Also write an LCOV report to target/llvm-cov/lcov.info for editors and coverage services.

    -o | --open
        Write the HTML report to target/llvm-cov/html and open it in the browser.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

CLEAN=false
LCOV=false
OPEN=false

while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help)
            print_usage_and_exit 0
            ;;
        -c | --clean)
            CLEAN=true
            ;;
        -l | --lcov)
            LCOV=true
            ;;
        -o | --open)
            OPEN=true
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
require_command cargo-llvm-cov "cargo-llvm-cov not found. Install it with: cargo install --locked cargo-llvm-cov"
require_command cargo-nextest "cargo-nextest not found. Install it with: cargo install --locked cargo-nextest"

cd "$REPO"

if [ "$CLEAN" = true ]; then
    print_magenta "Removing previous coverage data..."
    cargo llvm-cov clean --workspace
fi

print_magenta "Running tests with coverage..."
cargo llvm-cov nextest --workspace --no-fail-fast --no-report

cargo llvm-cov report --summary-only

if [ "$LCOV" = true ]; then
    mkdir -p target/llvm-cov
    cargo llvm-cov report --lcov --output-path target/llvm-cov/lcov.info
    print_green "LCOV report: $REPO/target/llvm-cov/lcov.info"
fi

if [ "$OPEN" = true ]; then
    cargo llvm-cov report --html --open
    print_green "HTML report: $REPO/target/llvm-cov/html/index.html"
fi
