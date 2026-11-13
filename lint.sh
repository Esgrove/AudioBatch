#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [cpp|rust|image-control] [OPTIONS]

Lint the C++ and Rust projects with their own lint.sh scripts, then the ImageControl plugin.
C++ and the plugin run clang-tidy, and a missing or out of date compile database is configured and built first.
Rust needs no separate build step, since cargo clippy builds what it checks.
Rust runs cargo clippy with --all-targets, applying fixes with --fix --allow-dirty and denying all warnings.
Name a project to lint only that one.

The options are passed to the lint script of each project.
See cpp/lint.sh --help, rust/lint.sh --help, and image-control-plugin/lint.sh --help for them.
Linting all projects accepts only the options all the scripts have, such as --check and --verbose.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.
        After a project name, display the instructions of that project's script.

    -c | --check
        Report warnings without applying fixes, and exit with code 1 if any project has them.
        Intended for CI.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    parse_project_arguments lint.sh true "$@"
}

# Run selected projects sequentially, preserving the first failed lint command's status.
lint_projects() {
    parse_arguments "$@"
    run_project_scripts lint.sh "${PROJECT_ARGUMENTS[@]}"
    if [ "$PROJECT_SCOPE" = cpp ] || [ "$PROJECT_SCOPE" = rust ]; then
        return
    fi
    print_magenta "Running image-control-plugin/lint.sh ${PROJECT_ARGUMENTS[*]}"
    "$REPO_ROOT/image-control-plugin/lint.sh" "${PROJECT_ARGUMENTS[@]}"
}

lint_projects "$@"
