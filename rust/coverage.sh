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

# Use one directory for test mappings, the executable build, subprocess profiles, and all reports.
export CARGO_LLVM_COV_TARGET_DIR="$REPO/target/llvm-cov-target"

if [ "$CLEAN" = true ]; then
    print_magenta "Removing previous coverage data..."
    cargo llvm-cov clean --workspace
fi

print_magenta "Running tests with coverage..."
TEST_STATUS=0
cargo llvm-cov nextest --workspace --no-fail-fast --no-report || TEST_STATUS=$?

# Build and run the real CLI with the same instrumentation as nextest.
# The shared suite preserves LLVM_PROFILE_FILE in every child environment.
# Keep this environment scoped so cargo-llvm-cov's report command sees its normal flags.
run_cli_coverage() {
    local coverage_environment cli_binary
    coverage_environment=$(CARGO_TARGET_DIR="$CARGO_LLVM_COV_TARGET_DIR" cargo llvm-cov show-env --sh) || return "$?"
    eval "$coverage_environment" || return "$?"
    export LLVM_PROFILE_FILE="$CARGO_LLVM_COV_TARGET_DIR/rust-cli-integration-%m-%p.profraw"
    cargo build --target-dir "$CARGO_LLVM_COV_TARGET_DIR" --bin audiobatch-rs || return "$?"
    require_command uv "uv not found in path, it runs the shared CLI integration suite" || return "$?"
    cli_binary="$CARGO_LLVM_COV_TARGET_DIR/debug/audiobatch-rs"
    if [ "$BASH_PLATFORM" = windows ]; then
        cli_binary="$cli_binary.exe"
    fi
    uv run --script ../tests/cli_integration.py \
        --cli "$cli_binary" \
        --artifacts target/integration-artifacts/cli-coverage
}
(run_cli_coverage) || TEST_STATUS=$?

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

if [ "$TEST_STATUS" -ne 0 ]; then
    print_error "Tests failed, the report includes only the code they reached."
    exit "$TEST_STATUS"
fi
