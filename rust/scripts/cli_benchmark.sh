#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Benchmark the release C++ and Rust CLIs with hyperfine.
Measures fresh --refresh analysis, peak 0 normalization, and LUFS -14 normalization,
with --jobs 1 and --jobs N. Normalization includes initial analysis, rendering, output measurement, and validation.
Every invocation uses the same options and its own fresh temporary copies, prepared outside the timer.
Replaced temporary originals are permanently deleted with --delete, never sent to the trash.
The auto corpus combines fixtures/normalization with fixtures/benchmark/output when generated audio exists.
Measured runs alternate C++ and Rust, with warmup only before each command's first measured run.
Known invalid, silent, or excessive-gain fixtures are excluded, and each source gets its own folder to avoid conflicts.
JSON, Markdown, CLI logs, and machine/build metadata go to a unique run directory under rust/target/cli-benchmark.
Timing is informational, but any CLI failure or correctness difference fails the script.
Requires hyperfine and uv, and cargo unless --no-build is given.

The C++ CLI is found in release CMake artefacts, or AUDIOBATCH_CPP_CLI.
A custom binary outside a recognizable release build requires AUDIOBATCH_CPP_BUILD_TYPE=Release.
HOME, CFFIXED_USER_HOME, and XDG directories are isolated on a best-effort basis.
Native platform APIs may still use the real user cache, which this script never removes.
This is a warm filesystem-cache benchmark, not a cold disk benchmark.

OPTIONS:
    -h | --help
        Display these instructions.
    -j | --jobs <N>
        Parallel worker count to compare with 1. Default: logical CPU count.
    -r | --runs <N>
        Measured runs per command. Default: 5.
    -w | --warmup <N>
        Warmup runs per command. Default: 1.
    -c | --corpus <directory|auto>
        Use only this audio corpus read-only, or auto for both default collections. Default: auto.
    --no-build
        Use the existing Rust target/release CLI instead of building it.
"

RUST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=../common.sh
source "$RUST_DIR/common.sh"

JOBS=$(getconf _NPROCESSORS_ONLN 2> /dev/null || sysctl -n hw.logicalcpu 2> /dev/null || echo 1)
RUNS=5
WARMUP=1
CORPUS=auto
BUILD=true
while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help) print_usage_and_exit 0 ;;
        -j | --jobs | -r | --runs | -w | --warmup | -c | --corpus)
            if [ $# -lt 2 ] || [ -z "$2" ]; then
                print_error_and_exit "Missing value for $1"
            fi
            case "$1" in
                -j | --jobs) JOBS="$2" ;;
                -r | --runs) RUNS="$2" ;;
                -w | --warmup) WARMUP="$2" ;;
                -c | --corpus) CORPUS="$2" ;;
            esac
            shift
            ;;
        --no-build) BUILD=false ;;
        *)
            print_error "Unknown option: $1"
            print_usage_and_exit
            ;;
    esac
    shift
done
for setting in JOBS RUNS WARMUP; do
    value="${!setting}"
    if ! [[ "$value" =~ ^[0-9]+$ ]] || [ "${#value}" -gt 6 ]; then
        print_error_and_exit "$setting must be a nonnegative integer with at most six digits."
    fi
