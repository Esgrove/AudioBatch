#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 save|compare <dir> [--] [paths...]

Save the output of the Rust CLI, or compare a later run to a saved snapshot.
Analyzes the paths with --refresh and JSON output,
then normalizes copies of them in peak and in loudness mode and stores the JSON and the files it wrote.
The paths default to the audio files in fixtures/ and fixtures/normalization.
compare repeats the run and requires the JSON to be identical,
and compare_outputs.py to find the same files with identical samples and tags.
A saved snapshot checks exact output stability.
Regenerate it only after independently validating an intentional numerical change.
The CLI moves the replaced copies to the system trash, as it does when normalizing.
Exits with code 1 when anything differs.

OPTIONS:
    -h | --help
        Display these instructions.
"

# Resolve help before checking fixtures or creating snapshot directories.
# Arguments after -- are literal paths, not help requests.
for argument in "$@"; do
    case "$argument" in
        --) break ;;
        -h | --help)
            printf '%s\n' "$USAGE"
            exit 0
            ;;
    esac
done

RUST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=../common.sh
source "$RUST_DIR/common.sh"
FIXTURES_DIR=$(cd "$REPO_ROOT/fixtures" && pwd)

case "$1" in
    save | compare)
        COMMAND="$1"
        ;;
    *)
        echo "$USAGE"
        exit 1
        ;;
esac
if [ -z "$2" ]; then
    echo "$USAGE"
    exit 1
fi
mkdir -p "$(dirname "$2")"
SNAPSHOT_DIR="$(cd "$(dirname "$2")" && pwd -P)/$(basename "$2")"
shift 2

if [ "${1:-}" = -- ]; then
    shift
fi
PATHS=()
for path in "$@"; do
    # cp and the comparison helpers must receive option-looking names as literal paths.
    if [[ "$path" == -* ]]; then path="./$path"; fi
    PATHS+=("$path")
done
if [ ${#PATHS[@]} -eq 0 ]; then
    for path in "$FIXTURES_DIR"/*.*; do
        case "$path" in
            *.md | *.sh | *.py) ;;
            *) PATHS+=("$path") ;;
        esac
    done
    PATHS+=("$FIXTURES_DIR/normalization")
fi

(cd "$RUST_DIR" && cargo build --quiet --release --bin audiobatch-rs)
RUST_CLI="$RUST_DIR/target/release/audiobatch-rs"

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

# Copies the paths into a numbered folder each, so paths with the same base name do not overwrite each other.
copy_inputs() {
    local destination="$1"
    local index=0
    mkdir -p "$destination"
    for path in "${PATHS[@]}"; do
        index=$((index + 1))
        mkdir -p "$destination/$index"
        cp -R "${path%/}" "$destination/$index/"
    done
}

# Runs the CLI on a folder and writes its JSON with the folder replaced by a placeholder,
# so the JSON of two runs in different temporary folders compares exactly.
run_cli() {
    # The physical path, since the CLI reports /var on macOS as /private/var.
    local folder
    folder=$(cd "$1" && pwd -P)
    local output="$2"
    shift 2
    # The CLI exits with code 2 when a file fails, which the JSON records per file.
    "$RUST_CLI" --refresh --recurse --sort path --output json "$@" "$folder" > "$WORK_DIR/raw.json" 2> /dev/null || true
    sed "s|$folder|<root>|g" "$WORK_DIR/raw.json" > "$output"
}

# Writes the analysis and both normalizations of the paths into the folder.
take_snapshot() {
    local destination="$1"
    mkdir -p "$destination"
    copy_inputs "$WORK_DIR/analyze"
    run_cli "$WORK_DIR/analyze" "$destination/analyze.json"
    copy_inputs "$destination/peak"
    run_cli "$destination/peak" "$destination/peak.json" --normalize --peak 0
    copy_inputs "$destination/lufs"
    run_cli "$destination/lufs" "$destination/lufs.json" --normalize --lufs -14
    rm -rf "$WORK_DIR/analyze"
}

if [ "$COMMAND" = "save" ]; then
    rm -rf "$SNAPSHOT_DIR"
    take_snapshot "$SNAPSHOT_DIR"
    echo "Saved the snapshot to $SNAPSHOT_DIR"
    exit 0
fi

if [ ! -d "$SNAPSHOT_DIR" ]; then
    echo "No snapshot in $SNAPSHOT_DIR, take one with: $0 save $SNAPSHOT_DIR" >&2
    exit 1
fi
take_snapshot "$WORK_DIR/current"

STATUS=0
for name in analyze peak lufs; do
    if diff -u "$SNAPSHOT_DIR/$name.json" "$WORK_DIR/current/$name.json"; then
        echo "$name.json: identical"
    else
        STATUS=1
    fi
done
# An empty list of lossy sources makes every output pair require identical samples.
: > "$WORK_DIR/lossy.txt"
for mode in peak lufs; do
    echo "$mode outputs:"
    uv run --quiet --script "$RUST_DIR/scripts/compare_outputs.py" "$SNAPSHOT_DIR/$mode" "$WORK_DIR/current/$mode" \
        --lossy "$WORK_DIR/lossy.txt" || STATUS=1
done
exit "$STATUS"
