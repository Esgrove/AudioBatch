#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Build AudioBatch.
On macOS, Ninja is used by default. Pass --xcode to use the Xcode generator instead.
On Windows, Visual Studio is used by default. Pass --ninja to use Ninja instead.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -n | --ninja
        Use Ninja as the build system (Windows only, macOS uses Ninja by default).

    -x | --xcode
        Use Xcode as the build system (macOS only, default is Ninja).

    -r | --reuse
        Skip CMake generation and reuse the existing build directory.

    -b | --build-type <type>
        Specify build type for CMake. Default is 'Release'.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    BUILD_TYPE="Release"
    USE_NINJA=false
    USE_XCODE=false
    REUSE_BUILD_DIR=false

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                echo "$USAGE"
                exit 1
                ;;
            -n | --ninja)
                USE_NINJA=true
                ;;
            -x | --xcode)
                USE_XCODE=true
                ;;
            -r | --reuse)
                REUSE_BUILD_DIR=true
                ;;
            -b | --build-type)
                BUILD_TYPE="$2"
                shift
                ;;
            -v | --verbose)
                set -x
                ;;
        esac
        shift
    done

    # On Mac, use Ninja by default unless --xcode was specified
    if [ "$BASH_PLATFORM" = "mac" ] && [ "$USE_XCODE" != true ]; then
        USE_NINJA=true
    fi

    JUCE_PATH="$REPO/JUCE"
    APP_NAME="AudioBatch"
    GUI_TARGET_NAME="AudioBatch"
    CLI_TARGET_NAME="AudioBatchCli"
    CLI_BINARY_NAME="audiobatch"
    CMAKE_BUILD_DIR="$REPO/cmake-build-${BASH_PLATFORM}-$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
}

require_path() {
    if [ ! -e "$1" ]; then
        print_error_and_exit "Expected build artifact not found: $1"
    fi
}

build_app() {
    print_magenta "Building $APP_NAME..."
    if [ "$BASH_PLATFORM" = "mac" ]; then
        build_mac_app
    elif [ "$BASH_PLATFORM" = "windows" ]; then
        build_windows_app
    else
        print_error_and_exit "Platform is not supported yet"
    fi
}

build_mac_app() {
    GUI_APP_BUNDLE="AudioBatch.app"
    GUI_APP_DESTINATION="$REPO/$GUI_APP_BUNDLE"
    CLI_APP_DESTINATION="$REPO/$CLI_BINARY_NAME"

    rm -rf "${GUI_APP_DESTINATION:?}"
    rm -f "$CLI_APP_DESTINATION"

    if [ "$USE_XCODE" = true ]; then
        if [ -n "$(command -v xcbeautify)" ]; then
            time cmake --build "$CMAKE_BUILD_DIR" --target "$GUI_TARGET_NAME" "$CLI_TARGET_NAME" --config "$BUILD_TYPE" | xcbeautify
        else
            print_yellow "xcbeautify missing, install it with: brew install xcbeautify"
            time cmake --build "$CMAKE_BUILD_DIR" --target "$GUI_TARGET_NAME" "$CLI_TARGET_NAME" --config "$BUILD_TYPE"
        fi
        GUI_APP_SOURCE="$CMAKE_BUILD_DIR/AudioBatch_artefacts/$BUILD_TYPE/$GUI_APP_BUNDLE"
        CLI_APP_SOURCE="$CMAKE_BUILD_DIR/AudioBatchCli_artefacts/$BUILD_TYPE/$CLI_BINARY_NAME"
    else
        time cmake --build "$CMAKE_BUILD_DIR" --target "$GUI_TARGET_NAME" "$CLI_TARGET_NAME"
        GUI_APP_SOURCE="$CMAKE_BUILD_DIR/AudioBatch_artefacts/$BUILD_TYPE/$GUI_APP_BUNDLE"
        CLI_APP_SOURCE="$CMAKE_BUILD_DIR/AudioBatchCli_artefacts/$BUILD_TYPE/$CLI_BINARY_NAME"
    fi

    require_path "$GUI_APP_SOURCE"
    require_path "$CLI_APP_SOURCE"

    mv -f "$GUI_APP_SOURCE" "$GUI_APP_DESTINATION"
    cp -f "$CLI_APP_SOURCE" "$CLI_APP_DESTINATION"

    print_green "GUI build successful: $GUI_APP_BUNDLE"
    file "$GUI_APP_DESTINATION"

    print_green "CLI build successful: $CLI_BINARY_NAME"
    file "$CLI_APP_DESTINATION"
    "$CLI_APP_DESTINATION" --version
}

