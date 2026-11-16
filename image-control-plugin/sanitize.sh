#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=image-control-plugin/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [--thread | --realtime] [--verbose]

Build and run ImageControl's unit tests on macOS under AddressSanitizer and UndefinedBehaviorSanitizer.
Use --thread for a separate ThreadSanitizer build.
All sanitizer builds use RelWithDebInfo for optimizations and debug symbols.
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
    PRESET="macos-asan"
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help) print_usage_and_exit 0 ;;
            -t | --thread)
                if [ "$SANITIZERS" = realtime ]; then
                    print_error_and_exit "--thread and --realtime require separate builds."
                fi
                SANITIZERS="thread"
                PRESET="macos-tsan"
                ;;
            -R | --realtime)
                if [ "$SANITIZERS" = thread ]; then
                    print_error_and_exit "--thread and --realtime require separate builds."
                fi
                SANITIZERS="realtime"
                PRESET="macos-rtsan"
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
        IMAGE_CONTROL_LLVM_PREFIX=$(brew --prefix llvm)
        export IMAGE_CONTROL_LLVM_PREFIX
        if [ ! -x "$IMAGE_CONTROL_LLVM_PREFIX/bin/clang++" ] || [ ! -x "$IMAGE_CONTROL_LLVM_PREFIX/bin/clang" ]; then
            print_error_and_exit "RealtimeSanitizer requires Homebrew LLVM. Install it with brew install llvm."
        fi
    fi

    cd "$REPO"
    cmake --preset "$PRESET"
    cmake --build --preset "$PRESET"
    ctest --preset "$PRESET"

    if [ "$SANITIZERS" = realtime ]; then
        print_green "ImageControl real-time probes completed. Review BLOCKED known-regression reports separately."
    else
        print_green "ImageControl sanitizer run passed."
    fi
}

sanitize_plugin "$@"
