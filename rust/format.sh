#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Format Rust code with rustfmt, shell scripts with shfmt, and prose with slb, when shfmt and slb are installed.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

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

CHECK=false

while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help)
            print_usage_and_exit 0
            ;;
        -c | --check)
            CHECK=true
            ;;
        -v | --verbose)
            set -x
            ;;
        *)
            print_error "Unknown option: $1"
            print_usage_and_exit
            ;;
    esac
    shift
done

require_command cargo "Cargo not found in path. Maybe install rustup?"

cd "$REPO"

if [ "$CHECK" = true ]; then
    print_magenta "Checking Rust formatting..."
    cargo fmt --all --check
else
    print_magenta "Formatting Rust files..."
    cargo fmt --all
fi

if [ -n "$(command -v shfmt)" ]; then
    if [ "$CHECK" = true ]; then
        print_magenta "Checking shell script formatting..."
        shfmt --diff ./*.sh
    else
        print_magenta "Formatting shell scripts..."
        shfmt --list --write ./*.sh
    fi
fi

if [ -n "$(command -v slb)" ]; then
    if [ "$CHECK" = true ]; then
        print_magenta "Checking prose..."
        slb --quiet "$REPO"
    else
        print_magenta "Formatting prose..."
        slb --fix "$REPO"
    fi
fi
