#!/bin/bash
set -eo pipefail

# The ImageControl project root, which is the directory of this script.
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export REPO

# shellcheck source=../common.sh
source "$REPO/../common.sh"

# Prints the CMake build directory for a build type.
# The first argument is the build type, and the second is "true" for the Windows Ninja workspace,
# which cannot share a directory with the default Visual Studio one.
plugin_build_dir() {
    local build_type_lower
    build_type_lower=$(echo "$1" | tr '[:upper:]' '[:lower:]')
    if [ "$2" = true ]; then
        echo "$REPO/cmake-build-ninja-$build_type_lower"
    else
        echo "$REPO/cmake-build-$build_type_lower"
    fi
}

# Configures a build directory with CMake.
# The arguments are the directory, the build type, "true" for the Windows Ninja workspace,
# and "ON" or "OFF" for building the unit tests and benchmark.
# Ninja is used on macOS when installed, and on Windows only when requested, with the MSVC environment loaded.
plugin_configure() {
    local build_dir="$1"
    local build_type="$2"
    local use_ninja="$3"
    local build_tests="$4"
    shift 4

    local generator_args=()
    local launcher_args=()
    if [ "$BASH_PLATFORM" = "windows" ]; then
        if [ "$use_ninja" = true ]; then
            load_msvc_environment
            generator_args=(-G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl)
            if [ -n "$(command -v ccache)" ]; then
                launcher_args=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
            fi
        fi
    else
        if [ -n "$(command -v ninja)" ]; then
            generator_args=(-G Ninja)
        fi
        if [ -n "$(command -v ccache)" ]; then
            launcher_args=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
        fi
    fi

    cmake -S "$REPO" -B "$build_dir" "${generator_args[@]}" "${launcher_args[@]}" \
        -DCMAKE_BUILD_TYPE="$build_type" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DIMAGE_CONTROL_BUILD_TESTS="$build_tests" "$@"
}

# Runs the plugin's unit suites with bounded discovery and execution, failing when no test is registered.
plugin_run_tests() {
    ctest --test-dir "$1" --build-config "$2" --output-on-failure --no-tests=error --timeout 180
}
