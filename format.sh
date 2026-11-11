#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

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

parse_arguments() {
    parse_project_arguments format.sh true "$@"
}

# Format one JSON file without replacing its permissions, and clean up after parse or write failures.
format_json_file() {
    local check="$1" file="$2" temporary
    temporary=$(mktemp)
    if ! jq --binary --indent 2 . "$file" > "$temporary"; then
        rm -- "$temporary"
        print_error_and_exit "Failed to parse JSON: $file"
    fi
    if [ "$check" = true ]; then
        printf 'Checking %s\n' "$file"
    else
        printf 'Formatting %s\n' "$file"
    fi
    if ! cmp -s "$temporary" "$file"; then
        if [ "$check" = true ]; then
            rm -- "$temporary"
            print_error_and_exit "JSON formatting differs: $file"
        fi
        if ! cat "$temporary" > "$file"; then
            rm -- "$temporary"
            print_error_and_exit "Failed to write formatted JSON: $file"
        fi
        printf '  changed %s\n' "$file"
    fi
    rm -- "$temporary"
}

# Include tracked and non-ignored new JSON paths, preserving filenames containing whitespace.
format_json() {
    local check="$1" file
    require_command jq "jq not found in path, it formats the JSON files"
    if [ "$check" = true ]; then
        print_magenta "Checking JSON formatting..."
    else
        print_magenta "Formatting JSON files..."
    fi
    git ls-files -z --cached --others --exclude-standard -- '*.json' |
        while IFS= read -r -d '' file; do
            format_json_file "$check" "$file"
        done
}

# Keep optional root shell formatting independent of project formatter availability.
format_shell() {
    local check="$1"
    local shell_files=(./*.sh)
    if command -v shfmt > /dev/null 2>&1; then
        if [ "$check" = true ]; then
            print_magenta "Checking root shell script formatting..."
            print_file_list Checking "${shell_files[@]}"
            shfmt --diff "${shell_files[@]}"
        else
            print_magenta "Formatting root shell scripts..."
            print_file_list Formatting "${shell_files[@]}"
            shfmt --list --write "${shell_files[@]}"
        fi
    fi
}

# Project formatters own their prose, leaving only shared documentation to this pass.
format_prose() {
    local check="$1"
    if command -v slb > /dev/null 2>&1; then
        if [ "$check" = true ]; then
            print_magenta "Checking root prose..."
            slb --exclude cpp --exclude rust --exclude image-control-plugin --quiet "$REPO_ROOT"
        else
            print_magenta "Formatting root prose..."
            slb --exclude cpp --exclude rust --exclude image-control-plugin --fix "$REPO_ROOT"
        fi
    fi
}

# Use the locked Ruff environment for all first-party Python scripts.
format_python() {
    local check="$1"
    require_command uv "uv not found in path, it runs ruff for the Python scripts"
    if [ "$check" = true ]; then
        print_magenta "Checking Python formatting..."
        uv run --frozen ruff format --check
        uv run --frozen ruff check
    else
        print_magenta "Formatting Python scripts..."
        uv run --frozen ruff format
        uv run --frozen ruff check --fix
    fi
}

# Dispatch project-scoped requests before running the shared repository formatters.
format_projects() {
    parse_arguments "$@"
    run_project_scripts format.sh "${PROJECT_ARGUMENTS[@]}"
    if [ "$PROJECT_SCOPE" = cpp ] || [ "$PROJECT_SCOPE" = rust ]; then
        return
    fi
    print_magenta "Running image-control-plugin/format.sh ${PROJECT_ARGUMENTS[*]}"
    "$REPO_ROOT/image-control-plugin/format.sh" "${PROJECT_ARGUMENTS[@]}"
    if [ "$PROJECT_SCOPE" = image-control ]; then
        return
    fi
    cd "$REPO_ROOT"
    format_json "$PROJECT_CHECK"
    format_shell "$PROJECT_CHECK"
    format_prose "$PROJECT_CHECK"
    format_python "$PROJECT_CHECK"
}

format_projects "$@"
