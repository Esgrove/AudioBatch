#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=cpp/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build the macOS libFuzzer harnesses, replay the committed corpus, and fuzz each harness.
New inputs go to fuzz-scratch/, and crashing inputs go to fuzz-artifacts/.
The committed corpus is never modified. Requires Homebrew LLVM and Ninja.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -B | --build-only
        Configure and build the harnesses without running them.

    -N | --no-build
        Use existing harnesses without configuring or building.
        Cannot be combined with --build-only.

    -r | --replay
        Only replay the committed corpus.

    -f | --fuzz
        Only fuzz, without a separate corpus replay.
        Cannot be combined with --replay.

    -s | --seconds <count>
        Fuzz each harness for this many seconds. Defaults to 60.

    -v | --verbose
        Display commands being executed.
"

# Validates modes and duration before building or modifying the scratch corpus.
parse_arguments() {
    BUILD_ONLY=false
    NO_BUILD=false
    MODE=all
    SECONDS_PER_HARNESS=60
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -B | --build-only)
                BUILD_ONLY=true
                ;;
            -N | --no-build)
                NO_BUILD=true
                ;;
            -r | --replay | -f | --fuzz)
                case "$1" in
                    -r | --replay) SELECTED_MODE=replay ;;
                    -f | --fuzz) SELECTED_MODE=fuzz ;;
                esac
                if [ "$MODE" != all ] && [ "$MODE" != "$SELECTED_MODE" ]; then
                    print_error_and_exit "--replay and --fuzz cannot be used together."
                fi
                MODE="$SELECTED_MODE"
                ;;
            -s | --seconds)
                if [ $# -lt 2 ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                SECONDS_PER_HARNESS="$2"
                shift
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

    if [ "$BUILD_ONLY" = true ] && [ "$NO_BUILD" = true ]; then
        print_error_and_exit "--build-only and --no-build cannot be used together."
    fi
    case "$SECONDS_PER_HARNESS" in
        "" | *[!0-9]*)
            print_error_and_exit "--seconds must be a positive integer."
            ;;
    esac
    if ! [ "$SECONDS_PER_HARNESS" -gt 0 ] 2> /dev/null; then
        print_error_and_exit "--seconds must be a positive integer."
    fi
    if [ "$BASH_PLATFORM" != mac ]; then
        print_error_and_exit "The fuzz harnesses are only supported on macOS."
    fi
}

build_harnesses() {
    cd "$REPO"
    BUILD_DIR="$REPO/cmake-build-mac-fuzz"
    if [ "$NO_BUILD" != true ]; then
        print_magenta "Building fuzz harnesses..."
        cmake --preset macos-fuzz
        cmake --build --preset macos-fuzz
    fi
}

run_harnesses() {
    if [ "$MODE" != fuzz ]; then
        print_magenta "Replaying the committed fuzz corpus..."
        ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error
    fi
    if [ "$MODE" != replay ]; then
        mkdir -p fuzz-artifacts
        for harness in SeratoTagFuzzer CliArgumentsFuzzer; do
            print_magenta "Fuzzing $harness for $SECONDS_PER_HARNESS seconds..."
            mkdir -p "fuzz-scratch/$harness"
            "$BUILD_DIR/${harness}_artefacts/RelWithDebInfo/$harness" \
                -max_total_time="$SECONDS_PER_HARNESS" -artifact_prefix="fuzz-artifacts/$harness-" \
                "fuzz-scratch/$harness" "tests/fuzz/corpus/$harness"
        done
    fi
}

fuzz_project() {
    parse_arguments "$@"
    build_harnesses
    if [ "$BUILD_ONLY" = true ]; then
        return 0
    fi
    run_harnesses
}

fuzz_project "$@"
