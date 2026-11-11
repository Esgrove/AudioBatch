#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Run the Rust application tests with code coverage using cargo-llvm-cov and cargo-nextest.
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

    -r | --report-only
        Write LCOV, HTML, and a summary from existing coverage data without running tests.
        Append the summary to GITHUB_STEP_SUMMARY when set.
        Cannot be combined with --clean.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    CLEAN=false
    LCOV=false
    OPEN=false
    REPORT_ONLY=false

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
            -r | --report-only)
                REPORT_ONLY=true
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

    if [ "$REPORT_ONLY" = true ] && [ "$CLEAN" = true ]; then
        print_error_and_exit "--report-only cannot be combined with --clean."
    fi
}

run_coverage() {
    parse_arguments "$@"
    require_command cargo "Cargo not found in path. Maybe install rustup?"
    require_command cargo-llvm-cov "cargo-llvm-cov not found. Install it with: cargo install --locked cargo-llvm-cov"

    cd "$REPO"

    # Use one directory for test mappings, the executable build, subprocess profiles, and all reports.
    export CARGO_LLVM_COV_TARGET_DIR="$REPO/target/llvm-cov-target"

    if [ "$REPORT_ONLY" = true ]; then
        mkdir -p target/llvm-cov
        cargo llvm-cov report "${APPLICATION_PACKAGE_ARGS[@]}" --lcov --output-path target/llvm-cov/lcov.info
        html_options=()
        if [ "$OPEN" = true ]; then
            html_options=(--open)
        fi
        cargo llvm-cov report "${APPLICATION_PACKAGE_ARGS[@]}" --html --output-dir target/llvm-cov "${html_options[@]}"
        # Capture first so a failed report cannot be hidden by the closing Markdown fence.
        summary=$(cargo llvm-cov report "${APPLICATION_PACKAGE_ARGS[@]}" --summary-only)
        if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
            # shellcheck disable=SC2016
            printf '## Rust code coverage\n```text\n%s\n```\n' "$summary" | tee -a "$GITHUB_STEP_SUMMARY"
        else
            printf '%s\n' "$summary"
        fi
        exit 0
    fi

    require_command cargo-nextest "cargo-nextest not found. Install it with: cargo install --locked cargo-nextest"

    if [ "$CLEAN" = true ]; then
        print_magenta "Removing previous coverage data..."
        cargo llvm-cov clean --workspace
    fi

    print_magenta "Running tests with coverage..."
    TEST_STATUS=0
    cargo llvm-cov nextest "${APPLICATION_PACKAGE_ARGS[@]}" --no-fail-fast --no-report || TEST_STATUS=$?

    (run_cli_coverage) || TEST_STATUS=$?
    report_coverage
}

# Build and run the real CLI with the same instrumentation as nextest.
# The shared suite preserves LLVM_PROFILE_FILE in every child environment.
# Keep this environment scoped so cargo-llvm-cov's report command sees its normal flags.
run_cli_coverage() {
    local coverage_environment
    coverage_environment=$(CARGO_TARGET_DIR="$CARGO_LLVM_COV_TARGET_DIR" cargo llvm-cov show-env --sh) || return "$?"
    eval "$coverage_environment" || return "$?"
    export LLVM_PROFILE_FILE="$CARGO_LLVM_COV_TARGET_DIR/rust-cli-integration-%m-%p.profraw"
    CARGO_TARGET_DIR="$CARGO_LLVM_COV_TARGET_DIR" "$REPO/test.sh" --cli-integration \
        --artifacts target/integration-artifacts/cli-coverage
}

report_coverage() {
    cargo llvm-cov report "${APPLICATION_PACKAGE_ARGS[@]}" --summary-only

    if [ "$LCOV" = true ]; then
        mkdir -p target/llvm-cov
        cargo llvm-cov report "${APPLICATION_PACKAGE_ARGS[@]}" --lcov --output-path target/llvm-cov/lcov.info
        print_green "LCOV report: $REPO/target/llvm-cov/lcov.info"
    fi

    if [ "$OPEN" = true ]; then
        cargo llvm-cov report "${APPLICATION_PACKAGE_ARGS[@]}" --html --output-dir target/llvm-cov --open
        print_green "HTML report: $REPO/target/llvm-cov/html/index.html"
    fi

    if [ "$TEST_STATUS" -ne 0 ]; then
        print_error "Tests failed, the report includes only the code they reached."
        exit "$TEST_STATUS"
    fi
}

run_coverage "$@"
