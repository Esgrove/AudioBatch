#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [--lcov] [--open] [--verbose]

Build and run ImageControl's unit tests with Clang source coverage on macOS.
Reports only image-control-plugin/src/, excluding JUCE, tests, and generated sources.
The isolated cmake-build-mac-coverage/ workspace builds no AudioBatch application.
--lcov also writes coverage/lcov.info, and --open generates and opens coverage/html/."

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

LCOV=false
OPEN=false
while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help) print_usage_and_exit 0 ;;
        -l | --lcov) LCOV=true ;;
        -o | --open) OPEN=true ;;
        -v | --verbose) set -x ;;
        *) print_error_and_exit "Unknown option: $1" ;;
    esac
    shift
done
if [ "$BASH_PLATFORM" != "mac" ]; then
    print_error_and_exit "ImageControl coverage requires macOS."
fi
require_command cmake
require_command xcrun
LLVM_PROFDATA=$(xcrun --find llvm-profdata)
LLVM_COV=$(xcrun --find llvm-cov)
BUILD_DIR="$REPO/cmake-build-mac-coverage"
COVERAGE_DIR="$BUILD_DIR/coverage"
TEST_BINARY="$BUILD_DIR/ImageControlTests"
PROFDATA="$COVERAGE_DIR/coverage.profdata"

plugin_configure "$BUILD_DIR" Debug false ON -DIMAGE_CONTROL_COVERAGE=ON -DIMAGE_CONTROL_SANITIZE=
cmake --build "$BUILD_DIR" --config Debug --target ImageControlTests
# Never merge stale data into a new measurement, including profiles from test discovery.
rm -rf "$COVERAGE_DIR/raw"
mkdir -p "$COVERAGE_DIR/raw"
TEST_STATUS=0
LLVM_PROFILE_FILE="$COVERAGE_DIR/raw/%m-%p.profraw" plugin_run_tests "$BUILD_DIR" Debug || TEST_STATUS=$?
"$LLVM_PROFDATA" merge -sparse "$COVERAGE_DIR"/raw/*.profraw -o "$PROFDATA"

# Pass an explicit source allowlist instead of relying on an exclusion regex that could admit application code.
SOURCES=()
while IFS= read -r -d '' source; do
    if [ -f "$REPO/$source" ]; then
        SOURCES+=("$REPO/$source")
    fi
done < <(git -C "$REPO" ls-files --cached --others --exclude-standard -z -- 'src/*.cpp' 'src/*.h')
if [ "${#SOURCES[@]}" -eq 0 ]; then
    print_error_and_exit "No ImageControl sources were found for the coverage report."
fi
"$LLVM_COV" report "$TEST_BINARY" -instr-profile="$PROFDATA" "${SOURCES[@]}"
if [ "$LCOV" = true ]; then
    "$LLVM_COV" export "$TEST_BINARY" -instr-profile="$PROFDATA" -format=lcov "${SOURCES[@]}" > "$COVERAGE_DIR/lcov.info"
    print_green "LCOV report: $COVERAGE_DIR/lcov.info"
fi
if [ "$OPEN" = true ]; then
    "$LLVM_COV" show "$TEST_BINARY" -instr-profile="$PROFDATA" -format=html -output-dir="$COVERAGE_DIR/html" "${SOURCES[@]}"
    open "$COVERAGE_DIR/html/index.html"
fi
if [ "$TEST_STATUS" -ne 0 ]; then
    print_error_and_exit "ImageControl tests failed, so coverage is incomplete." "$TEST_STATUS"
fi
print_green "ImageControl coverage run passed."
