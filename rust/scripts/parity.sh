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
The outputs of lossless sources must have identical samples,
and the outputs of lossy sources may differ by the decoders' tolerance.
Both scripts run with uv, which installs the Python packages they need.
The originals are left alone, but both CLIs move the replaced copies to the system trash.
Exits with code 1 when any file differs.

The C++ CLI is taken from AUDIOBATCH_CPP_CLI when set,
otherwise the newest build in the cpp directory or its CMake debug build directory.

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
CPP_DIR=$(cd "$RUST_DIR/../cpp" && pwd)

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

find_cpp_cli() {
    if [ -n "${AUDIOBATCH_CPP_CLI:-}" ]; then
        echo "$AUDIOBATCH_CPP_CLI"
        return
    fi
    # The CMake presets put the CLI in a folder per build type inside the artefacts folder,
    # older builds put it straight in the artefacts folder, and build.sh copies it into cpp/,
    # so the most recently built one is used.
    local candidate
    local newest=""
    for candidate in \
        "$CPP_DIR/audiobatch" \
        "$CPP_DIR/audiobatch.exe" \
        "$CPP_DIR/cmake-build-mac-debug/AudioBatchCli_artefacts/audiobatch" \
        "$CPP_DIR/cmake-build-mac-debug/AudioBatchCli_artefacts/Debug/audiobatch" \
        "$CPP_DIR/cmake-build-windows-debug/AudioBatchCli_artefacts/audiobatch.exe" \
        "$CPP_DIR/cmake-build-windows-debug/AudioBatchCli_artefacts/Debug/audiobatch.exe" \
        "$CPP_DIR/cmake-build-windows-ninja-debug/AudioBatchCli_artefacts/Debug/audiobatch.exe"; do
        if [ -x "$candidate" ] && { [ -z "$newest" ] || [ "$candidate" -nt "$newest" ]; }; then
            newest="$candidate"
        fi
    done
    echo "$newest"
}

CPP_CLI=$(find_cpp_cli)
if [ -z "$CPP_CLI" ]; then
    echo "C++ CLI not found. Build it with the CMake debug preset or set AUDIOBATCH_CPP_CLI." >&2
    exit 1
fi

CARGO_PROFILE=()
if [ "$PROFILE" = "release" ]; then
    CARGO_PROFILE=(--release)
fi
(cd "$RUST_DIR" && cargo build --quiet "${CARGO_PROFILE[@]}" --bin audiobatch-rs)
RUST_CLI="$RUST_DIR/target/$PROFILE/audiobatch-rs"

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

echo "C++ CLI:  $CPP_CLI"
echo "Rust CLI: $RUST_CLI"

CPP_PATHS=("${PATHS[@]}")
RUST_PATHS=("${PATHS[@]}")
CPP_BASE=""
RUST_BASE=""
if [ ${#NORMALIZE[@]} -gt 0 ]; then
    # Each CLI normalizes its own copy, since normalizing replaces and trashes the inputs.
    # Every path gets a numbered folder, so paths with the same base name do not overwrite each other.
    CPP_BASE="$WORK_DIR/cpp"
    RUST_BASE="$WORK_DIR/rust"
    mkdir -p "$CPP_BASE" "$RUST_BASE"
    index=0
    for path in "${PATHS[@]}"; do
        index=$((index + 1))
        mkdir -p "$CPP_BASE/$index" "$RUST_BASE/$index"
        cp -R "${path%/}" "$CPP_BASE/$index/"
        cp -R "${path%/}" "$RUST_BASE/$index/"
    done
    # The lossy sources tell compare_outputs.py which outputs may differ by the tolerance,
    # and are listed now, since normalizing moves the sources to the trash.
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

"$CPP_CLI" --refresh "${RECURSE[@]}" "${NORMALIZE[@]}" "${TARGET_OPTIONS[@]}" --output json "${CPP_PATHS[@]}" \
    > "$WORK_DIR/cpp.json" 2> "$WORK_DIR/cpp.log" || true
"$RUST_CLI" --refresh "${RECURSE[@]}" "${NORMALIZE[@]}" "${TARGET_OPTIONS[@]}" --output json "${RUST_PATHS[@]}" \
    > "$WORK_DIR/rust.json" 2> "$WORK_DIR/rust.log" || true

STATUS=0
uv run --quiet --script "$RUST_DIR/scripts/compare_levels.py" "$WORK_DIR/cpp.json" "$WORK_DIR/rust.json" \
    --tolerance "$TOLERANCE" --expectations "$WORK_DIR/expectations.json" "${COMPARE_OPTIONS[@]}" || STATUS=1

if [ ${#NORMALIZE[@]} -gt 0 ]; then
    uv run --quiet --script "$RUST_DIR/scripts/compare_outputs.py" "$CPP_BASE" "$RUST_BASE" \
        --lossy "$WORK_DIR/lossy.txt" --snapshots "$WORK_DIR/cpp-before.json" "$WORK_DIR/rust-before.json" || STATUS=1
fi
exit $STATUS
