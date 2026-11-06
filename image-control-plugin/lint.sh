#!/bin/bash
set -eo pipefail

# Lints the ImageControl sources and tests with clang-tidy,
# using the AudioBatch C++ configuration and the compile database of the Debug build in build-debug/.
# Configures the Debug build with the tests first when it has no compile database.

USAGE="Usage: $0 [OPTIONS]

Lint the ImageControl sources and tests with clang-tidy.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --check
        Exit with code 1 when clang-tidy reports any warning."

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DIR/.." && pwd)
# shellcheck source=../common.sh
source "$ROOT/common.sh"

CHECK=false
while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help)
            print_usage_and_exit 0
            ;;
        -c | --check)
            CHECK=true
            ;;
        *)
            print_error_and_exit "Unknown option: $1"
            ;;
    esac
    shift
done

CLANG_TIDY=$(command -v clang-tidy || true)
if [ -z "$CLANG_TIDY" ] && [ "$BASH_PLATFORM" = "mac" ] && [ -n "$(command -v brew)" ]; then
    LLVM_PREFIX=$(brew --prefix llvm 2> /dev/null || true)
    if [ -x "$LLVM_PREFIX/bin/clang-tidy" ]; then
        CLANG_TIDY="$LLVM_PREFIX/bin/clang-tidy"
    fi
fi
if [ -z "$CLANG_TIDY" ]; then
    print_error_and_exit "clang-tidy not found. Install LLVM (brew install llvm on macOS) or add its bin directory to PATH."
fi

BUILD_DIR="$DIR/build-debug"
if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    print_magenta "Configuring the Debug build for its compile database"
    GENERATOR_ARGS=()
    if [ -n "$(command -v ninja)" ]; then
        GENERATOR_ARGS=(-G Ninja)
    fi
    cmake -S "$DIR" -B "$BUILD_DIR" "${GENERATOR_ARGS[@]}" -DCMAKE_BUILD_TYPE=Debug -DIMAGE_CONTROL_BUILD_TESTS=ON
fi

# The configuration files are passed explicitly, so linting does not depend on the configuration symlinks.
# The test configuration inherits the project configuration and relaxes it for test code.
# clang-tidy prints its diagnostics to stdout and fails with an error, such as a compile error,
# which fails the run even without --check.
# A source missing from the compile database is linted with flags inferred from its neighbours.
OUTPUT=$(mktemp)
ERRORS=$(mktemp)
trap 'rm -f "$OUTPUT" "$ERRORS"' EXIT
JOBS=$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)
print_magenta "Running clang-tidy on the ImageControl sources and tests..."
TIDY_STATUS=0
find "$DIR/src" -name "*.cpp" -print0 |
    xargs -0 -P "$JOBS" -n 1 "$CLANG_TIDY" -p "$BUILD_DIR" --quiet --config-file="$ROOT/cpp/.clang-tidy" \
        >> "$OUTPUT" 2>> "$ERRORS" || TIDY_STATUS=1
find "$DIR/tests" -name "*.cpp" -print0 |
    xargs -0 -P "$JOBS" -n 1 "$CLANG_TIDY" -p "$BUILD_DIR" --quiet --config-file="$ROOT/cpp/tests/.clang-tidy" \
        >> "$OUTPUT" 2>> "$ERRORS" || TIDY_STATUS=1

if [ "$TIDY_STATUS" -ne 0 ]; then
    cat "$OUTPUT"
    grep -vE "^[0-9]+ warnings? generated\.$" "$ERRORS" || true
    print_error_and_exit "clang-tidy failed on at least one source"
fi

WARNINGS=$(grep -cE "(warning|error):" "$OUTPUT" || true)
if [ "$WARNINGS" -gt 0 ]; then
    cat "$OUTPUT"
    print_yellow "clang-tidy reported $WARNINGS warnings"
    if [ "$CHECK" = true ]; then
        exit 1
    fi
else
    print_green "No clang-tidy warnings"
fi
