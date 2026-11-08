#!/bin/bash
set -eo pipefail

# Builds the AudioBatch Test MS VST3 plugin, defaulting to Release mode in test-plugin/cmake-build-release,
# and prints the path of the bundle for the AUDIOBATCH_TEST_VST3 variable that the Rust tests read.
# The C++ tests build their own copy as part of the C++ project.
# Needs the JUCE submodule in cpp/JUCE: git submodule update --init cpp/JUCE

USAGE="Usage: $0 [OPTIONS]

Build the AudioBatch Test MS VST3 plugin used by the tests.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -p | --print
        Only print the path of the bundle without building.

    -d | --debug
        Build Debug into cmake-build-debug/ instead of Release into cmake-build-release/."

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../common.sh
source "$DIR/../common.sh"

PRINT_ONLY=false
BUILD_TYPE="Release"
while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help)
            print_usage_and_exit 0
            ;;
        -p | --print)
            PRINT_ONLY=true
            ;;
        -d | --debug)
            BUILD_TYPE="Debug"
            ;;
        *)
            print_error_and_exit "Unknown option: $1"
            ;;
    esac
    shift
done

BUILD_DIR="$DIR/cmake-build-release"
if [ "$BUILD_TYPE" = "Debug" ]; then
    BUILD_DIR="$DIR/cmake-build-debug"
fi
BUNDLE="$BUILD_DIR/AudioBatchTestPlugin_artefacts/$BUILD_TYPE/VST3/AudioBatch Test MS.vst3"

if [ "$PRINT_ONLY" = true ]; then
    echo "$BUNDLE"
    exit 0
fi

require_command cmake
if [ ! -f "$DIR/../cpp/JUCE/CMakeLists.txt" ]; then
    print_error_and_exit "JUCE submodule not found, run: git submodule update --init cpp/JUCE"
fi

GENERATOR_ARGS=()
if [ "$BASH_PLATFORM" != "windows" ] && [ -n "$(command -v ninja)" ]; then
    GENERATOR_ARGS=(-G Ninja)
fi

LAUNCHER_ARGS=()
if [ "$BASH_PLATFORM" != "windows" ] && [ -n "$(command -v ccache)" ]; then
    LAUNCHER_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

print_magenta "Building the test plugin into $BUILD_DIR"
cmake -S "$DIR" -B "$BUILD_DIR" "${GENERATOR_ARGS[@]}" "${LAUNCHER_ARGS[@]}" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --target AudioBatchTestPlugin_VST3

if [ ! -d "$BUNDLE" ]; then
    print_error_and_exit "Bundle not found after the build: $BUNDLE"
fi
print_green "Built: $BUNDLE"
echo "$BUNDLE"
