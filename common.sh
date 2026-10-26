#!/bin/bash
set -eo pipefail

# Shell helpers shared by the C++ and Rust project scripts.
# Each project sources this through its own common.sh, which sets REPO to the project directory
# and adds the helpers specific to that project.

# Set Bash platform script is running on (mac/windows/linux).
# BSD (Mac) and GNU (Linux & Git for Windows) core-utils implementations have some differences
# for example in the available command line options.
case "$(uname -s)" in
    "Darwin")
        BASH_PLATFORM="mac"
        ;;
    "MINGW"*)
        BASH_PLATFORM="windows"
        ;;
    "Windows_NT"*)
        BASH_PLATFORM="windows"
        ;;
    *)
        BASH_PLATFORM="linux"
        ;;
esac
export BASH_PLATFORM

# Print a message with green color
print_green() {
    printf "\e[1;49;32m%s\e[0m\n" "$1"
}

# Print a message with magenta color
print_magenta() {
    printf "\e[1;49;35m%s\e[0m\n" "$1"
}

# Print a message with red color
print_red() {
    printf "\e[1;49;31m%s\e[0m\n" "$1"
}

# Print a message with yellow color
print_yellow() {
    printf "\e[1;49;33m%s\e[0m\n" "$1"
}

# Print an error message to stderr without exiting
print_error() {
    print_red "ERROR: $1" >&2
}

# Print the calling script's USAGE text and exit.
# Use exit code if given as argument, otherwise default to 1.
print_usage_and_exit() {
    echo "$USAGE"
    exit "${1:-1}"
}

# Print an error message and exit the program
print_error_and_exit() {
    print_red "ERROR: $1"
    # use exit code if given as argument, otherwise default to 1
    exit "${2:-1}"
}

# Exit with an error if the command given as the first argument is not found in path.
# The second argument is the error message, which defaults to saying the command was not found.
require_command() {
    if [ -z "$(command -v "$1")" ]; then
        print_error_and_exit "${2:-$1 not found in path}"
    fi
}

# Enable calling functions directly as arguments:
# ./common.sh print_green "hello"
# shellcheck disable=SC2128
if [ $# -gt 0 ] && [ "$0" = "$BASH_SOURCE" ]; then
    "$@"
fi
