#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build the tests with sanitizers, then run them with ctest.
On macOS, the macos-asan preset uses AddressSanitizer and UndefinedBehaviorSanitizer,
and the macos-tsan preset uses ThreadSanitizer.
The macos-rtsan preset checks focused preview callbacks with Homebrew LLVM RealtimeSanitizer.
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

    -R | --realtime
        Check focused preview callbacks with RealtimeSanitizer. Only supported on macOS.
        Cannot be combined with --thread or --clang.

    -v | --verbose
        Display commands being executed.

    -k | --clang
        Use clang-cl and LLVM AddressSanitizer. Only supported on Windows.
"

parse_arguments() {
    THREAD_SANITIZER=false
    REALTIME_SANITIZER=false
    USE_CLANG=false
    TEST_TARGET="AudioBatchTests"
    TEST_LABEL="unit"
    BUILD_CONFIG="Debug"

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
            -R | --realtime)
                REALTIME_SANITIZER=true
                ;;
            -k | --clang)
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

# Selects an isolated sanitizer workspace and any required Windows runtime paths.
select_sanitizer_build() {
    if [ "$REALTIME_SANITIZER" = true ] && { [ "$THREAD_SANITIZER" = true ] || [ "$USE_CLANG" = true ]; }; then
        print_error_and_exit "--realtime cannot be combined with --thread or --clang."
    fi

    case "$BASH_PLATFORM" in
        mac)
            if [ "$USE_CLANG" = true ]; then
                print_error_and_exit "--clang is only supported on Windows."
            fi
            if [ "$REALTIME_SANITIZER" = true ]; then
                PRESET="macos-rtsan"
                BUILD_DIR="cmake-build-mac-rtsan"
                TEST_TARGET="AudioBatchRealtimeTests"
                TEST_LABEL="realtime"
            elif [ "$THREAD_SANITIZER" = true ]; then
                PRESET="macos-tsan"
                BUILD_DIR="cmake-build-mac-tsan"
            else
                PRESET="macos-asan"
                BUILD_DIR="cmake-build-mac-asan"
            fi
            ;;
        windows)
            if [ "$REALTIME_SANITIZER" = true ]; then
                print_error_and_exit "RealtimeSanitizer is only supported on macOS."
            fi
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
}

# Builds and runs only the selected sanitizer tier.
run_sanitizer_tests() {
    cd "$REPO"

    print_magenta "Configuring and building with the $PRESET preset..."
    cmake --preset "$PRESET" -DBUILD_TESTING=ON
    cmake --build --preset "$PRESET" --target "$TEST_TARGET"

    print_magenta "Running tests..."
    # The integration tier runs separately in the normal macOS and Windows builds.
    # Select only the requested tier, excluding unbuilt targets' unlabelled discovery placeholders too.
    ctest --test-dir "$BUILD_DIR" --build-config "$BUILD_CONFIG" -L "$TEST_LABEL" --output-on-failure --no-tests=error

    if [ "$REALTIME_SANITIZER" = true ]; then
        print_green "Real-time probes completed. BLOCKED known regressions remain, not clean callback guarantees."
    else
        print_green "Sanitizer run passed."
    fi
}

sanitize_project() {
    parse_arguments "$@"
    select_sanitizer_build
    run_sanitizer_tests
}

sanitize_project "$@"
