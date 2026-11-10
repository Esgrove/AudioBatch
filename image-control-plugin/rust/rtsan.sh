#!/bin/bash
# Audits the real packaged own-wrapper VST3, not an in-process factory or an installed plugin.
set -euo pipefail

USAGE="Usage: $0 [--bundle <path>] [--skip-build]

Run process-isolated native RealtimeSanitizer checks against the packaged ImageControl RS VST3.
Every native callback case requires a clean result. No dependency reports are suppressed.
Requires macOS, nightly Rust, uv, jq, and a matching Homebrew LLVM runtime.

OPTIONS:
    -h | --help       Display these instructions without building or checking dependencies.
    --bundle <path>   Audit this build-tree VST3 instead of the default own-wrapper bundle.
    --skip-build      Reuse the supplied/default bundle instead of building the Release plugin.

CASES:
    Detector controls, cold automation, reset, concurrent detached state save/restore,
    and native editor open/close/reopen while callbacks run on fresh processing threads.

ENVIRONMENT:
    AUDIOBATCH_RTSAN_TOOLCHAIN  Installed nightly toolchain, default nightly.
    AUDIOBATCH_RTSAN_CLANG      Matching clang executable, default Homebrew LLVM.
"
for argument in "$@"; do
    case "$argument" in
        -h | --help)
                     printf '%s\n' "$USAGE"
                                             exit 0
                                                    ;;
        --) break ;;
    esac
done
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PLUGIN_DIR=$(dirname "$SCRIPT_DIR")
# shellcheck source=../common.sh
source "$PLUGIN_DIR/common.sh"
BUNDLE="${IMAGE_CONTROL_OWN_BUNDLE:-${CARGO_TARGET_DIR:-$REPO_ROOT/rust/target}/bundled/ImageControl RS.vst3}"
SKIP_BUILD=false
while [ $# -gt 0 ]; do
    case "$1" in
        --bundle)
            [ $# -ge 2 ] || print_error_and_exit "--bundle requires a path"
            BUNDLE="$2"
            shift
            ;;
        --skip-build) SKIP_BUILD=true ;;
        *) print_error_and_exit "Unknown option: $1. See --help." ;;
    esac
    shift
done
[ "$BASH_PLATFORM" = mac ] || print_error_and_exit "Native realtime checks require macOS."
if [ "$SKIP_BUILD" = false ]; then "$SCRIPT_DIR/build.sh"; fi
[ -d "$BUNDLE/Contents" ] || print_error_and_exit "Build-tree VST3 is missing: $BUNDLE"
BUNDLE=$(cd "$BUNDLE" && pwd)
TOOLCHAIN=${AUDIOBATCH_RTSAN_TOOLCHAIN:-nightly}
CLANG=${AUDIOBATCH_RTSAN_CLANG:-$(brew --prefix llvm)/bin/clang}
RUST_VERSION=$(rustc +"$TOOLCHAIN" --version --verbose)
LLVM_VERSION=$(printf '%s\n' "$RUST_VERSION" | rg '^LLVM version: ' | cut -d ' ' -f 3)
[[ $("$CLANG" --version) == *"clang version $LLVM_VERSION"* ]] || print_error_and_exit "Rust and clang LLVM versions must match"
HOST=$(printf '%s\n' "$RUST_VERSION" | rg '^host: ' | cut -d ' ' -f 2)
RUNTIME=$("$CLANG" --print-file-name=libclang_rt.rtsan_osx_dynamic.dylib)
[ -f "$RUNTIME" ] || print_error_and_exit "Missing native RTSan runtime"
export CARGO_TARGET_DIR="$SCRIPT_DIR/target/rtsan-own"
export CARGO_INCREMENTAL=0
RUSTFLAGS="--cfg audiobatch_rtsan -Zsanitizer=realtime -Zexternal-clangrt -Cdebuginfo=2 -Cforce-frame-pointers=yes -Clinker=$CLANG -Clink-arg=$RUNTIME -Clink-arg=-Wl,-rpath,$(dirname "$RUNTIME")"
RTSAN_SYMBOLIZER_PATH="$(dirname "$CLANG")/llvm-symbolizer"
export RUSTFLAGS RTSAN_SYMBOLIZER_PATH
unset RTSAN_OPTIONS DYLD_INSERT_LIBRARIES
ARTIFACTS="$CARGO_TARGET_DIR/artifacts"
mkdir -p "$ARTIFACTS"
printf '%s\n' "$RUST_VERSION" "Runtime: $RUNTIME" "Bundle: $BUNDLE" > "$ARTIFACTS/versions.txt"
cargo +"$TOOLCHAIN" build --manifest-path "$REPO_ROOT/Cargo.toml" --target "$HOST" -p image-control-plugin-rs \
    --example realtime_host --message-format=json-render-diagnostics > "$ARTIFACTS/build.jsonl"
EXECUTABLE=$(jq -r 'select(.reason == "compiler-artifact" and .target.name == "realtime_host") | .executable // empty' "$ARTIFACTS/build.jsonl")
[ -x "$EXECUTABLE" ] || print_error_and_exit "No packaged realtime host was built"
run_case() {
    local name="$1"
    shift
    uv run --script "$REPO_ROOT/tests/realtime_sanitizer.py" --executable "$EXECUTABLE" \
        --case "own-vst3-$name" --artifacts "$ARTIFACTS" --timeout 45 \
        --argument=--case --argument="$name" --argument=--bundle --argument="$BUNDLE" "$@"
}
run_case clean-control --require-output 'CASE_OK clean-control'
run_case allocation-control --control --expect malloc --stack rust_allocation_region
run_case mutex-control --control --expect pthread_mutex_lock --stack mutex_region
for name in native-cold-automation native-reset native-state-interaction native-editor-lifecycle; do
    run_case "$name" --require-output "CASE_OK $name"
done
