#!/bin/bash
set -eo pipefail

# Format C++ code using ClangFormat

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
pushd "$REPO" > /dev/null

if [ -z "$(command -v clang-format)" ]; then
    echo "ERROR: clang-format not found in path"
    exit 1
fi

echo "Formatting C++ files..."
clang-format -i --verbose --style=file "$REPO"/Source/*.cpp "$REPO"/Source/*.h

if [ -n "$(command -v shfmt)" ]; then
    echo "Formatting shell scripts..."
    shfmt --list --write ./*.sh
fi

popd > /dev/null || :
