#!/bin/bash
set -eo pipefail

# Lints the ImageControl sources and tests with clang-tidy,
# using the AudioBatch C++ configuration and the compile database of the Debug build in cmake-build-debug/.
# Builds the unit tests and benchmark of the Debug build first, which configures it when it is missing,
# regenerates its compile database when the CMake files changed, and generates the JUCE headers clang-tidy needs.
# On Windows the Ninja workspace is used, since the Visual Studio generators write no compile database.
# The sanitizer-only callback probes use an existing RTSan database, never inferred ordinary build flags.
# Ordinary lint does not configure the optional Homebrew LLVM workspace when it is absent.

USAGE="Usage: $0 [OPTIONS] [-- CLANG_TIDY_ARGS...]

Lint the ImageControl sources and tests with clang-tidy.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -c | --check
        Exit with code 1 when clang-tidy reports any warning.

    -C | --changed
        Lint only the sources changed since the last commit, staged or not, including untracked files.

    -s | --staged
        Lint only the sources with staged changes.

    -p | --build-dir <directory>
        Use this build directory's compile database instead of cmake-build-debug/.

    -j | --jobs <count>
        Run up to this many clang-tidy processes in parallel.
        Defaults to the number of logical CPUs. Use 1 for serial processing.

    A changed header in --changed or --staged mode also lints the source file of the same name next to it.
    The files are linted as they are in the working tree.
    RealtimeTests.cpp uses an existing cmake-build-mac-rtsan/ database, or an explicitly selected RTSan build.
    Without that optional database it is skipped. Run ./sanitize.sh --realtime to prepare it.

    -v | --verbose
        Display commands being executed.

Arguments after -- are passed to clang-tidy as is."

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DIR/.." && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

init_options() {
    CHECK=false
    # Which sources to lint: all, changed, or staged.
    FILE_SELECTION="all"
    REQUESTED_BUILD_DIR=""
    CLANG_TIDY_ARGS=()
    JOBS=$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)

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

    case "$JOBS" in
        "" | *[!0-9]*)
            print_error_and_exit "Job count must be a positive integer."
            ;;
    esac
    JOBS=$((10#$JOBS))
    if [ "$JOBS" -lt 1 ]; then
        print_error_and_exit "Job count must be a positive integer."
    fi

    # Fixes can touch a header shared by several sources, and exports share one destination file.
    local argument
    for argument in "${CLANG_TIDY_ARGS[@]}"; do
        case "$argument" in
            -fix* | --fix* | -export-fixes* | --export-fixes*)
                JOBS=1
                ;;
        esac
    done
}

# Prints the paths, relative to the repository root, of the plugin files with changes of the selected kind.
# Deleted files are left out, and a renamed file is listed under its new name.
list_changed_files() {
    if [ "$FILE_SELECTION" = "staged" ]; then
        git -C "$ROOT" diff --cached --name-only --diff-filter=ACMR -- image-control-plugin
    else
        git -C "$ROOT" diff HEAD --name-only --diff-filter=ACMR -- image-control-plugin
        git -C "$ROOT" ls-files --others --exclude-standard -- image-control-plugin
    fi
}

