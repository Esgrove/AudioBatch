#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Format the ImageControl plugin: C++ code with ClangFormat, CMake files with cmake-format,
shell scripts with shfmt and prose with slb when they are installed, and Python scripts with ruff through uv.
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

# The style file is passed explicitly instead of relying on the configuration symlink.
STYLE_FILE="$REPO/.clang-format"
if [ "$BASH_PLATFORM" = windows ]; then
    require_command cygpath "Git Bash with cygpath is required for Windows formatting."
    # MSYS does not convert paths embedded after the "file:" style prefix.
    STYLE_FILE=$(cygpath -m "$STYLE_FILE")
fi

# Paths are relative to the plugin directory, so that the clang-format output is short.
CPP_FILES=()
while IFS= read -r file; do
    CPP_FILES+=("$file")
done < <(find src tests benchmarks -type f \( -name "*.cpp" -o -name "*.h" \) | sort)

if [ "$CHECK" = true ]; then
    print_magenta "Checking C++ formatting..."
    clang-format --verbose --style="file:$STYLE_FILE" --dry-run --Werror "${CPP_FILES[@]}"
else
    print_magenta "Formatting C++ files..."
    clang-format -i --verbose --style="file:$STYLE_FILE" "${CPP_FILES[@]}"
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
    CMAKE_FILES=(CMakeLists.txt cmake/*.cmake)
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
    SHELL_FILES=(./*.sh)
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

# ruff runs from the repository root, so it reads the shared pyproject.toml settings and lock file.
require_command uv "uv not found in path, it runs ruff for the Python scripts"
if [ "$CHECK" = true ]; then
    print_magenta "Checking Python formatting..."
    (cd "$REPO_ROOT" && uv run --frozen ruff format --check image-control-plugin)
    (cd "$REPO_ROOT" && uv run --frozen ruff check image-control-plugin)
else
    print_magenta "Formatting Python scripts..."
    (cd "$REPO_ROOT" && uv run --frozen ruff format image-control-plugin)
    (cd "$REPO_ROOT" && uv run --frozen ruff check --fix image-control-plugin)
fi

if [ -n "$(command -v slb)" ]; then
    if [ "$CHECK" = true ]; then
        print_magenta "Checking prose..."
        slb --quiet "$REPO"
    else
        print_magenta "Formatting prose..."
        slb --fix "$REPO"
    fi
fi
