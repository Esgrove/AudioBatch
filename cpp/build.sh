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

    -o | --open
        Generate and open the project in the IDE instead of building or running tests.
        Selects Xcode on macOS and Visual Studio on Windows. Cannot be used with --ninja.

    -r | --reuse
        Skip CMake generation and reuse the existing build directory.

    -b | --build-type <type>
        Specify build type for CMake. Default is 'Release'.

    -c | --cli-only
        Build only the CLI, without the GUI app.

    -t | --test
        Also build and run the tests.

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
    OPEN_PROJECT=false
    REUSE_BUILD_DIR=false
    RUN_TESTS=false
    CLI_ONLY=false

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -n | --ninja)
                USE_NINJA=true
                ;;
            -x | --xcode)
                USE_XCODE=true
                ;;
            -o | --open)
                OPEN_PROJECT=true
                ;;
            -r | --reuse)
                REUSE_BUILD_DIR=true
                ;;
            -b | --build-type)
                BUILD_TYPE="$2"
                shift
                ;;
            -c | --cli-only)
                CLI_ONLY=true
                ;;
            -t | --test)
                RUN_TESTS=true
                ;;
            -v | --verbose)
                set -x
                ;;
            *)
                print_error "Unknown option: $1"
                print_usage_and_exit
                ;;
        esac
        shift
    done

    if [ "$USE_XCODE" = true ] && [ "$BASH_PLATFORM" != "mac" ]; then
        print_error_and_exit "--xcode is only supported on macOS."
    fi
    if [ "$USE_XCODE" = true ] && [ "$USE_NINJA" = true ]; then
        print_error_and_exit "--xcode and --ninja cannot be used together."
    fi
    if [ "$OPEN_PROJECT" = true ]; then
        if [ "$USE_NINJA" = true ]; then
            print_error_and_exit "--open requires an IDE project and cannot be used with --ninja."
        fi
        if [ "$BASH_PLATFORM" = "mac" ]; then
            USE_XCODE=true
        fi
    fi

    # On Mac, use Ninja by default unless --xcode was specified
    if [ "$BASH_PLATFORM" = "mac" ] && [ "$USE_XCODE" != true ]; then
        USE_NINJA=true
    fi

    JUCE_PATH="$REPO/JUCE"
    APP_NAME="AudioBatch"
    GUI_TARGET_NAME="AudioBatch"
    CLI_TARGET_NAME="AudioBatchCli"
    CLI_BINARY_NAME="audiobatch"
    BUILD_TARGETS=("$GUI_TARGET_NAME" "$CLI_TARGET_NAME")
    if [ "$CLI_ONLY" = true ]; then
        BUILD_TARGETS=("$CLI_TARGET_NAME")
    fi

    # Use a separate build directory per generator so switching between Ninja and Xcode
    # or Visual Studio does not require wiping the other configuration's build directory.
    BUILD_TYPE_LOWER=$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')
    if [ "$BASH_PLATFORM" = "mac" ]; then
        if [ "$USE_XCODE" = true ]; then
            CMAKE_BUILD_DIR="$REPO/cmake-build-mac-xcode-${BUILD_TYPE_LOWER}"
        else
            CMAKE_BUILD_DIR="$REPO/cmake-build-mac-${BUILD_TYPE_LOWER}"
        fi
    elif [ "$BASH_PLATFORM" = "windows" ]; then
        if [ "$USE_NINJA" = true ]; then
            CMAKE_BUILD_DIR="$REPO/cmake-build-windows-ninja-${BUILD_TYPE_LOWER}"
        else
            CMAKE_BUILD_DIR="$REPO/cmake-build-windows-${BUILD_TYPE_LOWER}"
        fi
    else
        CMAKE_BUILD_DIR="$REPO/cmake-build-${BASH_PLATFORM}-${BUILD_TYPE_LOWER}"
    fi

    # Keep the compilation cache next to the build directory it belongs to,
    # so cleaning a build directory also drops its cache.
    export CCACHE_DIR="$CMAKE_BUILD_DIR/ccache"
}

