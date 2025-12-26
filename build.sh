#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

OPTIONS: All options are optional
    --help
        Display these instructions.

    --build-type <type>
        Specify build type for CMake. Default is 'Release'.

    --verbose
        Display commands being executed.
"
export USAGE

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    BUILD_TYPE="Release"

    while [ $# -gt 0 ]; do
        case "$1" in
            --help)
                echo "$USAGE"
                exit 1
                ;;
            --build-type)
                BUILD_TYPE="$2"
                shift
                ;;
            --verbose)
                set -x
                ;;
        esac
        shift
    done

    JUCE_PATH="$REPO/JUCE"
    APP_NAME="AudioBatch"
    CMAKE_BUILD_TARGET="$APP_NAME"
    CMAKE_BUILD_DIR="$REPO/cmake-build-${BASH_PLATFORM}-$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
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
    APP_BUNDLE="$APP_NAME.app"
    rm -rf "${REPO:?}/$APP_BUNDLE"

    if [ -n "$(command -v xcbeautify)" ]; then
        # nicer xcodebuild output: https://github.com/tuist/xcbeautify
        time cmake --build "$CMAKE_BUILD_DIR" --target "$CMAKE_BUILD_TARGET" --config "$BUILD_TYPE" | xcbeautify
    else
        print_yellow "xcbeautify missing, install it with brew"
        time cmake --build "$CMAKE_BUILD_DIR" --target "$CMAKE_BUILD_TARGET" --config "$BUILD_TYPE"
    fi

    APP_EXECUTABLE="$REPO/$APP_BUNDLE/Contents/MacOS/$APP_NAME"
    mv -f "$(find "$CMAKE_BUILD_DIR" -name "$APP_BUNDLE")" "$APP_BUNDLE"
    file "$APP_EXECUTABLE"
    $APP_EXECUTABLE --version
    print_green "Build successful: $APP_BUNDLE"
}

build_windows_app() {
    APP_EXE="$APP_NAME.exe"
    rm -rf "${REPO:?}/$APP_EXE"

    cmake --build "$CMAKE_BUILD_DIR" --target "$CMAKE_BUILD_TARGET" --config "$BUILD_TYPE"

    APP_EXECUTABLE="$REPO/$APP_EXE"
    mv -f "$(find "$CMAKE_BUILD_DIR" -name "$APP_EXE")" "$APP_EXECUTABLE"
    file "$APP_EXECUTABLE"
    $APP_EXECUTABLE --version
    print_green "Build successful: $APP_EXE"
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
    # Export Xcode / VS project from CMake
    cd "$REPO" || print_error_and_exit "Failed to cd to repo root"

    print_magenta "Generating IDE project..."
    echo "Exporting to: $CMAKE_BUILD_DIR"
    if [ "$BASH_PLATFORM" = "windows" ]; then
        if ! cmake -B "$CMAKE_BUILD_DIR" -G "Visual Studio 18 2026" -A x64; then
            rm -rf "$CMAKE_BUILD_DIR"
            cmake -B "$CMAKE_BUILD_DIR" -G "Visual Studio 18 2026" -A x64
        fi
    elif [ "$BASH_PLATFORM" = "mac" ]; then
        if ! cmake -B "$CMAKE_BUILD_DIR" -G "Xcode"; then
            rm -rf "$CMAKE_BUILD_DIR"
            cmake -B "$CMAKE_BUILD_DIR" -G "Xcode"
        fi
    else
        print_error_and_exit "Platform is not supported yet"
    fi
}

init_options "$@"
check_juce_submodule
export_cmake_project
build_app
