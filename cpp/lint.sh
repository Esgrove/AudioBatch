#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS] [-- CLANG_TIDY_ARGS...]

Lint C++ code in src/, tests/, and ../test-plugin/src/ with clang-tidy.
Progress and diagnostics report project paths relative to the repository root.
Every run ends with warning and error totals, counting repeated header findings only once.

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

    -P | --prepare
        Reconfigure the selected Ninja workspace and generate the headers needed by clang-tidy.
        Avoids compiling the applications. With --fuzz, builds the harnesses instead.

    -j | --jobs <count>
        Run up to this many clang-tidy processes in parallel. Defaults to 8.
        Use 1 for serial processing. Diagnostics are grouped by source file.
        Fixes and shared export files always use serial processing to avoid concurrent writes.

    -k | --clang
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

# Help takes precedence over parsing and validation, but not over downstream arguments after --.
for argument in "$@"; do
    case "$argument" in
        --) break ;;
        -h | --help) print_usage_and_exit 0 ;;
    esac
done

init_options() {
    CHECK=false
    PREPARE=false
    CLANG_TIDY_ARGS=()
    # Which sources to lint: all, changed, or staged.
    FILE_SELECTION="all"
    FUZZ=false
    REQUESTED_BUILD_DIR=""
    USE_CLANG=false
    BUILD_TYPE="Debug"
    BUILD_TYPE_EXPLICIT=false
    JOBS=8

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
            -P | --prepare)
                PREPARE=true
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
            -j | --jobs)
                if [ $# -lt 2 ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                JOBS="$2"
                shift
                ;;
            -k | --clang)
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
    case "$JOBS" in
        "" | *[!0-9]*)
            print_error_and_exit "Job count must be a positive integer."
            ;;
    esac
    if ! [ "$JOBS" -gt 0 ] 2> /dev/null; then
        print_error_and_exit "Job count must be a positive integer."
    fi
    JOBS=$((10#$JOBS))

    # Fixes can touch a header shared by several sources, and exports share one destination file.
    # Keep the original batched invocation for these arguments instead of running concurrent writers.
    local argument
    for argument in "${CLANG_TIDY_ARGS[@]}"; do
        case "$argument" in
            -fix* | --fix* | -export-fixes* | --export-fixes*)
                JOBS=1
                ;;
        esac
    done
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

# Returns success when the compile database is older than the CMake files that generate it.
database_is_older_than_cmake_files() {
    local newer
    newer=$(find "$REPO/CMakeLists.txt" "$REPO/cmake" "$ROOT/test-plugin/CMakeLists.txt" \
        -newer "$BUILD_DIR/compile_commands.json" -print -quit 2> /dev/null || true)
    [ -n "$newer" ]
}

# Returns success when a source to lint, other than a fuzz harness outside the fuzz build,
# has no entry in the compile database, such as a file added since the last CMake configuration.
database_lacks_sources() {
    local source
    while IFS= read -r source; do
        if [[ "$source" == "$REPO/tests/fuzz/"* ]] && [ "$FUZZ" != true ]; then
            continue
        fi
        # RTSan's attribute and runtime checks intentionally reject ordinary compiler flags.
        # Its harness is linted only with an exact entry in the dedicated sanitizer database.
        if [ "$source" = "$REPO/tests/RealtimeTests.cpp" ] && ! in_compile_database "$source"; then
            continue
        fi
        if ! in_compile_database "$source"; then
            return 0
        fi
    done < <(collect_sources)
    return 1
}

init_options "$@"

cd "$REPO"
ROOT=$(cd "$REPO/.." && pwd)
REPORT_ROOT_PREFIXES=("$ROOT/")
if [ "$BASH_PLATFORM" = "windows" ] && command -v cygpath > /dev/null 2>&1; then
    REPORT_ROOT_PREFIXES+=("$(cygpath -m "$ROOT")/" "$(cygpath -w "$ROOT")\\")
fi

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

# Replaces a worker subshell with clang-tidy so its recorded PID can also be cancelled directly.
# Project sources discover their own configuration, including the inherited test exceptions.
# The test plugin uses the shared configuration and the optional Windows formatting overlay.
run_clang_tidy_sources() {
    local arguments=()
    if [[ "$1" == "$ROOT/test-plugin/"* ]]; then
        arguments=(--config-file="$REPO/.clang-tidy" "${PLUGIN_ARGS[@]}")
    fi
    exec "$CLANG_TIDY" -p "$BUILD_DIR" --quiet "${EXTRA_ARGS[@]}" \
        "${arguments[@]}" "$@" "${CLANG_TIDY_ARGS[@]}"
}

# Stops and reaps any remaining workers before removing their logs and the shared formatting overlay.
cleanup_lint() {
    local process
    for process in "${WORKER_PIDS[@]}"; do
        kill "$process" 2> /dev/null || true
    done
    for process in "${WORKER_PIDS[@]}"; do
        wait "$process" 2> /dev/null || true
    done
    rm -rf "$WORK_DIR"
}

# Rewrites literal repository prefixes in diagnostics without changing the paths passed to clang-tidy.
# External files keep their absolute paths, and Windows logs can contain either native separator style.
report_relative_paths() {
    local line prefix
    while IFS= read -r line || [ -n "$line" ]; do
        for prefix in "${REPORT_ROOT_PREFIXES[@]}"; do
            line=${line//"$prefix"/}
        done
        printf '%s\n' "$line"
    done
}

# Waits for a worker and emits its complete log without interleaving another source's diagnostics.
# Keeps a nonzero status even if later workers succeed.
finish_worker() {
    local index="$1"
    wait "${WORKER_PIDS[index]}" || CLANG_TIDY_STATUS=$?
    unset 'WORKER_PIDS[index]'
    report_relative_paths < "$WORK_DIR/$index.log" | tee -a "$OUTPUT_FILE"
}

# Keeps at most JOBS workers outstanding, using Bash 3.2-compatible waits on macOS.
# Each invocation still analyzes every compile command for its source, not just the first target.
run_clang_tidy() {
    local index=0
    local oldest=0
    local source
    for source in "${SOURCES[@]}"; do
        if [ $((index - oldest)) -ge "$JOBS" ]; then
            finish_worker "$oldest"
            oldest=$((oldest + 1))
        fi
        print_magenta "[$((index + 1))/${#SOURCES[@]}] Linting ${source#"$ROOT/"}"
        run_clang_tidy_sources "$source" > "$WORK_DIR/$index.log" 2>&1 &
        WORKER_PIDS[index]=$!
        index=$((index + 1))
    done
    while [ "$oldest" -lt "$index" ]; do
        finish_worker "$oldest"
        oldest=$((oldest + 1))
    done
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
elif database_is_older_than_cmake_files; then
    print_yellow "The compile database is older than the CMake files, rebuilding it"
    NEEDS_BUILD=true
elif database_lacks_sources; then
    print_yellow "The compile database is missing sources to lint, rebuilding it"
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

if [ "$NEEDS_BUILD" = true ] || [ "$PREPARE" = true ]; then
    print_magenta "Preparing the compile database workspace at $BUILD_DIR..."
    load_msvc_environment
    CONFIGURE_OPTIONS=(-B "$BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
    if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
        case "$CACHED_GENERATOR" in
            Ninja | "Ninja Multi-Config" | "Unix Makefiles")
                if [ "$PREPARE" = true ] && [ "$CACHED_GENERATOR" != Ninja ]; then
                    print_error_and_exit "--prepare requires a single-configuration Ninja workspace."
                fi
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
    if [ "$PREPARE" = true ] && [ "$FUZZ" != true ]; then
        # These generated headers are enough to analyze every target without compiling the applications.
        cmake --build "$BUILD_DIR" --target \
            AudioBatch_artefacts/JuceLibraryCode/JuceHeader.h \
            AudioBatchCli_artefacts/JuceLibraryCode/JuceHeader.h \
            AudioBatchTests_artefacts/JuceLibraryCode/JuceHeader.h \
            AudioBatchIntegrationTests_artefacts/JuceLibraryCode/JuceHeader.h \
            juce_binarydata_Assets/JuceLibraryCode/BinaryData.h
    else
        cmake --build "$BUILD_DIR" --config "$BUILD_TYPE"
    fi

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
    print_green "No $FILE_SELECTION C++ sources to lint (0 warnings, 0 errors)"
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
        print_yellow "Skipping ${source#"$ROOT/"}, which only the macos-fuzz preset builds"
    elif [ "$source" = "$REPO/tests/RealtimeTests.cpp" ]; then
        print_yellow "Skipping ${source#"$ROOT/"}, which needs the macos-rtsan database (--build-dir cmake-build-mac-rtsan)"
    elif [[ "$source" == "$ROOT/test-plugin/"* ]]; then
        print_yellow "Skipping ${source#"$ROOT/"}, which only a build with the tests enabled has"
    else
        print_error_and_exit "Compile database has no entry for ${source#"$ROOT/"}. Reconfigure and build CMake first, for example: cmake --preset macos-debug && cmake --build --preset macos-debug"
    fi
done
if [ "${#LINTED_SOURCES[@]}" -eq 0 ]; then
    print_green "No C++ sources in this compile database to lint (0 warnings, 0 errors)"
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

WORK_DIR=$(mktemp -d)
WORKER_PIDS=()
trap cleanup_lint EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
OUTPUT_FILE="$WORK_DIR/output.log"
CLANG_TIDY_STATUS=0
PLUGIN_ARGS=()
if [ "${#PLUGIN_SOURCES[@]}" -gt 0 ] && [ "$BASH_PLATFORM" = windows ] &&
    [ ! -L "$ROOT/test-plugin/.clang-format" ]; then
    OVERLAY="$WORK_DIR/plugin-overlay.json"
    printf '{"version":0,"roots":[{"type":"file","name":"%s","external-contents":"%s"}]}\n' \
        "$(cygpath -m "$ROOT/test-plugin/.clang-format")" \
        "$(cygpath -m "$REPO/.clang-format")" > "$OVERLAY"
    PLUGIN_ARGS+=(--vfsoverlay="$OVERLAY")
fi

print_magenta "Running clang-tidy on ${#SOURCES[@]} source files with up to $JOBS parallel jobs..."

if [ "$JOBS" -eq 1 ]; then
    # Retain batched invocations for fixes and exports, which must share a single clang-tidy process.
    index=0
    for group in project plugin; do
        if [ "$group" = project ]; then
            GROUP_SOURCES=("${PROJECT_SOURCES[@]}")
        else
            GROUP_SOURCES=("${PLUGIN_SOURCES[@]}")
        fi
        if [ "${#GROUP_SOURCES[@]}" -gt 0 ]; then
            run_clang_tidy_sources "${GROUP_SOURCES[@]}" > "$WORK_DIR/$index.log" 2>&1 &
            WORKER_PIDS[index]=$!
            finish_worker "$index"
            index=$((index + 1))
        fi
    done
else
    run_clang_tidy
fi

# Headers are linted once per including source file, so count each finding only once.
WARNING_COUNT=$({ grep -E "(^|: )warning: " "$OUTPUT_FILE" || true; } | sort -u | wc -l | tr -d ' ')
ERROR_COUNT=$({ grep -E "(^|: )error: " "$OUTPUT_FILE" || true; } | sort -u | wc -l | tr -d ' ')
ISSUE_COUNT=$((WARNING_COUNT + ERROR_COUNT))

if [ "$ISSUE_COUNT" -gt 0 ]; then
    print_yellow "clang-tidy reported $WARNING_COUNT warning(s) and $ERROR_COUNT error(s)"
else
    print_green "clang-tidy reported 0 warnings and 0 errors"
fi

if [ "$CHECK" = false ]; then
    exit "$CLANG_TIDY_STATUS"
fi

if [ "$ISSUE_COUNT" -gt 0 ]; then
    exit 1
fi

if [ "$CLANG_TIDY_STATUS" -ne 0 ]; then
    print_error_and_exit "clang-tidy failed with exit code $CLANG_TIDY_STATUS"
fi
