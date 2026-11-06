#!/bin/bash
set -eo pipefail

# Builds the ImageControl VST3 plugin, defaulting to Release mode in image-control-plugin/build.
# Optionally runs its unit tests and installs the bundle into the user's VST3 folder.
# Needs the JUCE submodule in cpp/JUCE: git submodule update --init cpp/JUCE

USAGE="Usage: $0 [OPTIONS]

Build the ImageControl VST3 plugin.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -p | --print
        Only print the path of the bundle without building.

    -d | --debug
        Build Debug into build-debug/ instead of Release into build/.

    -t | --test
        Also build and run the unit tests.

    -i | --install
        Copy the built bundle into the user's VST3 folder."

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../common.sh
source "$DIR/../common.sh"

PRINT_ONLY=false
RUN_TESTS=false
INSTALL=false
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
        -t | --test)
            RUN_TESTS=true
            ;;
        -i | --install)
            INSTALL=true
            ;;
        *)
            print_error_and_exit "Unknown option: $1"
            ;;
    esac
    shift
done

BUILD_DIR="$DIR/build"
if [ "$BUILD_TYPE" = "Debug" ]; then
    BUILD_DIR="$DIR/build-debug"
fi
BUNDLE="$BUILD_DIR/ImageControl_artefacts/$BUILD_TYPE/VST3/ImageControl.vst3"

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

TARGETS=(--target ImageControl_VST3)
if [ "$RUN_TESTS" = true ]; then
    TARGETS+=(ImageControlTests)
fi

print_magenta "Building ImageControl into $BUILD_DIR"
cmake -S "$DIR" -B "$BUILD_DIR" "${GENERATOR_ARGS[@]}" "${LAUNCHER_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DIMAGE_CONTROL_BUILD_TESTS="$RUN_TESTS"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" "${TARGETS[@]}"

if [ ! -d "$BUNDLE" ]; then
    print_error_and_exit "Bundle not found after the build: $BUNDLE"
fi
print_green "Built: $BUNDLE"

if [ "$RUN_TESTS" = true ]; then
    print_magenta "Running the unit tests"
    ctest --test-dir "$BUILD_DIR" --build-config "$BUILD_TYPE" --output-on-failure
fi

if [ "$INSTALL" = true ]; then
    if [ "$BASH_PLATFORM" = "windows" ]; then
        VST3_DIR="$(cygpath -u "$LOCALAPPDATA")/Programs/Common/VST3"
    else
        VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
    fi
    mkdir -p "$VST3_DIR"
    rm -rf "${VST3_DIR:?}/ImageControl.vst3"
    cp -R "$BUNDLE" "$VST3_DIR/"
    print_green "Installed: $VST3_DIR/ImageControl.vst3"
fi

echo "$BUNDLE"
