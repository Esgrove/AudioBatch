#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Format C++ code with ClangFormat, and shell scripts with shfmt when it is installed.

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

if [ -z "$(command -v clang-format)" ]; then
    print_error_and_exit "clang-format not found in path"
fi

if [ "$CHECK" = true ]; then
    print_magenta "Checking C++ formatting..."
    clang-format --verbose --style=file --dry-run --Werror "$REPO"/src/*.cpp "$REPO"/src/*.h
else
    print_magenta "Formatting C++ files..."
    clang-format -i --verbose --style=file "$REPO"/src/*.cpp "$REPO"/src/*.h
fi

if [ -n "$(command -v shfmt)" ]; then
    if [ "$CHECK" = true ]; then
        print_magenta "Checking shell script formatting..."
        shfmt --diff ./*.sh
    else
        print_magenta "Formatting shell scripts..."
        shfmt --list --write ./*.sh
    fi
fi
