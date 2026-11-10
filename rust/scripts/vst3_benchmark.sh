#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Compare in-memory offline processing of AudioBatch Test MS in both native hosts.
Decoding, plugin setup, allocation, and sample exports are outside timing.
Host mode includes one host with input/output transfers. Chain mode adds the production channel adapter and finite checks.
Every output must match independent float arithmetic and final 16-bit PCM exactly.
The six cases cover mono/stereo, 44.1/48/96 kHz, one/four/eight plugins, and latency.
Each host has one unreported warmup and N measured repetitions, with fresh state before each repetition.
Alternates which host runs first for each case. Timings have no pass/fail threshold.
Reports and planar sample exports stay in a unique target/vst3-benchmark directory.
Builds the Release C++ test executable and Rust example unless --no-build is passed.

OPTIONS:
    -h | --help
        Display these instructions.
    -i | --iterations N
        Processing iterations in each measured run. Default: 10.
    -r | --runs N
        Measured runs per host and case. Default: 20. --repeat is an equivalent long alias.
    -C | --cpp PATH
        Use this Release C++ benchmark executable.
    -b | --bundle PATH
        Use this build-tree test-plugin bundle.
    -N | --no-build
        Use existing benchmark executables without building them.
"

# Resolve help before loading platform helpers or checking benchmark tools.
for argument in "$@"; do
    case "$argument" in
        --) break ;;
        -h | --help)
            printf '%s\n' "$USAGE"
            exit 0
            ;;
    esac
done

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=../common.sh
source "$DIR/common.sh"
ROOT="$REPO_ROOT"
ITERATIONS=10
RUNS=20
CPP=""
BUNDLE=""
BUILD=true
while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help) print_usage_and_exit 0 ;;
        -i | --iterations | -r | --runs | --repeat | -C | --cpp | -b | --bundle)
            if [ $# -lt 2 ] || [ -z "$2" ]; then
                print_error_and_exit "Missing value for $1"
            fi
            case "$1" in
                -i | --iterations) ITERATIONS="$2" ;;
                -r | --runs | --repeat) RUNS="$2" ;;
                -C | --cpp) CPP="$2" ;;
                -b | --bundle) BUNDLE="$2" ;;
            esac
            shift
            ;;
        -N | --no-build) BUILD=false ;;
        *) print_error_and_exit "Unknown option: $1" ;;
    esac
    shift
done
for value in "$ITERATIONS" "$RUNS"; do
    if ! [[ "$value" =~ ^[1-9][0-9]{0,2}$|^1000$ ]]; then
        print_error_and_exit "Iterations and runs must be decimal integers from 1 through 1000."
    fi
done
case "$BASH_PLATFORM" in
    windows)
        PRESET=windows-release
        BUILD_DIR="$ROOT/cpp/cmake-build-windows-release"
        ;;
    mac)
        PRESET=macos-release
        BUILD_DIR="$ROOT/cpp/cmake-build-mac-release"
        ;;
    *) print_error_and_exit "VST3 benchmarks require macOS or Windows." ;;
esac
if [ "$BUILD" = true ]; then
    (cd "$ROOT/cpp" && cmake --preset "$PRESET" -DBUILD_TESTING=ON && cmake --build --preset "$PRESET" --target AudioBatchTests)
    (cd "$DIR" && cargo build --release -p audiobatch-plugins --example vst3_benchmark)
fi
CPP=${CPP:-"$BUILD_DIR/AudioBatchTests_artefacts/Release/$(executable_name AudioBatchTests)"}
BUNDLE=${BUNDLE:-"$BUILD_DIR/test-plugin/AudioBatchTestPlugin_artefacts/Release/VST3/AudioBatch Test MS.vst3"}
RUST="$DIR/target/release/examples/$(executable_name vst3_benchmark)"
if [ ! -x "$CPP" ] || [ ! -x "$RUST" ] || [ ! -d "$BUNDLE" ]; then
    print_error_and_exit "Missing C++/Rust benchmark executable or real test-plugin bundle."
fi
if [ "$(cpp_cli_build_type "$CPP")" != release ]; then
    print_error_and_exit "The C++ benchmark must be a recognized Release build."
fi

# Native Windows processes need native paths, while Git Bash launches the executables itself.
native_path() {
    if [ "$BASH_PLATFORM" = windows ]; then
        cygpath -w "$1"
    else
        printf '%s\n' "$1"
    fi
}

mkdir -p "$DIR/target/vst3-benchmark"
RESULTS=$(mktemp -d "$DIR/target/vst3-benchmark/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
printf 'VST3 benchmark reports: %s\n' "$RESULTS"
CASES=(
    "stereo_split_44100_16.wav host 1 0"
    "stereo_split_44100_16.wav chain 1 0"
    "sine_mono_44100_16.wav chain 1 0"
    "sine_stereo_48000_24.wav chain 4 0"
    "sine_stereo_96000_16.wav chain 8 0"
    "stereo_split_44100_16.wav chain 4 256"
)
INDEX=0
for entry in "${CASES[@]}"; do
    read -r INPUT MODE INSTANCES LATENCY <<< "$entry"
    NAME="${INPUT%.*}-$MODE-$INSTANCES-$LATENCY"
    INPUT=$(native_path "$ROOT/fixtures/$INPUT")
    COMMON=("$(native_path "$BUNDLE")" "$INPUT" "" "$MODE" "$INSTANCES" "$LATENCY" "$ITERATIONS" "$RUNS")
    if [ $((INDEX % 2)) -eq 0 ]; then
        HOSTS=(cpp rust)
    else
        HOSTS=(rust cpp)
    fi
    for HOST in "${HOSTS[@]}"; do
        COMMON[2]=$(native_path "$RESULTS/$NAME-$HOST")
        if [ "$HOST" = cpp ]; then
            "$CPP" --vst3-benchmark "${COMMON[@]}" > "$RESULTS/$NAME-cpp.json"
        else
            "$RUST" run "${COMMON[@]}" > "$RESULTS/$NAME-rust.json"
        fi
    done
    "$RUST" compare "$(native_path "$RESULTS/$NAME-cpp.json")" "$(native_path "$RESULTS/$NAME-rust.json")" |
        tee "$RESULTS/$NAME-comparison.json" >> "$RESULTS/summary.jsonl"
    printf 'Verified %s\n' "$NAME"
    INDEX=$((INDEX + 1))
done
printf 'All native VST3 outputs match independent arithmetic and 16-bit PCM.\n'
