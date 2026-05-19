#!/bin/bash
set -eo pipefail

# Lint C++ code in src/ with clang-tidy.
#
# Uses the compile_commands.json from an existing CMake build directory.
# On macOS the script also passes the active SDK path so that Homebrew's
# clang-tidy can find platform headers like TargetConditionals.h.

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

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

EXTRA_ARGS=()
if [ "$BASH_PLATFORM" = "mac" ] && command -v xcrun > /dev/null 2>&1; then
    SDK=$(xcrun --show-sdk-path 2> /dev/null || true)
    if [ -n "$SDK" ]; then
        EXTRA_ARGS+=(--extra-arg=-isysroot --extra-arg="$SDK")
    fi
fi

# Collect the C++ sources under src/.
SOURCES=("$REPO"/src/*.cpp)

print_magenta "Running clang-tidy on ${#SOURCES[@]} source files..."
clang-tidy -p "$BUILD_DIR" --quiet "${EXTRA_ARGS[@]}" "${SOURCES[@]}" "$@"
