#!/bin/bash
set -eo pipefail

# Builds the ImageControl VST3 plugin, and on macOS its Audio Unit, defaulting to Release mode in image-control-plugin/cmake-build-release.
# Optionally runs unit tests and validates the built VST3 with pluginval. install.sh installs and validates bundles.
# Needs the JUCE submodule in cpp/JUCE: git submodule update --init cpp/JUCE

USAGE="Usage: $0 [OPTIONS]

Build the ImageControl VST3 plugin, and on macOS its Audio Unit.
On Windows, Visual Studio is used by default. Pass --ninja to use Ninja instead.
Release builds go into cmake-build-release/, Debug into cmake-build-debug/,
and other build types into cmake-build-<type>/. The Windows Ninja workspace uses cmake-build-ninja-<type>/.
Use install.sh to install the bundles into the plugin folders.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -p | --print
        Only print the path of the VST3 bundle without building.

    -n | --ninja
        Use Ninja as the build system (Windows only, macOS uses Ninja by default).

    -r | --reuse
        Skip CMake generation and reuse the existing build directory.

    -c | --clean
        Delete the selected build directory before configuring.
        Cannot be used with --reuse.

    -b | --build-type <type>
        Specify build type for CMake. Default is 'Release'.

    -d | --debug
        Select Debug, equivalent to --build-type Debug.
        When combined with --build-type, the last selection wins.

    -t | --test
        Also build and run the unit tests.

    -V | --validate
        Validate the built VST3 with pluginval, without installing the plugin.
        Use pluginval from PATH, or fetch and build the pinned version in ../.deps/ if missing.
        Audio Unit validation with auval requires install.sh --validate.

    --validation-level <5-10>
        Set pluginval strictness and enable validation. Default is 5.

    -v | --verbose
        Display commands being executed."

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    PRINT_ONLY=false
    USE_NINJA=false
    REUSE_BUILD_DIR=false
    CLEAN_BUILD_DIR=false
    RUN_TESTS=false
    VALIDATE=false
    VALIDATION_LEVEL=5
    BUILD_TYPE="Release"

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -p | --print)
                PRINT_ONLY=true
                ;;
            -n | --ninja)
                if [ "$BASH_PLATFORM" = "windows" ]; then
                    USE_NINJA=true
                fi
                ;;
            -r | --reuse)
                REUSE_BUILD_DIR=true
                ;;
            -c | --clean)
                CLEAN_BUILD_DIR=true
                ;;
            -b | --build-type)
                if [ $# -lt 2 ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                BUILD_TYPE="$2"
                shift
                ;;
            -d | --debug)
                BUILD_TYPE="Debug"
                ;;
            -t | --test)
                RUN_TESTS=true
                ;;
            -V | --validate)
                VALIDATE=true
                ;;
            --validation-level)
                if [ $# -lt 2 ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                VALIDATION_LEVEL="$2"
                VALIDATE=true
                shift
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

    plugin_check_validation_level "$VALIDATION_LEVEL"
    if [ "$CLEAN_BUILD_DIR" = true ] && [ "$REUSE_BUILD_DIR" = true ]; then
        print_error_and_exit "--clean and --reuse cannot be used together."
    fi
    case "$BUILD_TYPE" in
        "" | *[!a-zA-Z0-9_-]*)
            print_error_and_exit "Build type must contain only letters, numbers, underscores, or hyphens."
            ;;
    esac

    BUILD_DIR=$(plugin_build_dir "$BUILD_TYPE" "$USE_NINJA")
    BUNDLE="$BUILD_DIR/ImageControl_artefacts/$BUILD_TYPE/VST3/ImageControl.vst3"
    AU_BUNDLE="$BUILD_DIR/ImageControl_artefacts/$BUILD_TYPE/AU/ImageControl.component"
}

init_options "$@"

if [ "$PRINT_ONLY" = true ]; then
    echo "$BUNDLE"
    exit 0
fi

require_command cmake
if [ ! -f "$DIR/../cpp/JUCE/CMakeLists.txt" ]; then
    print_error_and_exit "JUCE submodule not found, run: git submodule update --init cpp/JUCE"
fi

TARGETS=(--target ImageControl_VST3)
if [ "$BASH_PLATFORM" = "mac" ]; then
    TARGETS+=(ImageControl_AUSign)
fi
if [ "$RUN_TESTS" = true ]; then
    TARGETS+=(ImageControlTests)
fi

if [ "$CLEAN_BUILD_DIR" = true ]; then
    print_yellow "Deleting $BUILD_DIR"
    rm -rf "${BUILD_DIR:?}"
fi

print_magenta "Building ImageControl into $BUILD_DIR"
if [ "$REUSE_BUILD_DIR" = true ]; then
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        print_error_and_exit "No CMake build directory to reuse at $BUILD_DIR"
    fi
else
    BUILD_TESTS=OFF
    if [ "$RUN_TESTS" = true ]; then
        BUILD_TESTS=ON
    fi
    plugin_configure "$BUILD_DIR" "$BUILD_TYPE" "$USE_NINJA" "$BUILD_TESTS"
fi
if [ "$USE_NINJA" = true ]; then
    load_msvc_environment
fi
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" "${TARGETS[@]}"

if [ ! -d "$BUNDLE" ]; then
    print_error_and_exit "Bundle not found after the build: $BUNDLE"
fi
print_green "Built: $BUNDLE"
if [ "$BASH_PLATFORM" = "mac" ]; then
    if [ ! -d "$AU_BUNDLE" ]; then
        print_error_and_exit "Audio Unit not found after the build: $AU_BUNDLE"
    fi
    print_green "Built: $AU_BUNDLE"
fi

if [ "$RUN_TESTS" = true ]; then
    print_magenta "Running the unit tests"
    plugin_run_tests "$BUILD_DIR" "$BUILD_TYPE"
fi

if [ "$VALIDATE" = true ]; then
    plugin_validate_vst3 "$BUNDLE" "$VALIDATION_LEVEL"
fi

echo "$BUNDLE"
