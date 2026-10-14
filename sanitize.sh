#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Build the tests with AddressSanitizer and UndefinedBehaviorSanitizer using the macos-asan preset,
then run them with ctest.
The build uses its own directory, cmake-build-mac-asan, so the debug build directory is not affected.
Only macOS is supported.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
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

print_magenta "Configuring and building with sanitizers..."
cmake --preset macos-asan -DBUILD_TESTING=ON
cmake --build --preset macos-asan --target AudioBatchTests

print_magenta "Running tests..."
ctest --test-dir cmake-build-mac-asan --output-on-failure --no-tests=error

print_green "Sanitizer run passed."