# Turns the changed file paths read from stdin into the absolute paths of the sources to lint.
# A changed header maps to the source file of the same name next to it, when there is one.
changed_sources() {
    while IFS= read -r path; do
        case "$path" in
            image-control-plugin/src/*.cpp | image-control-plugin/benchmarks/*.cpp | image-control-plugin/tests/*.cpp)
                echo "$ROOT/$path"
                ;;
            image-control-plugin/src/*.h | image-control-plugin/benchmarks/*.h | image-control-plugin/tests/*.h)
                if [ -f "$ROOT/${path%.h}.cpp" ]; then
                    echo "$ROOT/${path%.h}.cpp"
                fi
                ;;
        esac
    done
}

# Prints the absolute paths of the sources to lint, one per line, sorted and without duplicates.
collect_sources() {
    if [ "$FILE_SELECTION" = "all" ]; then
        find "$DIR/src" "$DIR/benchmarks" "$DIR/tests" -type f -name "*.cpp" | sort
    else
        list_changed_files | changed_sources | sort -u
    fi
}

init_options "$@"

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

# The clang-tidy and clang-format configuration files here are symlinks to the C++ project's files.
# When they are plain text files containing a path, clang-tidy fails to parse them.
for config_link in .clang-tidy .clang-format tests/.clang-tidy; do
    if [ ! -L "$DIR/$config_link" ]; then
        print_error "image-control-plugin/$config_link is not a symlink, so clang-tidy cannot read it."
        if [ "$BASH_PLATFORM" = "windows" ]; then
            print_error "Symlinks need Windows Developer Mode (Settings > System > For developers) and git config core.symlinks true."
            print_error "Then check the links out again from the repository root:"
            print_error "  git ls-files -s | awk '\$1 == \"120000\" {print \$4}' | xargs -I{} sh -c 'rm \"{}\" && git checkout -- \"{}\"'"
        else
            print_error "Check it out again with: git checkout -- image-control-plugin/$config_link"
        fi
        exit 1
    fi
done

USE_NINJA=false
if [ "$BASH_PLATFORM" = "windows" ]; then
    USE_NINJA=true
fi
BUILD_DIR="${REQUESTED_BUILD_DIR:-$(plugin_build_dir Debug "$USE_NINJA")}"
require_command cmake
if [ ! -d "$DIR/../cpp/JUCE/modules" ]; then
    print_error_and_exit "JUCE submodule not found, run: git submodule update --init cpp/JUCE"
fi
if [ "$USE_NINJA" = true ]; then
    load_msvc_environment
fi
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    print_magenta "Configuring the Debug build in $BUILD_DIR"
    plugin_configure "$BUILD_DIR" Debug "$USE_NINJA" ON
else
    # Keep the cached generator, compiler, and configuration, but ensure every linted target exists.
    # An ordinary plugin build configures IMAGE_CONTROL_BUILD_TESTS=OFF in this same workspace.
    print_magenta "Enabling the tests and benchmark in $BUILD_DIR"
    cmake -S "$DIR" -B "$BUILD_DIR" -DIMAGE_CONTROL_BUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
fi
# The build is incremental, so it does little when the database and headers are current.
# Ninja reruns CMake first when a CMake file is newer than the generated build files.
print_magenta "Updating the compile database and generated headers"
cmake --build "$BUILD_DIR" --target ImageControlTests ImageControlBenchmark ImageControlPresetWriter
if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    print_error_and_exit "$BUILD_DIR has no compile_commands.json, which needs a Ninja build directory."
fi

# Accept only a configured RTSan workspace with an explicit entry for the guarded callback probes.
# Never infer ordinary flags for this file or weaken its compile-time instrumentation requirements.
has_realtime_compile_database() {
    local directory="$1"
    [ -f "$directory/CMakeCache.txt" ] && [ -f "$directory/compile_commands.json" ] &&
        grep -Fqx 'IMAGE_CONTROL_SANITIZE:STRING=realtime' "$directory/CMakeCache.txt" &&
        grep -Fq "\"file\": \"$DIR/tests/RealtimeTests.cpp\"" "$directory/compile_commands.json"
}

# The configuration files are passed explicitly, and the test configuration inherits the project configuration and relaxes it for test code.
# clang-tidy prints its diagnostics to stdout and fails with an error, such as a compile error,
# which fails the run even without --check.
# Ordinary sources missing from the database retain the existing neighbouring-flag inference.
PROJECT_SOURCES=()
TEST_SOURCES=()
REALTIME_SOURCES=()
REALTIME_BUILD_DIR=""
if has_realtime_compile_database "$BUILD_DIR"; then
    REALTIME_BUILD_DIR="$BUILD_DIR"
elif [ "$BASH_PLATFORM" = mac ] && has_realtime_compile_database "$DIR/cmake-build-mac-rtsan"; then
    REALTIME_BUILD_DIR="$DIR/cmake-build-mac-rtsan"
fi
while IFS= read -r source; do
    case "$source" in
        "$DIR/tests/RealtimeTests.cpp")
            if [ -n "$REALTIME_BUILD_DIR" ]; then
                REALTIME_SOURCES+=("$source")
            else
                print_yellow "Skipping RTSan-only RealtimeTests.cpp: no instrumented database. Prepare it with ./sanitize.sh --realtime."
            fi
            ;;
        "$DIR/tests/"*)
            TEST_SOURCES+=("$source")
            ;;
        *)
            PROJECT_SOURCES+=("$source")
            ;;
    esac
done < <(collect_sources)

if [ "${#PROJECT_SOURCES[@]}" -eq 0 ] && [ "${#TEST_SOURCES[@]}" -eq 0 ] && [ "${#REALTIME_SOURCES[@]}" -eq 0 ]; then
    print_green "No $FILE_SELECTION C++ sources to lint (0 warnings)"
    exit 0
fi

OUTPUT=$(mktemp)
ERRORS=$(mktemp)
trap 'rm -f "$OUTPUT" "$ERRORS"' EXIT

# Lints the sources given after their compilation database and configuration file, up to JOBS at a time.
run_clang_tidy() {
    local database="$1"
    local config="$2"
    shift 2
    if [ $# -eq 0 ]; then
        return 0
    fi
    printf '%s\0' "$@" |
        xargs -0 -P "$JOBS" -n 1 "$CLANG_TIDY" -p "$database" --quiet --config-file="$config" \
            --extra-arg=-Wno-unused-command-line-argument "${CLANG_TIDY_ARGS[@]}" >> "$OUTPUT" 2>> "$ERRORS"
}

print_magenta "Running clang-tidy on $((${#PROJECT_SOURCES[@]} + ${#TEST_SOURCES[@]} + ${#REALTIME_SOURCES[@]})) source files with up to $JOBS parallel jobs..."
TIDY_STATUS=0
run_clang_tidy "$BUILD_DIR" "$ROOT/cpp/.clang-tidy" "${PROJECT_SOURCES[@]}" || TIDY_STATUS=1
run_clang_tidy "$BUILD_DIR" "$ROOT/cpp/tests/.clang-tidy" "${TEST_SOURCES[@]}" || TIDY_STATUS=1
if [ "${#REALTIME_SOURCES[@]}" -gt 0 ]; then
    print_magenta "Linting RTSan-only callback probes with $REALTIME_BUILD_DIR"
    run_clang_tidy "$REALTIME_BUILD_DIR" "$ROOT/cpp/tests/.clang-tidy" "${REALTIME_SOURCES[@]}" || TIDY_STATUS=1
fi

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