ccache_zero_stats() {
    if [ -n "$(command -v ccache)" ]; then
        echo "ccache: $(ccache --zero-stats)"
    else
        echo "ccache not installed"
    fi
}

ccache_show_stats() {
    if [ -n "$(command -v ccache)" ]; then
        echo "ccache:"
        ccache --show-stats
    else
        echo "ccache not installed"
    fi
}

require_path() {
    if [ ! -e "$1" ]; then
        print_error_and_exit "Expected build artifact not found: $1"
    fi
}

# Ninja is a single configuration generator, so the build type is fixed when the build directory is configured.
# Another tool configuring the same directory, such as an IDE, can leave it with a different or empty build type,
# which changes both the compiler flags and the folder the artefacts are written to.
ensure_cached_build_type() {
    if [ "$USE_NINJA" != true ]; then
        return
    fi
    local cached_build_type
    cached_build_type=$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$CMAKE_BUILD_DIR/CMakeCache.txt")
    if [ "$cached_build_type" != "$BUILD_TYPE" ]; then
        print_yellow "Build directory has build type '$cached_build_type', reconfiguring it for $BUILD_TYPE..."
        cmake -B "$CMAKE_BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
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

    # A CLI only build keeps the app of an earlier full build.
    if [ "$CLI_ONLY" != true ]; then
        rm -rf "${GUI_APP_DESTINATION:?}"
    fi
    rm -f "$CLI_APP_DESTINATION"

    if [ "$USE_XCODE" = true ]; then
        if [ -n "$(command -v xcbeautify)" ]; then
            time cmake --build "$CMAKE_BUILD_DIR" --target "${BUILD_TARGETS[@]}" --config "$BUILD_TYPE" | xcbeautify
        else
            print_yellow "xcbeautify missing, install it with: brew install xcbeautify"
            time cmake --build "$CMAKE_BUILD_DIR" --target "${BUILD_TARGETS[@]}" --config "$BUILD_TYPE"
        fi
        GUI_APP_SOURCE="$CMAKE_BUILD_DIR/AudioBatch_artefacts/$BUILD_TYPE/$GUI_APP_BUNDLE"
        CLI_APP_SOURCE="$CMAKE_BUILD_DIR/AudioBatchCli_artefacts/$BUILD_TYPE/$CLI_BINARY_NAME"
    else
        ccache_zero_stats
        time cmake --build "$CMAKE_BUILD_DIR" --target "${BUILD_TARGETS[@]}"
        ccache_show_stats
        GUI_APP_SOURCE="$CMAKE_BUILD_DIR/AudioBatch_artefacts/$BUILD_TYPE/$GUI_APP_BUNDLE"
        CLI_APP_SOURCE="$CMAKE_BUILD_DIR/AudioBatchCli_artefacts/$BUILD_TYPE/$CLI_BINARY_NAME"
    fi

    require_path "$CLI_APP_SOURCE"
    cp -f "$CLI_APP_SOURCE" "$CLI_APP_DESTINATION"

    if [ "$CLI_ONLY" != true ]; then
        require_path "$GUI_APP_SOURCE"
        mv -f "$GUI_APP_SOURCE" "$GUI_APP_DESTINATION"
        print_green "GUI build successful: $GUI_APP_BUNDLE"
        file "$GUI_APP_DESTINATION"
    fi

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

    if [ "$CLI_ONLY" != true ]; then
        rm -f "$GUI_EXECUTABLE"
    fi
    rm -f "$CLI_EXECUTABLE"

    if [ "$USE_NINJA" = true ]; then
        ccache_zero_stats
        cmake --build "$CMAKE_BUILD_DIR" --target "${BUILD_TARGETS[@]}" --config "$BUILD_TYPE"
        ccache_show_stats
    else
        cmake --build "$CMAKE_BUILD_DIR" --target "${BUILD_TARGETS[@]}" --config "$BUILD_TYPE"
    fi

    require_path "$CLI_EXECUTABLE_SOURCE"
    cp -f "$CLI_EXECUTABLE_SOURCE" "$CLI_EXECUTABLE"

    if [ "$CLI_ONLY" != true ]; then
        require_path "$GUI_EXECUTABLE_SOURCE"
        cp -f "$GUI_EXECUTABLE_SOURCE" "$GUI_EXECUTABLE"
        print_green "GUI build successful: $GUI_EXE"
        file "$GUI_EXECUTABLE"
    fi

    print_green "CLI build successful: $CLI_EXE"
    file "$CLI_EXECUTABLE"
    "$CLI_EXECUTABLE" --version
}