done
JOBS=$((10#$JOBS))
RUNS=$((10#$RUNS))
WARMUP=$((10#$WARMUP))
if [ "$JOBS" -eq 0 ] || [ "$RUNS" -eq 0 ]; then
    print_error_and_exit "Jobs and measured runs must be greater than zero."
fi

require_command hyperfine "hyperfine is required. Install it with brew install hyperfine or cargo install hyperfine."
require_command uv "uv is required for benchmark preparation and validation. Install it from https://docs.astral.sh/uv/."
BENCHMARK_SHELL=bash
if [ "$BASH_PLATFORM" = windows ]; then
    require_command cygpath "Git Bash with cygpath is required for native Windows benchmarks."
    require_command bash.exe "Git Bash is required for native Windows benchmarks."
    # A bare bash can resolve to WSL when a native Windows process launches it.
    BENCHMARK_SHELL="\"$(cygpath -m "$(command -v bash.exe)")\""
fi
CPP_CLI=$(find_cpp_cli release)
if [ "$BUILD" = true ]; then
    require_command cargo "Cargo is required to build the release Rust CLI."
    (cd "$RUST_DIR" && cargo build --quiet --release --bin audiobatch-rs)
fi
RUST_CLI="$RUST_DIR/target/release/$(executable_name audiobatch-rs)"
if [ ! -x "$RUST_CLI" ]; then
    print_error_and_exit "Release Rust CLI not found at $RUST_CLI. Build it with cargo build --release --bin audiobatch-rs."
fi

mkdir -p "$RUST_DIR/target/cli-benchmark"
RESULT_DIR=$(mktemp -d "$RUST_DIR/target/cli-benchmark/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
RESULT_DIR=$(cd "$RESULT_DIR" && pwd -P)
echo "Benchmark results and failure logs: $RESULT_DIR"
WORK_DIR=$(mktemp -d)
WORK_DIR=$(cd "$WORK_DIR" && pwd -P)
trap 'rm -rf "${WORK_DIR:?}"' EXIT
HELPER="$RUST_DIR/scripts/cli_benchmark.py"
uv run --quiet --script "$HELPER" init "$WORK_DIR" "$RESULT_DIR" \
    --cpp "$CPP_CLI" --rust "$RUST_CLI" --jobs "$JOBS" --runs "$RUNS" --warmup "$WARMUP" --corpus "$CORPUS"

# Quote each argument for the explicit Bash shell used by every hyperfine version.
quote_command() {
    printf '%q ' "$@"
}

WORKER_COUNTS=(1)
if [ "$JOBS" -ne 1 ]; then WORKER_COUNTS+=("$JOBS"); fi
for jobs in "${WORKER_COUNTS[@]}"; do
    for mode in analyze peak lufs; do
        # One-run pairs alternate implementations without version-specific hyperfine scheduling flags.
        for ((run = 1; run <= RUNS; run++)); do
            warmup=0
            if [ "$run" -eq 1 ]; then warmup="$WARMUP"; fi
            benchmark_options=(--shell "$BENCHMARK_SHELL" --warmup "$warmup" --runs 1
                --export-json "$RESULT_DIR/raw/$mode-jobs-$jobs-run-$run.json")
            for implementation in cpp rust; do
                binary="$CPP_CLI"
                if [ "$implementation" = rust ]; then binary="$RUST_CLI"; fi
                target_options=()
                case "$mode" in
                    peak) target_options=(--normalize --delete --peak 0) ;;
                    lufs) target_options=(--normalize --delete --lufs -14) ;;
                esac
                home="$WORK_DIR/$implementation/home"
                command=$(quote_command env AUDIOBATCH_DELETE=0 HOME="$home" CFFIXED_USER_HOME="$home" \
                    XDG_CONFIG_HOME="$home/config" XDG_DATA_HOME="$home/data" XDG_CACHE_HOME="$home/cache" \
                    "$binary" --refresh --recurse --jobs "$jobs" --sort path --output json \
                    "${target_options[@]}" "$WORK_DIR/$implementation/input")
                command+=" > $(printf '%q' "$RESULT_DIR/$mode-jobs-$jobs-$implementation.json")"
                command+=" 2> $(printf '%q' "$RESULT_DIR/$mode-jobs-$jobs-$implementation.log")"
                prepare=$(quote_command uv run --quiet --script "$HELPER" prepare "$WORK_DIR" "$implementation")
                conclude=$(quote_command uv run --quiet --script "$HELPER" check "$WORK_DIR" "$RESULT_DIR" \
                    "$implementation" "$mode" "$jobs")
                benchmark_options+=(--command-name "$implementation $mode jobs=$jobs"
                    --prepare "$prepare" --conclude "$conclude" "$command")
            done
            # No --ignore-failure, invalid CLI flags, crashes, and unsupported input must remain failures.
            hyperfine "${benchmark_options[@]}"
            uv run --quiet --script "$HELPER" compare "$WORK_DIR" "$RESULT_DIR" "$mode" "$jobs"
        done
        uv run --quiet --script "$HELPER" combine "$WORK_DIR" "$RESULT_DIR" "$mode" "$jobs"
    done
done
uv run --quiet --script "$HELPER" report "$WORK_DIR" "$RESULT_DIR"
echo "Benchmark results: $RESULT_DIR"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    while IFS= read -r line; do printf '%s\n' "$line"; done < "$RESULT_DIR/summary.md" >> "$GITHUB_STEP_SUMMARY"
fi
