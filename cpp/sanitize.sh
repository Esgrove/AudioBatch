#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Build the tests with sanitizers, then run them with ctest.
On macOS, the macos-asan preset uses AddressSanitizer and UndefinedBehaviorSanitizer,
and the macos-tsan preset uses ThreadSanitizer.
On Windows, the windows-asan preset uses the MSVC AddressSanitizer.
The --clang option selects LLVM AddressSanitizer with the windows-clang-asan preset.
Each preset uses its own build directory, such as cmake-build-mac-asan,
so the debug build directory is not affected.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -t | --thread
        Use ThreadSanitizer instead of AddressSanitizer and UndefinedBehaviorSanitizer.
        Only supported on macOS.

    -v | --verbose
        Display commands being executed.

    --clang
        Use clang-cl and LLVM AddressSanitizer. Only supported on Windows.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

THREAD_SANITIZER=false
USE_CLANG=false
BUILD_CONFIG="Debug"

init_options() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -t | --thread)
                THREAD_SANITIZER=true
                ;;
            -v | --verbose)
                set -x
                ;;
            --clang)
                USE_CLANG=true
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

case "$BASH_PLATFORM" in
    mac)
        if [ "$USE_CLANG" = true ]; then
            print_error_and_exit "--clang is only supported on Windows."
        fi
        if [ "$THREAD_SANITIZER" = true ]; then
            PRESET="macos-tsan"
            BUILD_DIR="cmake-build-mac-tsan"
        else
            PRESET="macos-asan"
            BUILD_DIR="cmake-build-mac-asan"
        fi
        ;;
    windows)
        if [ "$THREAD_SANITIZER" = true ]; then
            print_error_and_exit "ThreadSanitizer is not supported on Windows."
        fi
        PRESET="windows-asan"
        BUILD_DIR="cmake-build-windows-asan"
        if [ "$USE_CLANG" = true ]; then
            PRESET="windows-clang-asan"
            BUILD_DIR="cmake-build-windows-clang-asan"
            BUILD_CONFIG="RelWithDebInfo"
            load_msvc_environment
            CLANG_RESOURCE_DIR=$(clang-cl /clang:--print-resource-dir | tr -d '\r')
            PATH="$(cygpath -u "$CLANG_RESOURCE_DIR")/lib/windows:$PATH"
            export PATH
        fi
        ;;
    *)
        print_error_and_exit "The sanitizer build is only supported on macOS and Windows."
        ;;
esac

cd "$REPO"

print_magenta "Configuring and building with the $PRESET preset..."
cmake --preset "$PRESET" -DBUILD_TESTING=ON
cmake --build --preset "$PRESET" --target AudioBatchTests

print_magenta "Running tests..."
ctest --test-dir "$BUILD_DIR" --build-config "$BUILD_CONFIG" --output-on-failure --no-tests=error

print_green "Sanitizer run passed."
