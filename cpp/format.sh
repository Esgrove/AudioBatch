#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Format C++ code with ClangFormat, CMake files with cmake-format, and shell scripts with shfmt when it is installed.
cmake-format is run through uvx when it is not installed.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --check
        Check formatting without changing files.
        Exit with code 1 if any file is not formatted.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    CHECK=false

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -c | --check)
                CHECK=true
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
}

init_options "$@"

cd "$REPO"

require_command clang-format

# Collect the C++ sources and headers under src/ and tests/, including subdirectories,
# and those of the test plugin that the tests load.
CPP_FILES=()
while IFS= read -r file; do
    CPP_FILES+=("$file")
done < <(find "$REPO/src" "$REPO/tests" "$REPO/../test-plugin/src" -type f \( -name "*.cpp" -o -name "*.h" \) | sort)

if [ "$CHECK" = true ]; then
    print_magenta "Checking C++ formatting..."
    clang-format --verbose --style=file --dry-run --Werror "${CPP_FILES[@]}"
else
    print_magenta "Formatting C++ files..."
    clang-format -i --verbose --style=file "${CPP_FILES[@]}"
fi

# Use an installed cmake-format, or fetch it on demand with uvx.
# PyYAML is needed to read .cmake-format.yaml.
if [ -n "$(command -v cmake-format)" ]; then
    CMAKE_FORMAT=(cmake-format)
elif [ -n "$(command -v uvx)" ]; then
    # cmakelang's token scanner is incompatible with Python 3.15.
    CMAKE_FORMAT=(uvx --python 3.14 --from cmakelang --with pyyaml cmake-format)
else
    CMAKE_FORMAT=()
    print_yellow "cmake-format and uvx not found, skipping CMake formatting"
fi

if [ ${#CMAKE_FORMAT[@]} -gt 0 ]; then
    if [ "$CHECK" = true ]; then
        print_magenta "Checking CMake formatting..."
        "${CMAKE_FORMAT[@]}" --check CMakeLists.txt cmake/*.cmake ../test-plugin/CMakeLists.txt
    else
        print_magenta "Formatting CMake files..."
        "${CMAKE_FORMAT[@]}" --in-place CMakeLists.txt cmake/*.cmake ../test-plugin/CMakeLists.txt
    fi
fi

if [ -n "$(command -v shfmt)" ]; then
    if [ "$CHECK" = true ]; then
        print_magenta "Checking shell script formatting..."
        shfmt --diff ./*.sh ../common.sh ../fixtures/*.sh ../test-plugin/*.sh
    else
        print_magenta "Formatting shell scripts..."
        shfmt --list --write ./*.sh ../common.sh ../fixtures/*.sh ../test-plugin/*.sh
    fi
fi

if [ -n "$(command -v slb)" ]; then
    if [ "$CHECK" = true ]; then
        print_magenta "Checking prose..."
        slb --exclude JUCE --quiet "$REPO"
    else
        print_magenta "Formatting prose..."
        slb --exclude JUCE --fix "$REPO"
    fi
fi
