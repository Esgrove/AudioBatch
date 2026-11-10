#!/bin/bash
# Runs scoped primary plugin contracts. The patched-GPUI reference is an explicit separate workspace.
set -eo pipefail

USAGE="Usage: $0 [--raw|--miri|--licenses]

Run primary Rust ImageControl tests without physical audio devices or installed plugins.
Choose at most one optional verification mode.

OPTIONS:
    -h | --help    Display these instructions without running tests.
    --raw         Run the isolated legacy raw VST3/GPUI workspace tests.
    --miri        Check primary parameter/controller/admission contracts under nightly Miri.
    --licenses    Audit the primary plugin's dependency licenses with cargo-deny.
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
cd "$SCRIPT_DIR"
ROOT_MANIFEST="$REPO_ROOT/Cargo.toml"
PACKAGES=(-p image-control-dsp -p image-control-plugin-rs -p image-control-xtask)
case "${1:-}" in
    "")
        BUNDLE="${IMAGE_CONTROL_OWN_BUNDLE:-${CARGO_TARGET_DIR:-$REPO_ROOT/rust/target}/bundled/ImageControl RS.vst3}"
        [ -d "$BUNDLE" ] || print_error_and_exit "Build the primary VST3 before native tests: ./build.sh --debug"
        IMAGE_CONTROL_OWN_BUNDLE="$BUNDLE" cargo test --manifest-path "$ROOT_MANIFEST" "${PACKAGES[@]}" --all-features
        cargo run --manifest-path "$ROOT_MANIFEST" -p image-control-plugin-rs --example realtime_host -- --case native-editor-lifecycle --bundle "$BUNDLE"
        ;;
    --raw) CARGO_TARGET_DIR="$SCRIPT_DIR/target" cargo test --manifest-path Cargo.toml --workspace --all-features ;;
    --miri)
        cargo +nightly miri test --manifest-path "$ROOT_MANIFEST" -p image-control-plugin-rs --no-default-features --test parameters --test controller_contracts
        cargo +nightly miri test --manifest-path "$ROOT_MANIFEST" -p image-control-plugin-rs --no-default-features --lib -- host::component::component_tests::admission_releases_after_nested_rejection_and_unwind
        # Full partitioned FIR preparation is covered natively, not interpreted for minutes under Miri.
        cargo +nightly miri test --manifest-path "$ROOT_MANIFEST" -p image-control-dsp --lib -- test_processing::malformed_controls_and_rate_are_rejected
        ;;
    --licenses) cargo deny --manifest-path "$SCRIPT_DIR/crates/nice/Cargo.toml" check licenses ;;
    *) print_error_and_exit "Usage: ./test.sh [--raw|--miri|--licenses]" ;;
esac
