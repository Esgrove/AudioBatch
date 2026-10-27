#!/bin/bash
set -eo pipefail

# Builds the AudioBatch Test MS VST3 plugin in Release mode into test-plugin/build,
# and prints the path of the bundle for the AUDIOBATCH_TEST_VST3 variable that the Rust tests read.
# The C++ tests build their own copy as part of the C++ project.
# Needs the JUCE submodule in cpp/JUCE: git submodule update --init cpp/JUCE

USAGE="Usage: $0 [OPTIONS]

Build the AudioBatch Test MS VST3 plugin used by the tests.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -p | --print
        Only print the path of the bundle without building."

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../common.sh
source "$DIR/../common.sh"

PRINT_ONLY=false
while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help)
            print_usage_and_exit 0
            ;;
        -p | --print)
            PRINT_ONLY=true
            ;;
        *)
            print_error_and_exit "Unknown option: $1"
            ;;
    esac
    shift
done

BUILD_DIR="$DIR/build"
BUNDLE="$BUILD_DIR/AudioBatchTestPlugin_artefacts/Release/VST3/AudioBatch Test MS.vst3"

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
cmake -S "$DIR" -B "$BUILD_DIR" "${GENERATOR_ARGS[@]}" "${LAUNCHER_ARGS[@]}" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --config Release --target AudioBatchTestPlugin_VST3

if [ ! -d "$BUNDLE" ]; then
    print_error_and_exit "Bundle not found after the build: $BUNDLE"
fi
print_green "Built: $BUNDLE"
echo "$BUNDLE"
