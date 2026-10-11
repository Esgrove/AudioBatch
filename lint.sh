#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS] [-- CLANG_TIDY_ARGS...]

Lint C++ code in src/ with clang-tidy.

Uses the compile_commands.json from an existing CMake build directory.
On macOS the script also passes the active SDK path so that Homebrew's
clang-tidy can find platform headers like TargetConditionals.h.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --check
        Exit with code 1 if clang-tidy reports any warnings or errors.
        Intended for CI.

    -v | --verbose
        Display commands being executed.

Arguments after -- are passed to clang-tidy as is.
"

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    CHECK=false
    CLANG_TIDY_ARGS=()

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -c | --check)
                CHECK=true
                ;;
            -v | --verbose)
                set -x
                ;;
            --)
                shift
                CLANG_TIDY_ARGS=("$@")
                break
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

cd "$REPO"

if [ -z "$(command -v clang-tidy)" ]; then
    print_error_and_exit "clang-tidy not found in path"
fi

# Locate a compile_commands.json from one of the known CMake build directories.
# Prefer the debug build that the editor and the CMake debug presets share.
BUILD_DIR=""
for candidate in cmake-build-mac-debug cmake-build-windows-ninja-debug cmake-build-mac-release cmake-build-windows-ninja-release build; do
    if [ -f "$REPO/$candidate/compile_commands.json" ]; then
        BUILD_DIR="$REPO/$candidate"
        break
    fi
done

if [ -z "$BUILD_DIR" ]; then
    print_error_and_exit "No compile_commands.json found. Configure a CMake build first, for example: cmake --preset macos-debug"
fi

print_magenta "Using compile database at $BUILD_DIR"
clang-tidy --version | head -n 2

# MSVC compile commands contain flags like /MP that clang does not use,
# which would otherwise be reported as errors.
EXTRA_ARGS=(--extra-arg=-Wno-unused-command-line-argument)
if [ "$BASH_PLATFORM" = "mac" ] && command -v xcrun > /dev/null 2>&1; then
    SDK=$(xcrun --show-sdk-path 2> /dev/null || true)
    if [ -n "$SDK" ]; then
        EXTRA_ARGS+=(--extra-arg=-isysroot --extra-arg="$SDK")
    fi
fi

# Collect the C++ sources under src/.
SOURCES=("$REPO"/src/*.cpp)

print_magenta "Running clang-tidy on ${#SOURCES[@]} source files..."

if [ "$CHECK" = false ]; then
    clang-tidy -p "$BUILD_DIR" --quiet "${EXTRA_ARGS[@]}" "${SOURCES[@]}" "${CLANG_TIDY_ARGS[@]}"
    exit
fi

OUTPUT_FILE=$(mktemp)
trap 'rm -f "$OUTPUT_FILE"' EXIT

set +e
clang-tidy -p "$BUILD_DIR" --quiet "${EXTRA_ARGS[@]}" "${SOURCES[@]}" "${CLANG_TIDY_ARGS[@]}" 2>&1 | tee "$OUTPUT_FILE"
CLANG_TIDY_STATUS=${PIPESTATUS[0]}
set -e

# Headers are linted once per including source file, so count each finding only once.
ISSUE_COUNT=$({ grep -E "(^|: )(warning|error): " "$OUTPUT_FILE" || true; } | sort -u | wc -l | tr -d ' ')

if [ "$ISSUE_COUNT" -gt 0 ]; then
    print_error_and_exit "clang-tidy reported $ISSUE_COUNT issue(s)"
fi

if [ "$CLANG_TIDY_STATUS" -ne 0 ]; then
    print_error_and_exit "clang-tidy failed with exit code $CLANG_TIDY_STATUS"
fi

print_green "No clang-tidy issues found"
