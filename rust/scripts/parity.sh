#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS] <paths...>

Compare the analysis results of the Rust and the C++ AudioBatch CLI.
Runs both on the same paths with --refresh and JSON output,
then compares the levels of every file within a tolerance with compare_levels.py.
With --normalize, both normalize their own copy of the paths in a temporary folder,
and the re-analyzed outputs are compared instead.
Every normalized output must also reach the target, or the true peak ceiling in loudness mode.
compare_outputs.py then compares the files both CLIs left behind:
the file names, the AIFF format, the samples, and the ID3v2 frames.
The outputs of lossless sources must have identical samples in peak mode,
and may differ by at most one integer PCM step in loudness mode.
The outputs of lossy sources may differ by the decoders' tolerance.
Both scripts run with uv, which installs the Python packages they need.
The originals are left alone, and --delete permanently removes replaced temporary copies without using the trash.
Reports each CLI's wall time and the Rust/C++ time ratio, including analysis and normalization when requested.
Exits with code 1 when any file differs or a CLI has an unexpected exit status.

The C++ CLI is taken from AUDIOBATCH_CPP_CLI when set, otherwise a release build is preferred.
With --release, an unclassified custom C++ binary requires AUDIOBATCH_CPP_BUILD_TYPE=Release.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -r | --recurse
        Recurse into directories.

    -t | --tolerance <dB>
        Allowed level difference in dB or LU. Default: 0.05

    -n | --normalize
        Normalize copies of the paths with both CLIs and compare the outputs.

    -p | --peak <dBFS>
        Normalize to this sample peak. Default: 0

    -l | --lufs <LUFS>
        Normalize to this integrated loudness instead of a peak.

    --release
        Build and run the Rust CLI in release mode, which is faster on large folders.
"

RUST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=../common.sh
source "$RUST_DIR/common.sh"

TOLERANCE="0.05"
RECURSE=()
NORMALIZE=()
TARGET=(--peak 0)
PROFILE="debug"
PATHS=()

while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help)
            echo "$USAGE"
            exit 0
            ;;
        -r | --recurse)
            RECURSE=(--recurse)
            ;;
        -t | --tolerance)
            TOLERANCE="$2"
            shift
            ;;
        -n | --normalize)
            NORMALIZE=(--normalize)
            ;;
        -p | --peak)
            TARGET=(--peak "$2")
            shift
            ;;
        -l | --lufs)
            TARGET=(--lufs "$2")
            shift
            ;;
        --release)
            PROFILE="release"
            ;;
        -*)
            echo "Unknown option: $1" >&2
            echo "$USAGE"
            exit 1
            ;;
        *)
            PATHS+=("$1")
            ;;
    esac
    shift
done

