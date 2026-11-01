#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS] [-- CLANG_TIDY_ARGS...]

Lint C++ code in src/, tests/, and ../test-plugin/src/ with clang-tidy.

Uses the compile_commands.json from an existing CMake build directory.
If the selected database is missing, configures and builds its Ninja workspace first.
On Windows this loads the Visual Studio developer environment when cl is not on PATH.
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

    -f | --fuzz
        Lint only the fuzz harnesses in tests/fuzz/, with the compile database of the macos-fuzz preset,
        which is the only build that has them.

    -p | --build-dir <directory>
        Use this build directory's compile database instead of auto-detecting one.

    --clang
        Select the Windows clang-cl workspace. Ignored on macOS.

    -b | --build-type <type>
        Select a build configuration, such as Debug, Release, RelWithDebInfo, or MinSizeRel.
        Defaults to Debug when creating a workspace. Existing explicit directories retain their cached configuration.

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
    FUZZ=false
    REQUESTED_BUILD_DIR=""
    USE_CLANG=false
    BUILD_TYPE="Debug"
    BUILD_TYPE_EXPLICIT=false

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
            -f | --fuzz)
                FUZZ=true
                ;;
            -v | --verbose)
                set -x
                ;;
            -p | --build-dir)
                if [ $# -lt 2 ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                REQUESTED_BUILD_DIR="$2"
                shift
                ;;
            --clang)
                if [ "$BASH_PLATFORM" = "windows" ]; then
                    USE_CLANG=true
                fi
                ;;
            -b | --build-type)
                if [ $# -lt 2 ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                BUILD_TYPE="$2"
                BUILD_TYPE_EXPLICIT=true
                shift
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
    if [ "$FUZZ" = true ] && [ -n "$REQUESTED_BUILD_DIR" ]; then
        print_error_and_exit "--fuzz and --build-dir cannot be used together."
    fi
    if [ "$FUZZ" = true ] && { [ "$USE_CLANG" = true ] || [ "$BUILD_TYPE_EXPLICIT" = true ]; }; then
        print_error_and_exit "--fuzz selects its own compiler and configuration."
    fi
    case "$BUILD_TYPE" in
        "" | *[!a-zA-Z0-9_-]*)
            print_error_and_exit "Build type must contain only letters, numbers, underscores, or hyphens."
            ;;
    esac
}

# Prints the paths, relative to the repository root,
# of the C++ project and test plugin files with changes of the selected kind.
# Deleted files are left out, and a renamed file is listed under its new name.
list_changed_files() {
    if [ "$FILE_SELECTION" = "staged" ]; then
        git -C "$ROOT" diff --cached --name-only --diff-filter=ACMR -- cpp test-plugin
    else
        git -C "$ROOT" diff HEAD --name-only --diff-filter=ACMR -- cpp test-plugin
        git -C "$ROOT" ls-files --others --exclude-standard -- cpp test-plugin
    fi
}

