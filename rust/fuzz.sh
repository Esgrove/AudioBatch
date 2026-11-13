#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=rust/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build the cargo-fuzz targets, replay the committed corpus, and fuzz each target.
The serato_tags and cli_arguments targets also replay the corpus of the matching C++ harness.
New inputs go to fuzz/scratch/, and crashing inputs go to fuzz/artifacts/.
The committed corpus is never modified. Requires the nightly toolchain and cargo-fuzz.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -B | --build-only
        Build the targets without running them.

    -N | --no-build
        Use existing targets without building.
        Cannot be combined with --build-only.

    -r | --replay
        Only replay the committed corpus.

    -f | --fuzz
        Only fuzz, without a separate corpus replay.
        Cannot be combined with --replay.

    -s | --seconds <count>
        Fuzz each target for this many seconds. Defaults to 60.

    -T | --targets <all|parsers|decoder>
        Select all targets (default), the Serato/CLI/cache targets, or only decode_audio.
        Applies to building, replay, and mutation fuzzing. Failures always remain nonzero.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    # The selected target group. The default preserves complete local runs, including decoder findings.
    TARGET_GROUP=all

    BUILD_ONLY=false
    NO_BUILD=false
    MODE=all
    SECONDS_PER_TARGET=60
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
                    -r | --replay) selected_mode=replay ;;
                    -f | --fuzz) selected_mode=fuzz ;;
                esac
                if [ "$MODE" != all ] && [ "$MODE" != "$selected_mode" ]; then
                    print_error_and_exit "--replay and --fuzz cannot be used together."
                fi
                MODE="$selected_mode"
                ;;
            -s | --seconds)
                if [ $# -lt 2 ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                SECONDS_PER_TARGET="$2"
                shift
                ;;
            -T | --targets)
                if [ $# -lt 2 ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                TARGET_GROUP="$2"
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

    case "$TARGET_GROUP" in
        all) TARGETS=(serato_tags cli_arguments cache_blobs decode_audio) ;;
        parsers) TARGETS=(serato_tags cli_arguments cache_blobs) ;;
        decoder) TARGETS=(decode_audio) ;;
        *) print_error_and_exit "--targets must be all, parsers, or decoder." ;;
    esac

    if [ "$BUILD_ONLY" = true ] && [ "$NO_BUILD" = true ]; then
        print_error_and_exit "--build-only and --no-build cannot be used together."
    fi
    case "$SECONDS_PER_TARGET" in
        "" | *[!0-9]*)
            print_error_and_exit "--seconds must be a positive integer."
            ;;
    esac
    if ! [ "$SECONDS_PER_TARGET" -gt 0 ] 2> /dev/null; then
        print_error_and_exit "--seconds must be a positive integer."
    fi
}

run_fuzzing() {
    parse_arguments "$@"
    if [ "$BASH_PLATFORM" = windows ]; then
        print_error_and_exit "cargo-fuzz does not support Windows."
    fi
    require_command rustup "rustup not found in path, it provides the nightly toolchain that cargo-fuzz needs"
    require_command cargo-fuzz "cargo-fuzz not found in path, install it with: cargo install cargo-fuzz"

    cd "$REPO/fuzz"

    # The root target-dir setting is shared by the application and ImageControl, not the fuzz workspace.
    export CARGO_TARGET_DIR="$REPO/fuzz/target"

    # The native CPU flags of .cargo/config.toml would make the targets depend on the build machine.
    export RUSTFLAGS=""

    build_targets
    if [ "$BUILD_ONLY" = true ]; then
        exit 0
    fi
    run_targets
}

# Sets CORPUS to the directories a target replays,
# its own and the C++ one for the targets that share the input layout of a C++ harness.
set_corpus_directories() {
    CORPUS=("corpus/$1")
    case "$1" in
        serato_tags)
            CORPUS+=("../../cpp/tests/fuzz/corpus/SeratoTagFuzzer")
            ;;
        cli_arguments)
            CORPUS+=("../../cpp/tests/fuzz/corpus/CliArgumentsFuzzer")
            ;;
    esac
}

build_targets() {
    if [ "$NO_BUILD" != true ]; then
        print_magenta "Building $TARGET_GROUP fuzz targets..."
        if [ "$TARGET_GROUP" = all ]; then
            cargo +nightly fuzz build --fuzz-dir "$REPO/fuzz"
        else
            for target in "${TARGETS[@]}"; do
                cargo +nightly fuzz build --fuzz-dir "$REPO/fuzz" "$target"
            done
        fi
    fi
}

run_targets() {
    # cargo fuzz run always rebuilds, so the built targets run directly, like the C++ harnesses.
    HOST=$(rustc +nightly -vV | awk '/^host:/ { print $2 }')
    TARGET_DIRECTORY="target/$HOST/release"
    for target in "${TARGETS[@]}"; do
        if [ ! -x "$TARGET_DIRECTORY/$target" ]; then
            print_error_and_exit "Missing fuzz target $TARGET_DIRECTORY/$target, run without --no-build first."
        fi
    done

    # File size does not bound a hostile decoder's allocations or time inside one next_chunk call.
    # libFuzzer treats resource exhaustion as a finding and preserves its input in artifacts/.
    # The allocation hook reports a single allocation of at least 128 MiB.
    # The RSS monitor reports resident memory above 512 MiB, sampled by libFuzzer rather than a hard OS cap.
    RESOURCE_LIMITS=(-timeout=5 -malloc_limit_mb=128 -rss_limit_mb=512 -max_len=262145)

    # Keep freed-allocation detection without letting ASan's default 256 MiB quarantine dominate the RSS watchdog.
    # High-throughput Serato runs otherwise retain hundreds of thousands of freed chunks despite a small live heap.
    # Preserve caller diagnostics, with bounded quarantine settings applied last for the same policy in CI and locally.
    export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}quarantine_size_mb=64:thread_local_quarantine_size_kb=64"

    mkdir -p artifacts
    if [ "$MODE" != fuzz ]; then
        for target in "${TARGETS[@]}"; do
            print_magenta "Replaying the committed corpus of $target..."
            set_corpus_directories "$target"
            # With -runs=0, libFuzzer runs every input of the directories once and writes nothing to them.
            "$TARGET_DIRECTORY/$target" "${RESOURCE_LIMITS[@]}" -runs=0 -artifact_prefix="artifacts/$target-" "${CORPUS[@]}"
        done
    fi
    if [ "$MODE" != replay ]; then
        for target in "${TARGETS[@]}"; do
            print_magenta "Fuzzing $target for $SECONDS_PER_TARGET seconds..."
            mkdir -p "scratch/$target"
            set_corpus_directories "$target"
            # libFuzzer adds new inputs only to the first directory, so the committed seeds stay unchanged.
            "$TARGET_DIRECTORY/$target" "${RESOURCE_LIMITS[@]}" -max_total_time="$SECONDS_PER_TARGET" -artifact_prefix="artifacts/$target-" \
                "scratch/$target" "${CORPUS[@]}"
        done
    fi
}

run_fuzzing "$@"
