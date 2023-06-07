#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

OPTIONS: All options are optional
    --help
        Display these instructions.

    --build-type <type>
        Specify build type for CMake. Default is 'Release'.

    --verbose
        Display commands being executed.
"

init_options() {
    BUILD_TYPE="Release"

    while [ $# -gt 0 ]; do
        case "$1" in
            --help)
                echo "$USAGE"
                exit 1
                ;;
            --build-type)
                BUILD_TYPE="$2"
                shift
                ;;
            --verbose)
                set -x
                ;;
        esac
        shift
    done
}

init_options "$@"
