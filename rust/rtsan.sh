#!/bin/bash
set -euo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=rust/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build and run process-isolated native Rust RealtimeSanitizer contracts.
Requires an installed nightly Rust compiler and matching Homebrew LLVM runtime.
Only macOS hosts with native realtime sanitizer support are accepted.

OPTIONS:
    -h | --help          Display these instructions.
    -A | --artifacts DIR Save versions, build records, and subprocess diagnostics in DIR.
    -G | --diagnostic    Collect known production blockers without treating them as clean passes.
    -S | --skip-native   Explicitly omit the build-tree native VST3 contracts.
    -v | --verbose      Display commands being executed.

Environment:
    AUDIOBATCH_RTSAN_TOOLCHAIN   Installed nightly toolchain, default nightly.
    AUDIOBATCH_RTSAN_CLANG      Matching clang executable, default Homebrew LLVM.
    AUDIOBATCH_TEST_VST3        Build-tree native bundle, default AudioBatch Test MS.
"

parse_arguments() {
    ARTIFACTS="$DIR/target/rtsan/artifacts"
    DIAGNOSTIC=false
    NATIVE=true
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help) print_usage_and_exit 0 ;;
            -A | --artifacts)
                if [ $# -lt 2 ] || [ -z "$2" ] || [[ "$2" == -* ]]; then
                    print_error_and_exit "--artifacts requires a directory"
                fi
                ARTIFACTS="$2"
                shift
                ;;
            -G | --diagnostic) DIAGNOSTIC=true ;;
            -S | --skip-native) NATIVE=false ;;
            -v | --verbose) set -x ;;
            *) print_error_and_exit "Unknown option: $1" ;;
        esac
        shift
    done
}

run_realtime_checks() {
    parse_arguments "$@"
    cd "$DIR"
    [ "$(uname -s)" = Darwin ] || print_error_and_exit "This native RTSan workflow requires macOS"
    require_command rustup "Install rustup and the nightly toolchain first"
    require_command uv "uv is required for the shared sanitizer runner"
    require_command jq "jq is required to locate Cargo test executables"
    require_command rg "ripgrep is required to inspect toolchain identity"
    TOOLCHAIN="${AUDIOBATCH_RTSAN_TOOLCHAIN:-nightly}"
    RUST_VERSION=$(rustc +"$TOOLCHAIN" --version --verbose) || print_error_and_exit "Installed nightly toolchain unavailable: $TOOLCHAIN"
    [[ "$RUST_VERSION" == *nightly* ]] || print_error_and_exit "RTSan requires nightly Rust"
    HOST=$(printf '%s\n' "$RUST_VERSION" | rg '^host: ' | cut -d ' ' -f 2)
    case "$HOST" in
        aarch64-apple-darwin | x86_64-apple-darwin) ;;
        *) print_error_and_exit "Unsupported native RTSan host: $HOST" ;;
    esac
    if [ -z "${AUDIOBATCH_RTSAN_CLANG:-}" ]; then
        require_command brew "Homebrew LLVM is required, or set AUDIOBATCH_RTSAN_CLANG"
    fi
    CLANG="${AUDIOBATCH_RTSAN_CLANG:-$(brew --prefix llvm)/bin/clang}"
    [ -x "$CLANG" ] || print_error_and_exit "Matching Homebrew LLVM clang is missing"
    LLVM_VERSION=$(printf '%s\n' "$RUST_VERSION" | rg '^LLVM version: ' | cut -d ' ' -f 3)
    CLANG_VERSION=$("$CLANG" --version)
    [[ "$CLANG_VERSION" == *"clang version $LLVM_VERSION"* ]] || print_error_and_exit "Rust LLVM $LLVM_VERSION and clang must match"
    RUNTIME=$("$CLANG" --print-file-name=libclang_rt.rtsan_osx_dynamic.dylib)
    [ -f "$RUNTIME" ] || print_error_and_exit "Matching native RTSan startup runtime is missing"
    SYMBOLIZER="$(dirname "$CLANG")/llvm-symbolizer"
    [ -x "$SYMBOLIZER" ] || print_error_and_exit "Matching llvm-symbolizer is missing"
    mkdir -p "$ARTIFACTS"
    ARTIFACTS=$(cd "$ARTIFACTS" && pwd)
    {
        printf '%s\n' "$RUST_VERSION" "$CLANG_VERSION" "Runtime: $RUNTIME" "Target: $HOST"
        cargo +"$TOOLCHAIN" --version
        shasum -a 256 "$RUNTIME"
    } | tee "$ARTIFACTS/versions.txt"

    # Explicit --target prevents sanitizer flags from reaching build scripts and proc macros.
    # Rust passes -nodefaultlibs, so explicitly link the single matching native startup runtime.
    # It also intercepts native calls made by uninstrumented VST3 bundles.
    export CARGO_TARGET_DIR="$DIR/target/rtsan"
    export CARGO_INCREMENTAL=0
    RUSTFLAGS="--cfg audiobatch_rtsan -Zsanitizer=realtime -Zexternal-clangrt -Cdebuginfo=2 -Cforce-frame-pointers=yes -Clinker=$CLANG -Clink-arg=$RUNTIME -Clink-arg=-Wl,-rpath,$(dirname "$RUNTIME")"
    export RUSTFLAGS
    export RTSAN_SYMBOLIZER_PATH="$SYMBOLIZER"
    export AUDIOBATCH_RTSAN_RUNTIME="$RUNTIME"
    unset RTSAN_OPTIONS DYLD_INSERT_LIBRARIES
    cargo +"$TOOLCHAIN" test --target "$HOST" -j 4 --no-run \
        -p audiobatch-playback -p vst3-host -p audiobatch-plugins --lib --test realtime \
        --message-format=json-render-diagnostics > "$ARTIFACTS/build.jsonl"

    PLAYBACK=$(jq -r 'select(.reason == "compiler-artifact" and .target.name == "audiobatch_playback" and .profile.test) | .executable // empty' "$ARTIFACTS/build.jsonl")
    HOST_TESTS=$(jq -r 'select(.reason == "compiler-artifact" and .target.name == "vst3_host" and .profile.test) | .executable // empty' "$ARTIFACTS/build.jsonl")
    PREVIEW=$(jq -r 'select(.reason == "compiler-artifact" and .target.name == "realtime" and .profile.test) | .executable // empty' "$ARTIFACTS/build.jsonl")
    for executable in "$PLAYBACK" "$HOST_TESTS" "$PREVIEW"; do
        [ -x "$executable" ] || print_error_and_exit "Cargo did not produce the requested test executable"
        links=$(otool -L "$executable")
        printf '%s\n' "$links" | tee -a "$ARTIFACTS/runtime-links.txt"
        [ "$(printf '%s\n' "$links" | rg -c 'libclang_rt.*rtsan')" = 1 ] || print_error_and_exit "Each executable must load exactly one native RTSan runtime"
    done

    run_contracts
    print_magenta "Rust RTSan artifacts: $ARTIFACTS"
}

