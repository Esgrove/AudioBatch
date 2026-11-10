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

PLUGIN_STYLE_FILE="$REPO/.clang-format"
if [ "$BASH_PLATFORM" = windows ]; then
    require_command cygpath "Git Bash with cygpath is required for Windows formatting."
    # MSYS does not convert paths embedded after the "file:" style prefix.
    PLUGIN_STYLE_FILE=$(cygpath -m "$PLUGIN_STYLE_FILE")
fi

# Collect the C++ sources and headers under src/ and tests/, including subdirectories,
# and those of the test plugin that the tests load.
# The test plugin formats with the explicit style file instead of relying on its configuration symlink.
# Paths are relative to the repository root, so that the clang-format output is short and free of "..".
CPP_FILES=()
PLUGIN_CPP_FILES=()
while IFS= read -r file; do
    if [[ "$file" == test-plugin/* ]]; then
        PLUGIN_CPP_FILES+=("$file")
    else
        CPP_FILES+=("$file")
    fi
done < <(cd "$REPO_ROOT" && find cpp/src cpp/tests test-plugin/src -type f \( -name "*.cpp" -o -name "*.h" \) | sort)

if [ "$CHECK" = true ]; then
    print_magenta "Checking C++ formatting..."
    (cd "$REPO_ROOT" && clang-format --verbose --style=file --dry-run --Werror "${CPP_FILES[@]}")
    (cd "$REPO_ROOT" && clang-format --verbose --style="file:$PLUGIN_STYLE_FILE" --dry-run --Werror "${PLUGIN_CPP_FILES[@]}")
else
    print_magenta "Formatting C++ files..."
    (cd "$REPO_ROOT" && clang-format -i --verbose --style=file "${CPP_FILES[@]}")
    (cd "$REPO_ROOT" && clang-format -i --verbose --style="file:$PLUGIN_STYLE_FILE" "${PLUGIN_CPP_FILES[@]}")
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
    CMAKE_FILES=(CMakeLists.txt cmake/*.cmake ../test-plugin/CMakeLists.txt)
    if [ "$CHECK" = true ]; then
        print_magenta "Checking CMake formatting..."
        print_file_list Checking "${CMAKE_FILES[@]}"
        "${CMAKE_FORMAT[@]}" --config-files "$REPO/.cmake-format.yaml" --check "${CMAKE_FILES[@]}"
    else
        print_magenta "Formatting CMake files..."
        print_file_list Formatting "${CMAKE_FILES[@]}"
        "${CMAKE_FORMAT[@]}" --config-files "$REPO/.cmake-format.yaml" --in-place "${CMAKE_FILES[@]}"
    fi
fi

if [ -n "$(command -v shfmt)" ]; then
    SHELL_FILES=(./*.sh ../common.sh ../fixtures/*.sh ../test-plugin/*.sh)
    if [ "$CHECK" = true ]; then
        print_magenta "Checking shell script formatting..."
        print_file_list Checking "${SHELL_FILES[@]}"
        shfmt --diff "${SHELL_FILES[@]}"
    else
        print_magenta "Formatting shell scripts..."
        print_file_list Formatting "${SHELL_FILES[@]}"
        shfmt --list --write "${SHELL_FILES[@]}"
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
