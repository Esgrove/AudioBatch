#!/bin/bash
set -eo pipefail

# Format C++ code using ClangFormat

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

cd "$REPO"

if [ -z "$(command -v clang-format)" ]; then
    print_error_and_exit "clang-format not found in path"
fi

echo "Formatting C++ files..."
clang-format -i --verbose --style=file "$REPO"/Source/*.cpp "$REPO"/Source/*.h

if [ -n "$(command -v shfmt)" ]; then
    echo "Formatting shell scripts..."
    shfmt --list --write ./*.sh
fi