build_windows_app() {
    GUI_EXE="AudioBatch.exe"
    CLI_EXE="$CLI_BINARY_NAME.exe"
    GUI_EXECUTABLE_SOURCE="$CMAKE_BUILD_DIR/AudioBatch_artefacts/$BUILD_TYPE/$GUI_EXE"
    CLI_EXECUTABLE_SOURCE="$CMAKE_BUILD_DIR/AudioBatchCli_artefacts/$BUILD_TYPE/$CLI_EXE"
    GUI_EXECUTABLE="$REPO/AudioBatchApp.exe"
    CLI_EXECUTABLE="$REPO/$CLI_EXE"

    rm -f "$GUI_EXECUTABLE" "$CLI_EXECUTABLE"

    cmake --build "$CMAKE_BUILD_DIR" --target "$GUI_TARGET_NAME" "$CLI_TARGET_NAME" --config "$BUILD_TYPE"

    require_path "$GUI_EXECUTABLE_SOURCE"
    require_path "$CLI_EXECUTABLE_SOURCE"

    cp -f "$GUI_EXECUTABLE_SOURCE" "$GUI_EXECUTABLE"
    cp -f "$CLI_EXECUTABLE_SOURCE" "$CLI_EXECUTABLE"

    print_green "GUI build successful: $GUI_EXE"
    file "$GUI_EXECUTABLE"

    print_green "CLI build successful: $CLI_EXE"
    file "$CLI_EXECUTABLE"
    "$CLI_EXECUTABLE" --version
}

check_juce_submodule() {
    # Update JUCE submodule if needed
    print_magenta "Checking JUCE submodule..."
    if [ ! -d "$JUCE_PATH/modules" ]; then
        print_yellow "JUCE directory not found, cloning submodule..."
        git submodule update --init --recursive
    fi
    JUCE_VERSION="$(git submodule status | grep JUCE)"
    CACHED_JUCE_VERSION="$(git submodule status --cached | grep JUCE)"
    if [ "$JUCE_VERSION" != "$CACHED_JUCE_VERSION" ]; then
        print_yellow "JUCE submodule out of date, updating..."
        git submodule update --init --recursive
    else
        echo "up to date"
    fi
}

export_cmake_project() {
    cd "$REPO" || print_error_and_exit "Failed to cd to repo root"

    if [ "$BASH_PLATFORM" = "mac" ]; then
        if [ "$USE_XCODE" = true ]; then
            print_magenta "Generating Xcode project..."
            if ! cmake -B "$CMAKE_BUILD_DIR" -G "Xcode"; then
                rm -rf "$CMAKE_BUILD_DIR"
                cmake -B "$CMAKE_BUILD_DIR" -G "Xcode"
            fi
        else
            print_magenta "Generating Ninja project..."
            if ! cmake -B "$CMAKE_BUILD_DIR" -G Ninja \
                -DCMAKE_C_COMPILER=clang \
                -DCMAKE_CXX_COMPILER=clang++ \
                -DCMAKE_BUILD_TYPE="$BUILD_TYPE"; then
                rm -rf "$CMAKE_BUILD_DIR"
                cmake -B "$CMAKE_BUILD_DIR" -G Ninja \
                    -DCMAKE_C_COMPILER=clang \
                    -DCMAKE_CXX_COMPILER=clang++ \
                    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
            fi
        fi
    elif [ "$BASH_PLATFORM" = "windows" ]; then
        if [ "$USE_NINJA" = true ]; then
            print_magenta "Generating Ninja project..."
            if ! cmake -B "$CMAKE_BUILD_DIR" -G Ninja; then
                rm -rf "$CMAKE_BUILD_DIR"
                cmake -B "$CMAKE_BUILD_DIR" -G Ninja
            fi
        else
            print_magenta "Generating Visual Studio project..."
            if ! cmake -B "$CMAKE_BUILD_DIR" -G "Visual Studio 18 2026" -A x64; then
                rm -rf "$CMAKE_BUILD_DIR"
                cmake -B "$CMAKE_BUILD_DIR" -G "Visual Studio 18 2026" -A x64
            fi
        fi
    else
        print_error_and_exit "Platform is not supported yet"
    fi
}

init_options "$@"

if [ "$REUSE_BUILD_DIR" = true ]; then
    if [ ! -d "$CMAKE_BUILD_DIR" ]; then
        print_error_and_exit "Build directory does not exist: $CMAKE_BUILD_DIR"
    fi
    print_magenta "Reusing existing build directory: $CMAKE_BUILD_DIR"
else
    check_juce_submodule
    export_cmake_project
fi

build_app
