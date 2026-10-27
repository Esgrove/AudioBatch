#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [cpp|rust] [OPTIONS]

Format the C++ and Rust projects with their own format.sh scripts.
Without a project name, also format the shell scripts in the repository root with shfmt
and the prose outside the projects with slb, when they are installed, and the Python scripts with ruff through uv.
Name a project to format only that one.

The options are passed to the format script of each project.
See cpp/format.sh --help and rust/format.sh --help for them.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.
        After a project name, display the instructions of that project's script.

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

run_project_scripts format.sh "$@"

if [ "$1" = "cpp" ] || [ "$1" = "rust" ]; then
    exit 0
fi

CHECK=false
for argument in "$@"; do
    if [ "$argument" = "-c" ] || [ "$argument" = "--check" ]; then
        CHECK=true
    fi
done

cd "$DIR"

if [ -n "$(command -v shfmt)" ]; then
    if [ "$CHECK" = true ]; then
        print_magenta "Checking root shell script formatting..."
        shfmt --diff ./*.sh
    else
        print_magenta "Formatting root shell scripts..."
        shfmt --list --write ./*.sh
    fi
fi

if [ -n "$(command -v slb)" ]; then
    if [ "$CHECK" = true ]; then
        print_magenta "Checking root prose..."
        slb --exclude cpp --exclude rust --quiet "$DIR"
    else
        print_magenta "Formatting root prose..."
        slb --exclude cpp --exclude rust --fix "$DIR"
    fi
fi

require_command uv "uv not found in path, it runs ruff for the Python scripts"
if [ "$CHECK" = true ]; then
    print_magenta "Checking Python formatting..."
    uv run --frozen ruff format --check
    uv run --frozen ruff check
else
    print_magenta "Formatting Python scripts..."
    uv run --frozen ruff format
    uv run --frozen ruff check --fix
fi
