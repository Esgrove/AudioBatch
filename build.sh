#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [cpp|rust] [OPTIONS]

Build the binaries of the C++ and Rust projects with their own build.sh scripts. Defaults to release mode.
Name a project to build only that one.

The options are passed to the build script of each project.
See cpp/build.sh --help and rust/build.sh --help for them.
Building both projects accepts only the options both scripts have, such as --verbose.
Use -c or --clean to delete the selected C++ CMake build directory before configuring.
Rust build artifacts are not cleaned. Use -l or --cli-only to build only the CLIs.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.
        After a project name, display the instructions of that project's script.

    -d | --debug
        Build debug binaries in both projects, or in the named project.

    -l | --cli-only
        Build only the CLIs.

    -c | --clean
        Clean the selected C++ build directory before rebuilding.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

run_project_scripts build.sh "$@"
