#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS] [-- CLANG_TIDY_ARGS...]

Lint C++ code in src/ and tests/ with clang-tidy.

Uses the compile_commands.json from an existing CMake build directory.
If none is found, configures and builds the platform's Ninja debug preset first.
On macOS the script also passes the active SDK path so that Homebrew's
clang-tidy can find platform headers like TargetConditionals.h.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --check
        Exit with code 1 if clang-tidy reports any warnings or errors.
        Intended for CI.

    -C | --changed
        Lint only the sources changed since the last commit, staged or not, including untracked files.

    -s | --staged
        Lint only the sources with staged changes.

    A changed header in --changed or --staged mode also lints the source file of the same name next to it.
    The files are linted as they are in the working tree.

    -v | --verbose
        Display commands being executed.

Arguments after -- are passed to clang-tidy as is.
"

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    CHECK=false
    CLANG_TIDY_ARGS=()
    # Which sources to lint: all, changed, or staged.
    FILE_SELECTION="all"

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -c | --check)
                CHECK=true
                ;;
            -C | --changed)
                FILE_SELECTION="changed"
                ;;
            -s | --staged)
                FILE_SELECTION="staged"
                ;;
            -v | --verbose)
                set -x
                ;;
            --)
                shift
                CLANG_TIDY_ARGS=("$@")
                break
                ;;
            *)
                print_error "Unknown option: $1"
                print_usage_and_exit
                ;;
        esac
        shift
    done
}

# Prints the paths, relative to the C++ project, of the files with changes of the selected kind.
# Deleted files are left out, and a renamed file is listed under its new name.
list_changed_files() {
    if [ "$FILE_SELECTION" = "staged" ]; then
        git diff --cached --name-only --relative --diff-filter=ACMR
    else
        git diff HEAD --name-only --relative --diff-filter=ACMR
        git ls-files --others --exclude-standard
    fi
}

