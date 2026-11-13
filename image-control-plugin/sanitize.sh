#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=image-control-plugin/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [--thread | --realtime] [--verbose]

Build and run ImageControl's unit tests on macOS under AddressSanitizer and UndefinedBehaviorSanitizer.
Use --thread for a separate ThreadSanitizer build.
Use --realtime for callback-only RealtimeSanitizer probes with Homebrew LLVM, including the built VST3.
Each configuration uses its own cmake-build-mac-asan/, cmake-build-mac-tsan/, or cmake-build-mac-rtsan/ workspace.
No application target or installed plugin is built or modified.

OPTIONS:
    -h | --help       Display these instructions without building.
    -t | --thread     Run ThreadSanitizer instead of address/undefined checks.
    -R | --realtime   Run the separate RealtimeSanitizer callback checks.
    -v | --verbose    Display commands being executed."

parse_arguments() {
    SANITIZERS="address,undefined"
    BUILD_DIR="$REPO/cmake-build-mac-asan"
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help) print_usage_and_exit 0 ;;
            -t | --thread)
                if [ "$SANITIZERS" = realtime ]; then
                    print_error_and_exit "--thread and --realtime require separate builds."
                fi
                SANITIZERS="thread"
                BUILD_DIR="$REPO/cmake-build-mac-tsan"
                ;;
            -R | --realtime)
                if [ "$SANITIZERS" = thread ]; then
                    print_error_and_exit "--thread and --realtime require separate builds."
                fi
                SANITIZERS="realtime"
                BUILD_DIR="$REPO/cmake-build-mac-rtsan"
                ;;
            -v | --verbose) set -x ;;
            *) print_error_and_exit "Unknown option: $1" ;;
        esac
        shift
    done
}

# Runs each sanitizer in its isolated workspace without modifying installed plugins.
sanitize_plugin() {
    parse_arguments "$@"
    if [ "$BASH_PLATFORM" != "mac" ]; then
        print_error_and_exit "ImageControl sanitizer builds require macOS."
    fi
    require_command cmake

    print_magenta "Building ImageControl tests with $SANITIZERS"
    if [ "$SANITIZERS" = realtime ]; then
        require_command brew
        require_command uv
        LLVM_PREFIX=$(brew --prefix llvm)
        if [ ! -x "$LLVM_PREFIX/bin/clang++" ] || [ ! -x "$LLVM_PREFIX/bin/clang" ]; then
            print_error_and_exit "RealtimeSanitizer requires Homebrew LLVM. Install it with brew install llvm."
        fi
        plugin_configure "$BUILD_DIR" Debug false ON \
            -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
            -DIMAGE_CONTROL_SANITIZE=realtime -DIMAGE_CONTROL_COVERAGE=OFF
        cmake --build "$BUILD_DIR" --config Debug --target ImageControlRealtimeTests --parallel 4
        export RTSAN_SYMBOLIZER_PATH="$LLVM_PREFIX/bin/llvm-symbolizer"
        ctest --test-dir "$BUILD_DIR" --build-config Debug --output-on-failure --no-tests=error -L realtime
        print_green "ImageControl real-time probes completed. Review BLOCKED known-regression reports separately."
        exit 0
    fi
    plugin_configure "$BUILD_DIR" Debug false ON -DIMAGE_CONTROL_SANITIZE="$SANITIZERS" -DIMAGE_CONTROL_COVERAGE=OFF
    cmake --build "$BUILD_DIR" --config Debug --target ImageControlTests
    # UBSan must fail the process, and TSan must not report findings with a successful exit status.
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1:print_stacktrace=1"
    export TSAN_OPTIONS="${TSAN_OPTIONS:+$TSAN_OPTIONS:}halt_on_error=1:exitcode=66"
    plugin_run_tests "$BUILD_DIR" Debug
    print_green "ImageControl sanitizer run passed."
}

sanitize_plugin "$@"
