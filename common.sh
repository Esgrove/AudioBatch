#!/bin/bash
set -eo pipefail

REPO=$(git rev-parse --show-toplevel || (cd "$(dirname "${BASH_SOURCE[0]}")" && pwd))
export REPO

# Set Bash platform script is running on (mac/windows/linux).
# BSD (Mac) and GNU (Linux & Git for Windows) core-utils implementations
# have some differences for example in the available command line options.
case "$(uname -s)" in
    "Darwin")
        BASH_PLATFORM="mac"
        ;;
    "MINGW"*)
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

# Print an error message and exit the program
print_error_and_exit() {
    print_red "ERROR: $1"
    # use exit code if given as argument, otherwise default to 1
    exit "${2:-1}"
}

verify_universal_binary() {
    # Verify that a Mac bundle is a Universal binary
    if [ -z "$1" ]; then
        print_error_and_exit "Give path to Mac executable as an argument!"
    fi
    print_magenta "Verifying universal binary: $1"
    file --brief "$1"
    local architectures
    architectures=$(lipo -archs "$1")
    if ! echo "$architectures" | grep -q "x86_64 arm64"; then
        print_error_and_exit "Expected Universal Binary, got '$architectures' instead"
    fi
}

# Enable calling functions directly as arguments:
# ./common.sh verify_universal_binary
# shellcheck disable=SC2128
if [ $# -gt 0 ] && [ "$0" = "$BASH_SOURCE" ]; then
    "$@"
fi
