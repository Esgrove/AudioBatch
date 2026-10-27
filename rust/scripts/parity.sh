#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS] <paths...>

Compare the analysis results of the Rust and the C++ AudioBatch CLI.
Runs both on the same paths with --refresh and JSON output,
then compares the levels of every file within a tolerance.
With --normalize, both normalize their own copy of the paths in a temporary folder,
and the re-analyzed outputs are compared instead.
Every normalized output must also reach the target, or the true peak ceiling in loudness mode.
compare_outputs.py then compares the files both CLIs left behind:
the file names, the AIFF format, the samples, and the ID3v2 frames.
It runs with uv, which installs the Python packages it needs.
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
TARGET=()
TARGET_MODE="peak"
TARGET_VALUE="0"
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
            TARGET_MODE="peak"
            TARGET_VALUE="$2"
            shift
            ;;
        -l | --lufs)
            TARGET=(--lufs "$2")
            TARGET_MODE="lufs"
            TARGET_VALUE="$2"
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
    # The CMake presets put the CLI straight in the artefacts folder and build.sh in a folder per build type,
    # so the most recently built one is used.
    local candidate
    local newest=""
    for candidate in \
        "$CPP_DIR/audiobatch" \
        "$CPP_DIR/audiobatch.exe" \
        "$CPP_DIR/cmake-build-mac-debug/AudioBatchCli_artefacts/audiobatch" \
        "$CPP_DIR/cmake-build-mac-debug/AudioBatchCli_artefacts/Debug/audiobatch" \
        "$CPP_DIR/cmake-build-windows-debug/AudioBatchCli_artefacts/audiobatch.exe" \
        "$CPP_DIR/cmake-build-windows-debug/AudioBatchCli_artefacts/Debug/audiobatch.exe"; do
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
    CPP_BASE="$WORK_DIR/cpp"
    RUST_BASE="$WORK_DIR/rust"
    mkdir -p "$CPP_BASE" "$RUST_BASE"
    for path in "${PATHS[@]}"; do
        cp -R "$path" "$CPP_BASE/"
        cp -R "$path" "$RUST_BASE/"
    done
    CPP_PATHS=("$CPP_BASE")
    RUST_PATHS=("$RUST_BASE")
    RECURSE=(--recurse)
fi

# Both CLIs exit with code 2 when a file fails, which the comparison reports per file instead.
TARGET_OPTIONS=()
if [ ${#NORMALIZE[@]} -gt 0 ]; then
    TARGET_OPTIONS=("${TARGET[@]}")
else
    TARGET_MODE=""
fi

"$CPP_CLI" --refresh "${RECURSE[@]}" "${NORMALIZE[@]}" "${TARGET_OPTIONS[@]}" --output json "${CPP_PATHS[@]}" \
    > "$WORK_DIR/cpp.json" 2> "$WORK_DIR/cpp.log" || true
"$RUST_CLI" --refresh "${RECURSE[@]}" "${NORMALIZE[@]}" "${TARGET_OPTIONS[@]}" --output json "${RUST_PATHS[@]}" \
    > "$WORK_DIR/rust.json" 2> "$WORK_DIR/rust.log" || true

STATUS=0
python3 - "$WORK_DIR/cpp.json" "$WORK_DIR/rust.json" "$TOLERANCE" "$CPP_BASE" "$RUST_BASE" \
    "$TARGET_MODE" "$TARGET_VALUE" << 'EOF' || STATUS=1
import json
import os
import sys

cpp_path, rust_path, tolerance_text, cpp_base, rust_base, target_mode, target_text = sys.argv[1:8]
tolerance = float(tolerance_text)
target = float(target_text)

# A normalized peak lands on the target within the quantization of the output samples.
PEAK_TARGET_TOLERANCE = 0.01
# Loudness lands within the gating of the loudness measurement, or stops at the true peak ceiling.
LOUDNESS_TARGET_TOLERANCE = 0.1
TRUE_PEAK_CEILING = -0.3
TRUE_PEAK_CEILING_TOLERANCE = 0.1

def missed_target(row):
    """Returns why a normalized output missed the target, or None when it reached it."""
    if target_mode == "peak":
        if row["peakDbfs"] is None or abs(row["peakDbfs"] - target) > PEAK_TARGET_TOLERANCE:
            return f"peak {row['peakDbfs']} dBFS instead of {target}"
    elif target_mode == "lufs" and row["integratedLufs"] is not None:
        loudness = row["integratedLufs"]
        true_peak = row["truePeakDbtp"]
        at_ceiling = true_peak is not None and abs(true_peak - TRUE_PEAK_CEILING) <= TRUE_PEAK_CEILING_TOLERANCE
        if abs(loudness - target) > LOUDNESS_TARGET_TOLERANCE and not (at_ceiling and loudness < target):
            return f"loudness {loudness} LUFS and true peak {true_peak} dBTP for a {target} LUFS target"
    return None

def load(path, base):
    with open(path, encoding="utf-8") as file:
        rows = json.load(file)
    # Normalized copies live in a folder per CLI, so they are matched by their path inside it.
    if base:
        return {os.path.relpath(os.path.realpath(row["path"]), os.path.realpath(base)): row for row in rows}
    return {os.path.normpath(row["path"]): row for row in rows}

cpp_rows = load(cpp_path, cpp_base)
rust_rows = load(rust_path, rust_base)

level_fields = ["peakDbfs", "truePeakDbtp", "integratedLufs", "maxShortTermLufs"]
exact_fields = ["sampleRate", "channels", "bitsPerSample", "status"]
differences = []

for path in sorted(set(cpp_rows) | set(rust_rows)):
    cpp = cpp_rows.get(path)
    rust = rust_rows.get(path)
    name = os.path.basename(path)
    if cpp is None or rust is None:
        differences.append(f"{name}: only in {'C++' if rust is None else 'Rust'} output")
        continue
    if cpp["status"] == "failed" and rust["status"] == "failed":
        continue
    if target_mode and cpp["status"] != "failed":
        for label, row in (("C++", cpp), ("Rust", rust)):
            if (reason := missed_target(row)) is not None:
                differences.append(f"{name}: {label} output missed the target, {reason}")
    for field in exact_fields:
        if cpp[field] != rust[field]:
            differences.append(f"{name}: {field} C++ {cpp[field]} Rust {rust[field]}")
    for field in level_fields:
        cpp_value = cpp[field]
        rust_value = rust[field]
        if cpp_value is None or rust_value is None:
            if cpp_value is not rust_value:
                differences.append(f"{name}: {field} C++ {cpp_value} Rust {rust_value}")
        elif abs(cpp_value - rust_value) > tolerance:
            differences.append(f"{name}: {field} C++ {cpp_value} Rust {rust_value}")

print(f"Compared {len(set(cpp_rows) | set(rust_rows))} files with a tolerance of {tolerance}")
if differences:
    print(f"{len(differences)} difference(s):")
    for difference in differences:
        print(f"  {difference}")
    sys.exit(1)
print("No differences")
EOF

if [ ${#NORMALIZE[@]} -gt 0 ]; then
    uv run --quiet --script "$RUST_DIR/scripts/compare_outputs.py" "$CPP_BASE" "$RUST_BASE" || STATUS=1
fi
exit $STATUS
