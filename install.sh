#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [cpp|rust|image-control] [OPTIONS]

Install the CLIs, GUI apps, and shell completions of the C++ and Rust projects with their own install.sh scripts.
The ImageControl plugin is installed with image-control-plugin/build.sh --install, after the other projects.
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
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

if [ "$1" = "image-control" ]; then
    shift
    if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
        "$REPO_ROOT/image-control-plugin/build.sh" --help
        exit 0
    fi
else
    run_project_scripts install.sh "$@"
    if [ "$1" = "cpp" ] || [ "$1" = "rust" ]; then
        exit 0
    fi
fi

print_magenta "Running image-control-plugin/build.sh --install"
"$REPO_ROOT/image-control-plugin/build.sh" --install
