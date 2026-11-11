#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build the tests with Clang source based code coverage, run them with ctest, and report the coverage of src/.
Prints a per-file summary by default.
The macos-coverage preset uses its own build directory, cmake-build-mac-coverage,
so the debug build directory is not affected.
The JUCE sources, the tests, and the fetched dependencies are left out of the report.
Only supported on macOS.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --clean
        Remove the previous coverage data before running.

    -l | --lcov
        Also write an LCOV report to cmake-build-mac-coverage/coverage/lcov.info for editors and coverage services.

    -o | --open
        Write the HTML report to cmake-build-mac-coverage/coverage/html and open it in the browser.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
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
}

# Resolves the coverage tools and the isolated instrumented binaries.
prepare_coverage() {
    if [ "$BASH_PLATFORM" != "mac" ]; then
        print_error_and_exit "The coverage build is only supported on macOS."
    fi

    require_command xcrun "xcrun not found in path. Install the Xcode command line tools."
    LLVM_PROFDATA=$(xcrun --find llvm-profdata)
    LLVM_COV=$(xcrun --find llvm-cov)

    PRESET="macos-coverage"
    BUILD_DIR="$REPO/cmake-build-mac-coverage"
    COVERAGE_DIR="$BUILD_DIR/coverage"
    TEST_BINARY="$BUILD_DIR/AudioBatchTests_artefacts/Debug/AudioBatchTests"
    INTEGRATION_BINARY="$BUILD_DIR/AudioBatchIntegrationTests_artefacts/Debug/AudioBatchIntegrationTests"
    CLI_BINARY="$BUILD_DIR/AudioBatchCli_artefacts/Debug/audiobatch"
    PROFDATA="$COVERAGE_DIR/coverage.profdata"
    # Keep only the project sources, so JUCE, the tests, the fetched dependencies, and the SDK headers do not count.
    IGNORE_REGEX='/(JUCE|tests|cmake-build[^/]*|_deps)/|/Applications/Xcode|/Library/Developer'
}

# Clears stale profiles, builds all test tiers, and records their status for reporting.
run_coverage_tests() {
    cd "$REPO"

    if [ "$CLEAN" = true ]; then
        print_magenta "Removing previous coverage data..."
        rm -rf "$COVERAGE_DIR"
    fi
    # Raw profiles from an earlier run would be merged into this one.
    rm -rf "$COVERAGE_DIR/raw"
    mkdir -p "$COVERAGE_DIR/raw"

    print_magenta "Configuring and building with the $PRESET preset..."
    cmake --preset "$PRESET" -DBUILD_TESTING=ON
    cmake --build --preset "$PRESET" --target AudioBatchTests AudioBatchIntegrationTests AudioBatchCli

    print_magenta "Running tests with coverage..."
    # %m-%p separates both instrumented binaries and child processes, including CLI runs through the Python suite.
    # Failing tests still leave profiles, so the report is written before the exit status is checked.
    TEST_STATUS=0
    LLVM_PROFILE_FILE="$COVERAGE_DIR/raw/%m-%p.profraw" \
        ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error || TEST_STATUS=$?
}

# Writes reports even after test failures, then preserves the original test status.
report_coverage() {
    "$LLVM_PROFDATA" merge -sparse "$COVERAGE_DIR"/raw/*.profraw -o "$PROFDATA"

    COVERAGE_OBJECTS=("$TEST_BINARY" -object "$INTEGRATION_BINARY" -object "$CLI_BINARY")
    "$LLVM_COV" report "${COVERAGE_OBJECTS[@]}" -instr-profile="$PROFDATA" -ignore-filename-regex="$IGNORE_REGEX"

    if [ "$LCOV" = true ]; then
        "$LLVM_COV" export "${COVERAGE_OBJECTS[@]}" -instr-profile="$PROFDATA" -ignore-filename-regex="$IGNORE_REGEX" \
            -format=lcov > "$COVERAGE_DIR/lcov.info"
        print_green "LCOV report: $COVERAGE_DIR/lcov.info"
    fi

    if [ "$OPEN" = true ]; then
        "$LLVM_COV" show "${COVERAGE_OBJECTS[@]}" -instr-profile="$PROFDATA" -ignore-filename-regex="$IGNORE_REGEX" \
            -format=html -output-dir="$COVERAGE_DIR/html"
        open "$COVERAGE_DIR/html/index.html"
        print_green "HTML report: $COVERAGE_DIR/html/index.html"
    fi

    if [ "$TEST_STATUS" -ne 0 ]; then
        print_error_and_exit "Some tests failed, so the coverage is incomplete." "$TEST_STATUS"
    fi
    print_green "Coverage run passed."
}

measure_coverage() {
    parse_arguments "$@"
    prepare_coverage
    run_coverage_tests
    report_coverage
}

measure_coverage "$@"
