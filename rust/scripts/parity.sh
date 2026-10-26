#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS] <paths...>

Compare the analysis results of the Rust and the C++ AudioBatch CLI.
Runs both on the same paths with --refresh and JSON output,
then compares the levels of every file within a tolerance.
Exits with code 1 when any file differs.

The C++ CLI is taken from AUDIOBATCH_CPP_CLI when set,
otherwise from the repository root or the CMake debug build directory.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -r | --recurse
        Recurse into directories.

    -t | --tolerance <dB>
        Allowed level difference in dB or LU. Default: 0.05

    --release
        Build and run the Rust CLI in release mode, which is faster on large folders.
"

RUST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPO_DIR=$(cd "$RUST_DIR/.." && pwd)

TOLERANCE="0.05"
RECURSE=()
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
    local candidate
    for candidate in \
        "$REPO_DIR/audiobatch" \
        "$REPO_DIR/audiobatch.exe" \
        "$REPO_DIR/cmake-build-mac-debug/AudioBatchCli_artefacts/Debug/audiobatch" \
        "$REPO_DIR/cmake-build-windows-debug/AudioBatchCli_artefacts/Debug/audiobatch.exe"; do
        if [ -x "$candidate" ]; then
            echo "$candidate"
            return
        fi
    done
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

# Both CLIs exit with code 2 when a file fails, which the comparison reports per file instead.
"$CPP_CLI" --refresh "${RECURSE[@]}" --output json "${PATHS[@]}" > "$WORK_DIR/cpp.json" 2> "$WORK_DIR/cpp.log" || true
"$RUST_CLI" --refresh "${RECURSE[@]}" --output json "${PATHS[@]}" > "$WORK_DIR/rust.json" 2> "$WORK_DIR/rust.log" || true

python3 - "$WORK_DIR/cpp.json" "$WORK_DIR/rust.json" "$TOLERANCE" << 'EOF'
import json
import os
import sys

cpp_path, rust_path, tolerance_text = sys.argv[1:4]
tolerance = float(tolerance_text)

def load(path):
    with open(path, encoding="utf-8") as file:
        return {os.path.normpath(row["path"]): row for row in json.load(file)}

cpp_rows = load(cpp_path)
rust_rows = load(rust_path)

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
