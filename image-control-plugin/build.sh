#!/bin/bash
set -eo pipefail

# Builds the ImageControl VST3 plugin, and on macOS its Audio Unit, defaulting to Release mode in image-control-plugin/build.
# Optionally runs its unit tests, installs the bundles into the user's plugin folders,
# and validates the installed Audio Unit with auval.
# Needs the JUCE submodule in cpp/JUCE: git submodule update --init cpp/JUCE

USAGE="Usage: $0 [OPTIONS]

Build the ImageControl VST3 plugin, and on macOS its Audio Unit.

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
        Copy the built bundles into the user's VST3 folder, and on macOS the Audio Unit into the Components folder.

    -v | --validate
        On macOS, install and validate the Audio Unit with auval."

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../common.sh
source "$DIR/../common.sh"

PRINT_ONLY=false
RUN_TESTS=false
INSTALL=false
VALIDATE=false
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
        -v | --validate)
            INSTALL=true
            VALIDATE=true
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
AU_BUNDLE="$BUILD_DIR/ImageControl_artefacts/$BUILD_TYPE/AU/ImageControl.component"

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
if [ "$BASH_PLATFORM" = "mac" ]; then
    TARGETS+=(ImageControl_AU)
fi
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
if [ "$BASH_PLATFORM" = "mac" ]; then
    if [ ! -d "$AU_BUNDLE" ]; then
        print_error_and_exit "Audio Unit not found after the build: $AU_BUNDLE"
    fi
    print_green "Built: $AU_BUNDLE"
fi

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

    if [ "$BASH_PLATFORM" = "mac" ]; then
        COMPONENTS_DIR="$HOME/Library/Audio/Plug-Ins/Components"
        mkdir -p "$COMPONENTS_DIR"
        rm -rf "${COMPONENTS_DIR:?}/ImageControl.component"
        cp -R "$AU_BUNDLE" "$COMPONENTS_DIR/"
        # The Audio Unit registry caches components, so it is restarted to pick up the new build.
        killall -q AudioComponentRegistrar || true
        print_green "Installed: $COMPONENTS_DIR/ImageControl.component"
    fi
fi

if [ "$VALIDATE" = true ] && [ "$BASH_PLATFORM" = "mac" ]; then
    require_command auval
    # The restarted registry takes a moment to list the new component, and auval fails until it does.
    for attempt in $(seq 1 30); do
        if auval -a 2> /dev/null | grep -q "aufx Egic Esgr"; then
            break
        fi
        if [ "$attempt" -eq 30 ]; then
            print_error_and_exit "The Audio Unit did not register: aufx Egic Esgr"
        fi
        sleep 1
    done
    print_magenta "Validating the Audio Unit with auval"
    auval -strict -v aufx Egic Esgr
fi

echo "$BUNDLE"
