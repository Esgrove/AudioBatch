#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [cpp|rust] [OPTIONS]

Build the release binaries of the C++ and Rust projects with their own build.sh scripts.
Name a project to build only that one.

The options are passed to the build script of each project.
See cpp/build.sh --help and rust/build.sh --help for them.
Building both projects accepts only the options both scripts have, such as --verbose.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.
        After a project name, display the instructions of that project's script.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

run_project_scripts build.sh "$@"
