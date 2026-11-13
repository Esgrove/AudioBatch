#!/bin/bash
# Measures bounded packaged VST3 CPU work without installed plugins, editors, storage, or audio devices.
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=image-control-plugin/rust/common.sh
source "$SCRIPT_DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Benchmark the explicit build-tree ImageControl RS VST3 with hyperfine.
Builds the Release plugin and hardware-free processing_benchmark host unless --no-build is given.
Wall time includes module load, preparation, warmup, copies, output checks, and teardown.
Each host report separately records warmed callback-only time, finite counts, energy, and a bit checksum.
Neutral and bypass also check independently delayed input bits. Active checksums are not a correctness oracle.
Results and machine/build metadata stay under ignored target/benchmark/ by default.
Run measurements only while other builds and benchmarks are idle. No baseline improvement is implied.

For separate preallocated Criterion DSP/analysis and setup measurements:
    cargo bench -p image-control-dsp --bench dsp -- --quick
    cargo bench -p image-control-dsp --bench dsp -- imager_warm/recover/48000

OPTIONS:
    -h | --help
        Show help before tool checks or builds.
    -N | --no-build
        Reuse the existing Release packaged VST3 and benchmark host.
    -b | --bundle <absolute-path>
        Exact build-tree ImageControl RS.vst3 inside the repository or explicit CARGO_TARGET_DIR.
        Default: shared target/bundled/ImageControl RS.vst3. Relative targets use the invocation directory.
    -C | --case <name>
        neutral|width|pan|gains|recover|delta|bypass|mono|solo|mono-input. Default: recover.
    -a | --signal <name>
        tones|moving-noise. Default: tones. Moving noise has six independently enveloped, panned bands.
    -S | --sample-rate <Hz>
        44100|48000|96000|192000. Default: 48000.
    -k | --block <frames|irregular>
        Host block size 1..4096, or repeating 1,17,257,511. Default: irregular.
    -f | --frames <N>
        Measured audio frames, 4096..4000000, excluding warmup. Default: 96000.
    -r | --runs <N>
        Measured hyperfine process runs, 1..100. Default: 5.
    -w | --warmup <N>
        Hyperfine process warmups, 0..20. Default: 1.
    -A | --artifacts <directory>
        Parent for a unique run directory. Default: target/benchmark/ beside this script.
"
parse_arguments() {
    BUILD=true
    BUNDLE=
    CUSTOM_BUNDLE=false
    CASE=recover
    SIGNAL=tones
    SAMPLE_RATE=48000
    BLOCK=irregular
    FRAMES=96000
    RUNS=5
    WARMUP=1
    ARTIFACTS="$SCRIPT_DIR/target/benchmark"
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                printf '%s\n' "$USAGE"
                exit 0
                ;;
            -N | --no-build) BUILD=false ;;
            -b | --bundle | -C | --case | -a | --signal | -S | --sample-rate | -k | --block | -f | --frames | -r | --runs | -w | --warmup | -A | --artifacts)
                [ $# -ge 2 ] && [ -n "$2" ] || print_error_and_exit "Missing value for $1. See --help."
                case "$1" in
                    -b | --bundle)
                        BUNDLE="$2"
                        CUSTOM_BUNDLE=true
                        ;;
                    -C | --case) CASE="$2" ;;
                    -a | --signal) SIGNAL="$2" ;;
                    -S | --sample-rate) SAMPLE_RATE="$2" ;;
                    -k | --block) BLOCK="$2" ;;
                    -f | --frames) FRAMES="$2" ;;
                    -r | --runs) RUNS="$2" ;;
                    -w | --warmup) WARMUP="$2" ;;
                    -A | --artifacts) ARTIFACTS="$2" ;;
                esac
                shift
                ;;
            *) print_error_and_exit "Unknown option: $1. See --help." ;;
        esac
        shift
    done
}

# Reject ambiguous, noncanonical, and unbounded numeric requests before builds or artifact creation.
check_integer() {
    local name="$1" value="$2" minimum="$3" maximum="$4"
    if ! [[ "$value" =~ ^(0|[1-9][0-9]*)$ ]] || [ "${#value}" -gt 7 ]; then
        print_error_and_exit "$name must be a canonical integer from $minimum to $maximum."
    fi
    if [ "$value" -lt "$minimum" ] || [ "$value" -gt "$maximum" ]; then
        print_error_and_exit "$name must be from $minimum to $maximum."
    fi
}