check_juce_submodule() {
    # Update JUCE submodule if needed.
    # The git commands need to run inside the repository, whatever directory the script was started from.
    cd "$REPO" || print_error_and_exit "Failed to cd to $REPO"
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
            if ! cmake -B "$CMAKE_BUILD_DIR" -G "Xcode" -DBUILD_TESTING="$RUN_TESTS"; then
                rm -rf "$CMAKE_BUILD_DIR"
                cmake -B "$CMAKE_BUILD_DIR" -G "Xcode" -DBUILD_TESTING="$RUN_TESTS"
            fi
        else
            print_magenta "Generating Ninja project..."
            if ! cmake -B "$CMAKE_BUILD_DIR" -G Ninja \
                -DCMAKE_C_COMPILER=clang \
                -DCMAKE_CXX_COMPILER=clang++ \
                -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_TESTING="$RUN_TESTS"; then
                rm -rf "$CMAKE_BUILD_DIR"
                cmake -B "$CMAKE_BUILD_DIR" -G Ninja \
                    -DCMAKE_C_COMPILER=clang \
                    -DCMAKE_CXX_COMPILER=clang++ \
                    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_TESTING="$RUN_TESTS"
            fi
        fi
    elif [ "$BASH_PLATFORM" = "windows" ]; then
        if [ "$USE_NINJA" = true ]; then
            # Like the windows-ninja presets, Ninja uses MSVC, which needs a developer command prompt environment.
            print_magenta "Generating Ninja project..."
            local ninja_options=(
                -G Ninja
                -DCMAKE_C_COMPILER=cl
                -DCMAKE_CXX_COMPILER=cl
                -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
                -DBUILD_TESTING="$RUN_TESTS"
            )
            if ! cmake -B "$CMAKE_BUILD_DIR" "${ninja_options[@]}"; then
                rm -rf "$CMAKE_BUILD_DIR"
                cmake -B "$CMAKE_BUILD_DIR" "${ninja_options[@]}"
            fi
        else
            print_magenta "Generating Visual Studio project..."
            if ! cmake -B "$CMAKE_BUILD_DIR" -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING="$RUN_TESTS"; then
                rm -rf "$CMAKE_BUILD_DIR"
                cmake -B "$CMAKE_BUILD_DIR" -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING="$RUN_TESTS"
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
    if [ "$RUN_TESTS" = true ] && ! grep -Eiq '^BUILD_TESTING:BOOL=(ON|TRUE|1)$' "$CMAKE_BUILD_DIR/CMakeCache.txt"; then
        print_error_and_exit "Tests are disabled in this build directory. Run with --test without --reuse to enable them."
    fi
    print_magenta "Reusing existing build directory: $CMAKE_BUILD_DIR"
    ensure_cached_build_type
else
    check_juce_submodule
    export_cmake_project
fi

if [ "$OPEN_PROJECT" = true ]; then
    print_magenta "Opening project in the IDE..."
    cmake --open "$CMAKE_BUILD_DIR"
    exit 0
fi

build_app

if [ "$RUN_TESTS" = true ]; then
    print_magenta "Building tests..."
    cmake --build "$CMAKE_BUILD_DIR" --target AudioBatchTests --config "$BUILD_TYPE"
    print_magenta "Running tests..."
    ctest --test-dir "$CMAKE_BUILD_DIR" --build-config "$BUILD_TYPE" --output-on-failure --no-tests=error
fi