# Turns the changed file paths read from stdin into the absolute paths of the sources to lint.
# A changed header maps to the source file of the same name next to it, when there is one.
changed_sources() {
    while IFS= read -r path; do
        case "$path" in
            src/*.cpp | tests/*.cpp)
                echo "$REPO/$path"
                ;;
            src/*.h | tests/*.h)
                if [ -f "$REPO/${path%.h}.cpp" ]; then
                    echo "$REPO/${path%.h}.cpp"
                fi
                ;;
        esac
    done
}

# Prints the absolute paths of the sources to lint, one per line, sorted and without duplicates.
collect_sources() {
    if [ "$FILE_SELECTION" = "all" ]; then
        find "$REPO/src" "$REPO/tests" -type f -name "*.cpp" | sort
    else
        list_changed_files | changed_sources | sort -u
    fi
}

init_options "$@"

cd "$REPO"

CLANG_TIDY=$(command -v clang-tidy || true)
if [ -z "$CLANG_TIDY" ] && [ "$BASH_PLATFORM" = "mac" ] && command -v brew > /dev/null 2>&1; then
    LLVM_PREFIX=$(brew --prefix llvm 2> /dev/null || true)
    if [ -n "$LLVM_PREFIX" ] && [ -x "$LLVM_PREFIX/bin/clang-tidy" ]; then
        CLANG_TIDY="$LLVM_PREFIX/bin/clang-tidy"
    fi
fi
if [ -z "$CLANG_TIDY" ]; then
    print_error_and_exit "clang-tidy not found. Install LLVM (brew install llvm on macOS) or add its bin directory to PATH."
fi

# Locate a compile_commands.json from one of the known CMake build directories.
# Prefer the debug build that the editor and the CMake debug presets share.
BUILD_DIR=""
for candidate in cmake-build-mac-debug cmake-build-windows-ninja-debug cmake-build-mac-release cmake-build-windows-ninja-release build; do
    if [ -f "$REPO/$candidate/compile_commands.json" ]; then
        BUILD_DIR="$REPO/$candidate"
        break
    fi
done

if [ -z "$BUILD_DIR" ]; then
    case "$BASH_PLATFORM" in
        mac)
            BUILD_PRESET="macos-debug"
            BUILD_DIR="$REPO/cmake-build-mac-debug"
            ;;
        windows)
            BUILD_PRESET="windows-ninja-debug"
            BUILD_DIR="$REPO/cmake-build-windows-ninja-debug"
            ;;
        *)
            print_error_and_exit "Platform is not supported yet"
            ;;
    esac

    print_magenta "No compile database found. Configuring and building $BUILD_PRESET..."
    cmake --preset "$BUILD_PRESET"
    cmake --build --preset "$BUILD_PRESET"

    if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
        print_error_and_exit "Build did not generate $BUILD_DIR/compile_commands.json"
    fi
fi

print_magenta "Using compile database at $BUILD_DIR"
"$CLANG_TIDY" --version | head -n 2

# MSVC compile commands contain flags like /MP that clang does not use,
# which would otherwise be reported as errors.
EXTRA_ARGS=(--extra-arg=-Wno-unused-command-line-argument)
if [ "$BASH_PLATFORM" = "mac" ] && command -v xcrun > /dev/null 2>&1; then
    SDK=$(xcrun --show-sdk-path 2> /dev/null || true)
    if [ -n "$SDK" ]; then
        EXTRA_ARGS+=(--extra-arg=-isysroot --extra-arg="$SDK")
    fi
fi

# Collect the C++ sources under src/ and tests/, including subdirectories, or only the changed ones.
SOURCES=()
while IFS= read -r source; do
    SOURCES+=("$source")
done < <(collect_sources)

if [ "${#SOURCES[@]}" -eq 0 ]; then
    print_green "No $FILE_SELECTION C++ sources to lint"
    exit 0
fi

# Do not let clang-tidy infer flags from an unrelated source when the database is stale.
# The fuzz harnesses are only in the database of the macos-fuzz build, so they are skipped elsewhere.
LINTED_SOURCES=()
for source in "${SOURCES[@]}"; do
    if grep -Fq "\"file\": \"$source\"" "$BUILD_DIR/compile_commands.json"; then
        LINTED_SOURCES+=("$source")
    elif [[ "$source" == "$REPO/tests/fuzz/"* ]]; then
        print_yellow "Skipping $source, which only the macos-fuzz preset builds"
    else
        print_error_and_exit "Compile database has no entry for $source. Reconfigure and build CMake first, for example: cmake --preset macos-debug && cmake --build --preset macos-debug"
    fi
done
if [ "${#LINTED_SOURCES[@]}" -eq 0 ]; then
    print_green "No C++ sources in this compile database to lint"
    exit 0
fi
SOURCES=("${LINTED_SOURCES[@]}")

print_magenta "Running clang-tidy on ${#SOURCES[@]} source files..."

if [ "$CHECK" = false ]; then
    "$CLANG_TIDY" -p "$BUILD_DIR" --quiet "${EXTRA_ARGS[@]}" "${SOURCES[@]}" "${CLANG_TIDY_ARGS[@]}"
    exit
fi

OUTPUT_FILE=$(mktemp)
trap 'rm -f "$OUTPUT_FILE"' EXIT

set +e
"$CLANG_TIDY" -p "$BUILD_DIR" --quiet "${EXTRA_ARGS[@]}" "${SOURCES[@]}" "${CLANG_TIDY_ARGS[@]}" 2>&1 | tee "$OUTPUT_FILE"
CLANG_TIDY_STATUS=${PIPESTATUS[0]}
set -e

# Headers are linted once per including source file, so count each finding only once.
ISSUE_COUNT=$({ grep -E "(^|: )(warning|error): " "$OUTPUT_FILE" || true; } | sort -u | wc -l | tr -d ' ')

if [ "$ISSUE_COUNT" -gt 0 ]; then
    print_error_and_exit "clang-tidy reported $ISSUE_COUNT issue(s)"
fi

if [ "$CLANG_TIDY_STATUS" -ne 0 ]; then
    print_error_and_exit "clang-tidy failed with exit code $CLANG_TIDY_STATUS"
fi

print_green "No clang-tidy issues found"