if [ ${#PATHS[@]} -eq 0 ]; then
    echo "$USAGE"
    exit 1
fi

require_command uv "uv is required to run the parity oracles. Install it from https://docs.astral.sh/uv/."
require_command cargo "Cargo is required to build the Rust CLI."
CPP_PROFILE=any
if [ "$PROFILE" = release ]; then
    CPP_PROFILE=release
else
    print_yellow "Warning: debug Rust parity timings are diagnostic only, not a release performance comparison." >&2
fi
CPP_CLI=$(find_cpp_cli "$CPP_PROFILE")
if [ "$(cpp_cli_build_type "$CPP_CLI")" != release ]; then
    print_yellow "Warning: C++ CLI is debug or its build type is unknown. Do not use these timings as a release baseline." >&2
fi

CARGO_PROFILE=()
if [ "$PROFILE" = "release" ]; then
    CARGO_PROFILE=(--release)
fi
(cd "$RUST_DIR" && cargo build --quiet "${CARGO_PROFILE[@]}" --bin audiobatch-rs)
RUST_CLI="$RUST_DIR/target/$PROFILE/$(executable_name audiobatch-rs)"

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

echo "C++ CLI:  $CPP_CLI"
echo "Rust CLI: $RUST_CLI"

CPP_PATHS=("${PATHS[@]}")
RUST_PATHS=("${PATHS[@]}")
CPP_BASE=""
RUST_BASE=""
if [ ${#NORMALIZE[@]} -gt 0 ]; then
    NORMALIZE+=(--delete)
    # Each CLI normalizes its own copy, since normalizing replaces and permanently deletes the inputs.
    # Every path gets a numbered folder, so paths with the same base name do not overwrite each other.
    CPP_BASE="$WORK_DIR/cpp"
    RUST_BASE="$WORK_DIR/rust"
    mkdir -p "$CPP_BASE" "$RUST_BASE"
    index=0
    for path in "${PATHS[@]}"; do
        index=$((index + 1))
        mkdir -p "$CPP_BASE/$index" "$RUST_BASE/$index"
        # Dereference symlinks while copying, so --delete cannot follow a copied link back to a user file.
        cp -RL "${path%/}" "$CPP_BASE/$index/"
        cp -RL "${path%/}" "$RUST_BASE/$index/"
    done
    # The lossy sources tell compare_outputs.py which outputs may differ by the tolerance,
    # and are listed now, since normalizing deletes the replaced sources.
    uv run --quiet --script "$RUST_DIR/scripts/compare_outputs.py" --list-lossy "$CPP_BASE" > "$WORK_DIR/lossy.txt"
    CPP_PATHS=("$CPP_BASE")
    RUST_PATHS=("$RUST_BASE")
    RECURSE=(--recurse)
fi

# Capture expected paths and fixture outcomes before either implementation can remove a source.
EXPECT_OPTIONS=()
if [ ${#NORMALIZE[@]} -gt 0 ]; then
    EXPECT_OPTIONS=(--base "$CPP_BASE" "${TARGET[@]}")
fi
uv run --quiet --script "$RUST_DIR/scripts/compare_levels.py" \
    --write-expectations "$WORK_DIR/expectations.json" "${RECURSE[@]}" "${EXPECT_OPTIONS[@]}" \
    --inputs "${CPP_PATHS[@]}"
if [ ${#NORMALIZE[@]} -gt 0 ]; then
    for implementation in cpp rust; do
        uv run --quiet --script "$RUST_DIR/scripts/compare_outputs.py" \
            --snapshot "$WORK_DIR/$implementation" --expectations "$WORK_DIR/expectations.json" \
            > "$WORK_DIR/$implementation-before.json"
    done
fi

# Both CLIs exit with code 2 when a file fails, which the comparison reports per file instead.
TARGET_OPTIONS=()
COMPARE_OPTIONS=()
if [ ${#NORMALIZE[@]} -gt 0 ]; then
    TARGET_OPTIONS=("${TARGET[@]}")
    COMPARE_OPTIONS=(--folders "$CPP_BASE" "$RUST_BASE" "${TARGET[@]}")
fi

# Bash's time keyword measures only the CLI, not the Python startup or the comparison oracles.
# The inner redirection keeps the CLI's stderr separate from the wall time.
run_timed_cli() {
    local implementation="$1"
    shift
    local cli_status=0
    { time { "$@" > "$WORK_DIR/$implementation.json" 2> "$WORK_DIR/$implementation.log"; }; } \
        2> "$WORK_DIR/$implementation.time" || cli_status=$?
    printf '%s\n' "$cli_status" > "$WORK_DIR/$implementation.status"
}

TIMEFORMAT='%R'
export LC_NUMERIC=C
run_timed_cli cpp "$CPP_CLI" --refresh "${RECURSE[@]}" "${NORMALIZE[@]}" "${TARGET_OPTIONS[@]}" \
    --output json "${CPP_PATHS[@]}"
run_timed_cli rust "$RUST_CLI" --refresh "${RECURSE[@]}" "${NORMALIZE[@]}" "${TARGET_OPTIONS[@]}" \
    --output json "${RUST_PATHS[@]}"
awk 'NR == 1 { cpp = $1; next } { printf "Wall time: C++ %.3f s, Rust %.3f s", cpp, $1;
    if (cpp > 0 && $1 > 0) printf ", Rust/C++ %.3fx (C++/Rust %.3fx)", $1 / cpp, cpp / $1;
    print "" }' "$WORK_DIR/cpp.time" "$WORK_DIR/rust.time"

STATUS=0
for implementation in cpp rust; do
    cli_status=$(< "$WORK_DIR/$implementation.status")
    if [ "$cli_status" != 0 ] && [ "$cli_status" != 2 ]; then
        print_error "$implementation CLI exited with unexpected status $cli_status"
        STATUS=1
    fi
    if [ "$cli_status" != 0 ]; then
        while IFS= read -r line; do printf '%s\n' "$line" >&2; done < "$WORK_DIR/$implementation.log"
    fi
done
uv run --quiet --script "$RUST_DIR/scripts/compare_levels.py" "$WORK_DIR/cpp.json" "$WORK_DIR/rust.json" \
    --tolerance "$TOLERANCE" --expectations "$WORK_DIR/expectations.json" "${COMPARE_OPTIONS[@]}" || STATUS=1

if [ ${#NORMALIZE[@]} -gt 0 ]; then
    MODE=peak
    if [ "${TARGET[0]}" = --lufs ]; then
        MODE=lufs
    fi
    uv run --quiet --script "$RUST_DIR/scripts/compare_outputs.py" "$CPP_BASE" "$RUST_BASE" --mode "$MODE" \
        --lossy "$WORK_DIR/lossy.txt" --snapshots "$WORK_DIR/cpp-before.json" "$WORK_DIR/rust-before.json" || STATUS=1
fi
exit $STATUS
