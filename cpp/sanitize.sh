#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Build the tests with AddressSanitizer and UndefinedBehaviorSanitizer using the macos-asan preset,
or with ThreadSanitizer using the macos-tsan preset, then run them with ctest.
Each preset uses its own build directory, cmake-build-mac-asan or cmake-build-mac-tsan,
so the debug build directory is not affected.
Only macOS is supported.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -t | --thread
        Use ThreadSanitizer instead of AddressSanitizer and UndefinedBehaviorSanitizer.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

PRESET="macos-asan"
BUILD_DIR="cmake-build-mac-asan"

init_options() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -t | --thread)
                PRESET="macos-tsan"
                BUILD_DIR="cmake-build-mac-tsan"
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

init_options "$@"

if [ "$BASH_PLATFORM" != "mac" ]; then
    print_error_and_exit "The sanitizer build is only supported on macOS."
fi

cd "$REPO"

print_magenta "Configuring and building with the $PRESET preset..."
cmake --preset "$PRESET" -DBUILD_TESTING=ON
cmake --build --preset "$PRESET" --target AudioBatchTests

print_magenta "Running tests..."
ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error

print_green "Sanitizer run passed."