validate_options() {
    case "$SIGNAL" in
        tones | moving-noise) ;;
        *) print_error_and_exit "Unknown --signal: $SIGNAL." ;;
    esac
    check_integer --frames "$FRAMES" 4096 4000000
    check_integer --runs "$RUNS" 1 100
    check_integer --warmup "$WARMUP" 0 20
    if [ "$BLOCK" != irregular ]; then check_integer --block "$BLOCK" 1 4096; fi
    case "$SAMPLE_RATE" in
        44100 | 48000 | 96000 | 192000) ;;
        *) print_error_and_exit "--sample-rate must be 44100, 48000, 96000, or 192000." ;;
    esac
    case "$CASE" in
        neutral | width | pan | gains | recover | delta | bypass | mono | solo | mono-input) ;;
        *) print_error_and_exit "Unknown --case: $CASE." ;;
    esac
}

prepare_tools() {
    case "$BASH_PLATFORM" in
        mac | windows) ;;
        *) print_error_and_exit "Packaged benchmarks require macOS or Windows." ;;
    esac
    require_command hyperfine
    require_command uv
    require_command git
    require_command rustc
    require_command cargo
    if [ -z "$BUNDLE" ]; then BUNDLE="$CARGO_TARGET_DIR/bundled/ImageControl RS.vst3"; fi
    HOST="$CARGO_TARGET_DIR/release/examples/processing_benchmark"
    BENCHMARK_SHELL=bash
    if [ "$BASH_PLATFORM" = windows ]; then
        require_command cygpath
        require_command bash.exe
        HOST+=".exe"
        BENCHMARK_SHELL="\"$(cygpath -m "$(command -v bash.exe)")\""
    fi
}

prepare_artifacts() {
    # Validate the selected path before any build, including repository and symlink-escape policy.
    uv run --quiet --script "$SCRIPT_DIR/benchmarks/artifacts.py" validate-bundle "$REPO_ROOT" "$BUNDLE" \
        "$([ "$BUILD" = true ] && printf building || printf existing)"
    if [ "$BUILD" = true ]; then
        "$SCRIPT_DIR/build.sh"
        cargo build --manifest-path "$REPO_ROOT/Cargo.toml" --release -p image-control-plugin-rs \
            --example processing_benchmark --no-default-features --features vst3
    fi
    if [ ! -x "$HOST" ] || [ ! -d "$BUNDLE" ]; then
        print_error_and_exit "Release host or bundle is missing. Run without --no-build first."
    fi
    mkdir -p "$ARTIFACTS"
    RESULT_DIR=$(mktemp -d "$ARTIFACTS/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
    RESULT_DIR=$(cd "$RESULT_DIR" && pwd -P)
    printf 'Benchmark artifacts: %s\n' "$RESULT_DIR"
}

# Verifies counts, finite energy, and deterministic checksums independently of hyperfine timing.
run_preflight() {
    "$HOST" "${OPTIONS[@]}" > "$RESULT_DIR/preflight.json"
    "$HOST" "${OPTIONS[@]}" > "$RESULT_DIR/repeat.json"
    uv run --quiet --script "$SCRIPT_DIR/benchmarks/artifacts.py" check \
        "$RESULT_DIR/preflight.json" "$RESULT_DIR/repeat.json" "$FRAMES" "$CASE" "$SIGNAL"
    uv run --quiet --script "$SCRIPT_DIR/benchmarks/artifacts.py" metadata \
        "$REPO_ROOT" "$RESULT_DIR" "$HOST" "$BUNDLE" "$RUNS" "$WARMUP" "$BUILD" "$CUSTOM_BUNDLE"
}

# Hyperfine uses an explicit Bash shell so paths with spaces remain one argument on both supported platforms.
quote_command() {
    printf '%q ' "$@"
}

run_measurements() {
    local command
    command=$(quote_command "$HOST" "${OPTIONS[@]}")
    command+=" > $(printf '%q' "$RESULT_DIR/last-run.json")"
    hyperfine --shell "$BENCHMARK_SHELL" --warmup "$WARMUP" --runs "$RUNS" \
        --command-name "ImageControl RS $CASE $SIGNAL $SAMPLE_RATE/$BLOCK frames=$FRAMES" \
        --export-json "$RESULT_DIR/hyperfine.json" "$command"
    uv run --quiet --script "$SCRIPT_DIR/benchmarks/artifacts.py" check \
        "$RESULT_DIR/preflight.json" "$RESULT_DIR/last-run.json" "$FRAMES" "$CASE" "$SIGNAL"
    printf 'Complete process timings: %s/hyperfine.json\nWarmed callback diagnostics: %s/last-run.json\n' \
        "$RESULT_DIR" "$RESULT_DIR"
}

benchmark_plugin() {
    parse_arguments "$@"
    validate_options
    image_control_resolve_target_directory
    cd "$SCRIPT_DIR"
    prepare_tools
    prepare_artifacts
    local OPTIONS=(--bundle "$BUNDLE" --case "$CASE" --signal "$SIGNAL" --sample-rate "$SAMPLE_RATE" --block "$BLOCK" --frames "$FRAMES")
    run_preflight
    run_measurements
}

benchmark_plugin "$@"