# Libtest arguments replace the runner's default --case forwarding, --case remains the artifact label.
run_case() {
    local executable="$1" name="$2"
    shift 2
    local expects_report=false diagnostic=false option
    for option in "$@"; do
        case "$option" in
            --expect) expects_report=true ;;
            --diagnostic) diagnostic=true ;;
        esac
    done
    local completion=()
    if [ "$expects_report" = false ] || [ "$diagnostic" = true ]; then
        completion=(--require-output "^test $name \\.\\.\\. (?:[^\\n]*\\n)*?ok$" --require-output '^test result: ok\. 1 passed; 0 failed; 0 ignored; 0 measured; [0-9]+ filtered out;')
    fi
    uv run --script "$REPO_ROOT/tests/realtime_sanitizer.py" \
        --executable "$executable" --case "rust-${name//::/.}" --artifacts "$ARTIFACTS" --timeout 30 \
        --argument=--exact --argument="$name" --argument=--nocapture --argument=--test-threads=1 ${completion[@]+"${completion[@]}"} "$@"
}

run_contracts() {
    run_case "$PLAYBACK" realtime_tests::clean_control
    run_case "$PLAYBACK" realtime_tests::rust_allocation_control --control --expect malloc --stack rust_allocation_region
    run_case "$PLAYBACK" realtime_tests::rust_free_control --control --expect free --stack rust_free_region
    run_case "$PLAYBACK" realtime_tests::native_allocation_control --control --expect malloc --stack native_allocation_region
    run_case "$PLAYBACK" realtime_tests::mutex_control --control --expect pthread_mutex_lock --stack mutex_region
    run_case "$PLAYBACK" realtime_tests::nested_allocation_control --control --expect malloc --stack nested_allocation_region
    run_case "$PLAYBACK" realtime_tests::nested_scope_cleanup
    run_case "$PLAYBACK" realtime_tests::unwind_outside_scope_cleanup
    run_case "$PLAYBACK" realtime_tests::unwind_scope_baseline --control --expect malloc --stack unwind_region
    run_case "$PLAYBACK" output::insert::test_insert_playback::rtsan_full_callback_contended_silence
    run_case "$PLAYBACK" output::insert::test_insert_playback::rtsan_renderer_retirement_and_tail_storage
    for name in paused_output_is_silent stale_frames_are_skipped_and_the_insert_gain_is_left_to_the_insert end_of_file_stops_playback empty_buffer_before_the_end_counts_an_underrun; do
        run_case "$PLAYBACK" "output::test_output_renderer::$name"
    done
    for name in compensation_discards_exactly_the_latency a_pause_fades_out_and_a_resume_preserves_unheard_input the_end_drains_the_latency_and_the_tail non_finite_output_stops_playback a_seek_cancels_unheard_pre_roll_and_resets_at_the_target a_full_retire_queue_stalls_changes_until_control_thread_collection blocked_changes_preserve_the_active_inserts_tail; do
        run_case "$PLAYBACK" "output::insert::test_insert_playback::$name"
    done
    run_case "$HOST_TESTS" processor::test_in_place_processing::rtsan_gain_owned_and_borrowed_automation_reset
    run_case "$PREVIEW" preview_gain_first_automation
    run_case "$PREVIEW" preview_gain_reset
    # These clean contracts cover only the deterministic in-process CLAP fixture, not arbitrary plugins.
    run_case "$PREVIEW" preview_clap_cold_automation
    run_case "$PREVIEW" preview_clap_reset
    run_case "$PREVIEW" preview_clap_retirement
    run_case "$PLAYBACK" output::insert::test_insert_playback::rtsan_full_callback_clean_all_formats
    run_case "$PLAYBACK" output::insert::test_insert_playback::rtsan_full_callback_silence_all_formats
    run_case "$PLAYBACK" output::insert::test_insert_playback::rtsan_full_callback_idle_and_retired_silence
    run_case "$PLAYBACK" stream_errors::test_realtime::rtsan_owned_success_is_clean
    run_case "$PLAYBACK" stream_errors::test_realtime::rtsan_borrowed_ignored_is_clean
    if [ "$DIAGNOSTIC" = true ]; then
        # These audit complete owned-error disposal, not the clean normal sample callback gate.
        for name in rtsan_owned_ignored_is_blocked rtsan_owned_full_is_blocked rtsan_owned_disconnected_is_blocked; do
            run_case "$PLAYBACK" "stream_errors::test_realtime::$name" --diagnostic \
                --expect free --stack '(?s)function \x60free\x60.*(?:alloc::string::String|cpal::error::Error|cpal::Error).*StreamErrors.*report'
        done
    fi

    if [ "$NATIVE" = true ]; then
        export AUDIOBATCH_TEST_VST3="${AUDIOBATCH_TEST_VST3:-$DIR/../test-plugin/cmake-build-release/AudioBatchTestPlugin_artefacts/Release/VST3/AudioBatch Test MS.vst3}"
        [ -d "$AUDIOBATCH_TEST_VST3" ] || print_error_and_exit "Build the native fixture with test-plugin/build.sh, or supply its build-tree AUDIOBATCH_TEST_VST3"
        run_case "$PREVIEW" preview_native_loaded_runtime_identity \
            --require-output 'AUDIOBATCH_RTSAN_RUNTIME_LOADED phase=before-plugin count=1' \
            --require-output 'AUDIOBATCH_RTSAN_RUNTIME_LOADED phase=after-plugin count=1'
        # Pair each native call with its observed owner, never accept another JUCE method as an interchangeable stack.
        # Release inline frames fold setValueAndNotifyIfChanged's TLS access into processParameterChanges.
        local_native_owners='(?s)(?:function \x60calloc\x60.*libdyld\.dylib.*juce::JuceVST3Component::processParameterChanges\(Steinberg::Vst::IParameterChanges&\)|function \x60pthread_mutex_lock\x60.*juce::CriticalSection::enter\(\) const.*juce::JuceVST3Component::processAudio<float>\(Steinberg::Vst::ProcessData&\)|function \x60pthread_mutex_unlock\x60.*juce::CriticalSection::exit\(\) const.*juce::JuceVST3Component::processAudio<float>\(Steinberg::Vst::ProcessData&\))'
        for name in preview_native_first_automation preview_native_reset; do
            if [ "$DIAGNOSTIC" = true ]; then
                run_case "$PREVIEW" "$name" --diagnostic \
                    --expect calloc --expect pthread_mutex_lock --expect pthread_mutex_unlock \
                    --stack "$local_native_owners" --stack 'PreviewChain.*process' \
                    --require-output 'AUDIOBATCH_RTSAN_RUNTIME_LOADED phase=before-plugin count=1' \
                    --require-output 'AUDIOBATCH_RTSAN_RUNTIME_LOADED phase=after-plugin count=1' \
                    --require-output 'AUDIOBATCH_RTSAN_RUNTIME_LOADED phase=after-processing count=1'
            else
                run_case "$PREVIEW" "$name"
            fi
        done
    fi
}

run_realtime_checks "$@"
