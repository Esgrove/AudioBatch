#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [cpp|rust|image-control] [OPTIONS]

Install the CLIs, GUI apps, and shell completions of the C++ and Rust projects with their own install.sh scripts.
The ImageControl plugin is installed with image-control-plugin/install.sh, after the other projects.
Name a project to install only that one.

The options are passed to the install script of each project.
See cpp/install.sh --help and rust/install.sh --help for them.
Installing both projects accepts only the options both scripts have, such as --cli-only and --verbose.
Use -c or --clean to delete the C++ release CMake build directory before rebuilding.
Rust build artifacts are not cleaned. Use -l or --cli-only to install only the CLIs.
The options are not passed to the ImageControl plugin build.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.
        After a project name, display the instructions of that project's script.

    -l | --cli-only
        Install only the CLIs, followed by the ImageControl plugin unless a project is named.

    -c | --clean
        Clean the selected C++ build directory before rebuilding.

    -r | --reuse
        Reuse the existing C++ build directory. Rust accepts this compatibility option without changing its build.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    parse_project_arguments install.sh true "$@"
}

# Dispatch application options separately from the plugin's independent installer interface.
install_projects() {
    parse_arguments "$@"
    run_project_scripts install.sh "${PROJECT_ARGUMENTS[@]}"
    if [ "$PROJECT_SCOPE" = cpp ] || [ "$PROJECT_SCOPE" = rust ]; then
        return
    fi
    print_magenta "Running image-control-plugin/install.sh"
    "$REPO_ROOT/image-control-plugin/install.sh"
}

install_projects "$@"