# Turns the changed file paths read from stdin into the absolute paths of the sources to lint.
# A changed header maps to the source file of the same name next to it, when there is one.
changed_sources() {
    while IFS= read -r path; do
        case "$path" in
            cpp/src/*.cpp | cpp/tests/*.cpp | test-plugin/src/*.cpp)
                echo "$ROOT/$path"
                ;;
            cpp/src/*.h | cpp/tests/*.h | test-plugin/src/*.h)
                if [ -f "$ROOT/${path%.h}.cpp" ]; then
                    echo "$ROOT/${path%.h}.cpp"
                fi
                ;;
        esac
    done
}

# Prints the absolute paths of the sources to lint, one per line, sorted and without duplicates.
collect_sources() {
    if [ "$FUZZ" = true ]; then
        find "$REPO/tests/fuzz" -type f -name "*.cpp" | sort
    elif [ "$FILE_SELECTION" = "all" ]; then
        find "$REPO/src" "$REPO/tests" "$ROOT/test-plugin/src" -type f -name "*.cpp" | sort
    else
        list_changed_files | changed_sources | sort -u
    fi
}

# Returns success when the compile database has an entry for the source.
# On Windows the database holds native paths, with forward or escaped backward slashes,
# while the sources are collected as Git Bash paths, so both native forms are checked there.
in_compile_database() {
    local database="$BUILD_DIR/compile_commands.json"
    if grep -Fq "\"file\": \"$1\"" "$database"; then
        return 0
    fi
    if [ "$BASH_PLATFORM" != "windows" ] || ! command -v cygpath > /dev/null 2>&1; then
        return 1
    fi
    local mixed
    mixed=$(cygpath -m "$1")
    local escaped="${mixed//\//\\\\}"
    grep -Fiq "\"file\": \"$mixed\"" "$database" || grep -Fiq "\"file\": \"$escaped\"" "$database"
}

init_options "$@"

cd "$REPO"
ROOT=$(cd "$REPO/.." && pwd)

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
# Applies the shared configuration directly to the test plugin, including Windows symlink-stub checkouts.
run_clang_tidy() {
    local status=0
    if [ "${#PROJECT_SOURCES[@]}" -gt 0 ]; then
        "$CLANG_TIDY" -p "$BUILD_DIR" --quiet "${EXTRA_ARGS[@]}" \
            "${PROJECT_SOURCES[@]}" "${CLANG_TIDY_ARGS[@]}" || status=$?
    fi
    if [ "${#PLUGIN_SOURCES[@]}" -gt 0 ]; then
        local overlay=""
        local plugin_args=()
        if [ "$BASH_PLATFORM" = windows ] && [ ! -L "$ROOT/test-plugin/.clang-format" ]; then
            overlay=$(mktemp)
            printf '{"version":0,"roots":[{"type":"file","name":"%s","external-contents":"%s"}]}\n' \
                "$(cygpath -m "$ROOT/test-plugin/.clang-format")" \
                "$(cygpath -m "$REPO/.clang-format")" > "$overlay"
            plugin_args+=(--vfsoverlay="$overlay")
        fi
        "$CLANG_TIDY" -p "$BUILD_DIR" --quiet "${EXTRA_ARGS[@]}" --config-file="$REPO/.clang-tidy" \
            "${plugin_args[@]}" "${PLUGIN_SOURCES[@]}" "${CLANG_TIDY_ARGS[@]}" || status=$?
        if [ -n "$overlay" ]; then
            rm -f "$overlay"
        fi
    fi
    return "$status"
}

# The debug preset supplies the toolchain even when selecting another single-configuration build type.
case "$BASH_PLATFORM" in
    mac)
        BUILD_PRESET="macos-debug"
        BUILD_PREFIX="cmake-build-mac"
        ;;
    windows)
        if [ "$USE_CLANG" = true ]; then
            BUILD_PRESET="windows-clang-debug"
            BUILD_PREFIX="cmake-build-windows-clang"
        else
            BUILD_PRESET="windows-ninja-debug"
            BUILD_PREFIX="cmake-build-windows-ninja"
        fi
        ;;
    *)
        print_error_and_exit "Platform is not supported yet"
        ;;
esac
BUILD_TYPE_LOWER=$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')

BUILD_DIR=""
if [ -n "$REQUESTED_BUILD_DIR" ]; then
    if [ "$BASH_PLATFORM" = "windows" ]; then
        REQUESTED_BUILD_DIR=$(cygpath -u "$REQUESTED_BUILD_DIR")
    fi
    BUILD_DIR="$REQUESTED_BUILD_DIR"
elif [ "$FUZZ" = true ]; then
    if [ "$BASH_PLATFORM" != "mac" ]; then
        print_error_and_exit "--fuzz is only supported on macOS."
    fi
    BUILD_PRESET="macos-fuzz"
    BUILD_DIR="$REPO/cmake-build-mac-fuzz"
elif [ "$USE_CLANG" = true ] || [ "$BUILD_TYPE_EXPLICIT" = true ]; then
    BUILD_DIR="$REPO/$BUILD_PREFIX-$BUILD_TYPE_LOWER"
else
    for candidate in cmake-build-mac-debug cmake-build-windows-clang-debug cmake-build-windows-ninja-debug cmake-build-mac-release cmake-build-windows-clang-release cmake-build-windows-ninja-release build; do
        if [ -f "$REPO/$candidate/compile_commands.json" ]; then
            BUILD_DIR="$REPO/$candidate"
            break
        fi
    done
fi

if [ -z "$BUILD_DIR" ]; then
    BUILD_DIR="$REPO/$BUILD_PREFIX-$BUILD_TYPE_LOWER"
fi

SELECT_BUILD_TYPE="$BUILD_TYPE_EXPLICIT"
if [ "$USE_CLANG" = true ] && [ -z "$REQUESTED_BUILD_DIR" ]; then
    SELECT_BUILD_TYPE=true
fi

NEEDS_BUILD=false
if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    NEEDS_BUILD=true
fi
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    CACHED_BUILD_TYPE=$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt")
    CACHED_GENERATOR=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")
    if [ "$SELECT_BUILD_TYPE" = true ] && [ "$CACHED_GENERATOR" != "Ninja Multi-Config" ] &&
           [ "$CACHED_BUILD_TYPE" != "$BUILD_TYPE" ]; then
        NEEDS_BUILD=true
    elif [ "$SELECT_BUILD_TYPE" != true ] && [ -n "$CACHED_BUILD_TYPE" ]; then
        BUILD_TYPE="$CACHED_BUILD_TYPE"
    fi
    if [ "$USE_CLANG" = true ] && ! grep -Eiq '^CMAKE_CXX_COMPILER:[A-Z]*=.*clang-cl' "$BUILD_DIR/CMakeCache.txt"; then
        print_error_and_exit "--clang requires a clang-cl workspace. Use a separate build directory from MSVC."
    fi
fi

if [ "$NEEDS_BUILD" = true ]; then
    print_magenta "Configuring and building the compile database workspace at $BUILD_DIR..."
    load_msvc_environment
    CONFIGURE_OPTIONS=(-B "$BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
    if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
        case "$CACHED_GENERATOR" in
            Ninja | "Ninja Multi-Config" | "Unix Makefiles")
                ;;
            *)
                print_error_and_exit "$CACHED_GENERATOR cannot export a compile database. Select a separate Ninja workspace."
                ;;
        esac
        CONFIGURE_OPTIONS+=(-S "$REPO")
        if [ "$SELECT_BUILD_TYPE" = true ] && [ "$CACHED_GENERATOR" != "Ninja Multi-Config" ]; then
            CONFIGURE_OPTIONS+=("-DCMAKE_BUILD_TYPE=$BUILD_TYPE")
        fi
    else
        CONFIGURE_OPTIONS+=(--preset "$BUILD_PRESET")
        if [ "$FUZZ" != true ]; then
            CONFIGURE_OPTIONS+=("-DCMAKE_BUILD_TYPE=$BUILD_TYPE")
        fi
    fi
    if [ "$FUZZ" != true ]; then
        CONFIGURE_OPTIONS+=(-DBUILD_TESTING=ON)
    fi
    cmake "${CONFIGURE_OPTIONS[@]}"
    if [ "$BUILD_TYPE_EXPLICIT" != true ]; then
        CACHED_BUILD_TYPE=$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt")
        if [ -n "$CACHED_BUILD_TYPE" ]; then
            BUILD_TYPE="$CACHED_BUILD_TYPE"
        fi
    fi
    cmake --build "$BUILD_DIR" --config "$BUILD_TYPE"

    if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
        print_error_and_exit "Build did not generate $BUILD_DIR/compile_commands.json"
    fi
fi
BUILD_DIR=$(cd "$BUILD_DIR" && pwd)

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
    if [ "$FUZZ" = true ]; then
        print_error_and_exit "No fuzz harnesses found in $REPO/tests/fuzz"
    fi
    print_green "No $FILE_SELECTION C++ sources to lint"
    exit 0
fi

# Do not let clang-tidy infer flags from an unrelated source when the database is stale.
# The fuzz harnesses are only in the database of the macos-fuzz build, so they are skipped elsewhere,
# and the test plugin only in a build with the tests enabled.
LINTED_SOURCES=()
for source in "${SOURCES[@]}"; do
    if in_compile_database "$source"; then
        LINTED_SOURCES+=("$source")
    elif [[ "$source" == "$REPO/tests/fuzz/"* ]] && [ "$FUZZ" != true ]; then
        print_yellow "Skipping $source, which only the macos-fuzz preset builds"
    elif [[ "$source" == "$ROOT/test-plugin/"* ]]; then
        print_yellow "Skipping $source, which only a build with the tests enabled has"
    else
        print_error_and_exit "Compile database has no entry for $source. Reconfigure and build CMake first, for example: cmake --preset macos-debug && cmake --build --preset macos-debug"
    fi
done
if [ "${#LINTED_SOURCES[@]}" -eq 0 ]; then
    print_green "No C++ sources in this compile database to lint"
    exit 0
fi
SOURCES=("${LINTED_SOURCES[@]}")
PROJECT_SOURCES=()
PLUGIN_SOURCES=()
for source in "${SOURCES[@]}"; do
    if [[ "$source" == "$ROOT/test-plugin/"* ]]; then
        PLUGIN_SOURCES+=("$source")
    else
        PROJECT_SOURCES+=("$source")
    fi
done

print_magenta "Running clang-tidy on ${#SOURCES[@]} source files..."

if [ "$CHECK" = false ]; then
    run_clang_tidy
    exit
fi

OUTPUT_FILE=$(mktemp)
trap 'rm -f "$OUTPUT_FILE"' EXIT

set +e
run_clang_tidy 2>&1 | tee "$OUTPUT_FILE"
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
