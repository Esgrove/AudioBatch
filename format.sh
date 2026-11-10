#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [cpp|rust|image-control] [OPTIONS]

Format the C++ and Rust projects with their own format.sh scripts, then the ImageControl plugin.
Without a project name, also format the shell scripts in the repository root with shfmt
and the prose outside the projects with slb, when they are installed, and the Python scripts with ruff through uv.
Without a project name, format tracked and non-ignored new JSON files with jq using two-space indentation.
Name a project to format only that one.

The options are passed to the format script of each project.
See cpp/format.sh --help, rust/format.sh --help, and image-control-plugin/format.sh --help for them.

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

if [ "$1" = "image-control" ]; then
    shift
    for argument in "$@"; do
        if [ "$argument" = "-h" ] || [ "$argument" = "--help" ]; then
            "$REPO_ROOT/image-control-plugin/format.sh" --help
            exit 0
        fi
    done
    print_magenta "Running image-control-plugin/format.sh $*"
    "$REPO_ROOT/image-control-plugin/format.sh" "$@"
    exit 0
fi

run_project_scripts format.sh "$@"

if [ "$1" = "cpp" ] || [ "$1" = "rust" ]; then
    exit 0
fi

print_magenta "Running image-control-plugin/format.sh $*"
"$REPO_ROOT/image-control-plugin/format.sh" "$@"

CHECK=false
for argument in "$@"; do
    if [ "$argument" = "-c" ] || [ "$argument" = "--check" ]; then
        CHECK=true
    fi
done

cd "$DIR"

require_command jq "jq not found in path, it formats the JSON files"
if [ "$CHECK" = true ]; then
    print_magenta "Checking JSON formatting..."
else
    print_magenta "Formatting JSON files..."
fi

git ls-files -z --cached --others --exclude-standard -- '*.json' |
    while IFS= read -r -d '' file; do
        temporary=$(mktemp)
        if ! jq --indent 2 . "$file" > "$temporary"; then
            rm -- "$temporary"
            print_error "Failed to parse JSON: $file"
            exit 1
        fi
        if [ "$CHECK" = true ]; then
            printf 'Checking %s\n' "$file"
        else
            printf 'Formatting %s\n' "$file"
        fi
        if ! cmp -s "$temporary" "$file"; then
            if [ "$CHECK" = true ]; then
                rm -- "$temporary"
                print_error "JSON formatting differs: $file"
                exit 1
            fi
            if ! cat "$temporary" > "$file"; then
                rm -- "$temporary"
                print_error "Failed to write formatted JSON: $file"
                exit 1
            fi
            printf '  changed %s\n' "$file"
        fi
        rm -- "$temporary"
    done

if [ -n "$(command -v shfmt)" ]; then
    SHELL_FILES=(./*.sh)
    if [ "$CHECK" = true ]; then
        print_magenta "Checking root shell script formatting..."
        print_file_list Checking "${SHELL_FILES[@]}"
        shfmt --diff "${SHELL_FILES[@]}"
    else
        print_magenta "Formatting root shell scripts..."
        print_file_list Formatting "${SHELL_FILES[@]}"
        shfmt --list --write "${SHELL_FILES[@]}"
    fi
fi

if [ -n "$(command -v slb)" ]; then
    if [ "$CHECK" = true ]; then
        print_magenta "Checking root prose..."
        slb --exclude cpp --exclude rust --exclude image-control-plugin --quiet "$DIR"
    else
        print_magenta "Formatting root prose..."
        slb --exclude cpp --exclude rust --exclude image-control-plugin --fix "$DIR"
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
