#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [--thread] [--verbose]

Build and run ImageControl's unit tests on macOS under AddressSanitizer and UndefinedBehaviorSanitizer.
Use --thread for a separate ThreadSanitizer build.
Each configuration uses its own cmake-build-mac-asan/ or cmake-build-mac-tsan/ workspace.
No application target or installed plugin is built or modified."

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

SANITIZERS="address,undefined"
BUILD_DIR="$REPO/cmake-build-mac-asan"
while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help) print_usage_and_exit 0 ;;
        -t | --thread)
            SANITIZERS="thread"
            BUILD_DIR="$REPO/cmake-build-mac-tsan"
            ;;
        -v | --verbose) set -x ;;
        *) print_error_and_exit "Unknown option: $1" ;;
    esac
    shift
done

if [ "$BASH_PLATFORM" != "mac" ]; then
    print_error_and_exit "ImageControl sanitizer builds require macOS."
fi
require_command cmake

print_magenta "Building ImageControl tests with $SANITIZERS"
plugin_configure "$BUILD_DIR" Debug false ON -DIMAGE_CONTROL_SANITIZE="$SANITIZERS" -DIMAGE_CONTROL_COVERAGE=OFF
cmake --build "$BUILD_DIR" --config Debug --target ImageControlTests
# UBSan must fail the process, and TSan must not report findings with a successful exit status.
export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1:print_stacktrace=1"
export TSAN_OPTIONS="${TSAN_OPTIONS:+$TSAN_OPTIONS:}halt_on_error=1:exitcode=66"
plugin_run_tests "$BUILD_DIR" Debug
print_green "ImageControl sanitizer run passed."
