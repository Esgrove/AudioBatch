#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=rust/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Run the unsafe VST3 and CLAP hosting code, the shared host core, the test plugins,
the Serato tag parsers, and the analysis cache blob readers under Miri, which detects undefined behaviour.
Requires the nightly toolchain with the miri component and cargo-nextest.
Tests that need the file system, plugin bundles, or minutes of interpretation are ignored under Miri.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -I | --install
        Install the nightly toolchain with the miri and rust-src components first.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    INSTALL=false
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -I | --install)
                INSTALL=true
                ;;
            -v | --verbose)
                set -x
                ;;
            *)
                print_error "Unknown option: $1"
                print_usage_and_exit
                ;;
        esac
        shift
    done
}

run_miri() {
    parse_arguments "$@"
    require_command rustup "rustup not found in path, it provides the nightly toolchain that Miri needs"
    require_command cargo-nextest "cargo-nextest not found in path, install it with: cargo install cargo-nextest"

    cd "$REPO"

    if [ "$INSTALL" = true ]; then
        print_magenta "Installing nightly Miri..."
        rustup toolchain install nightly --profile minimal --component miri,rust-src
    fi
    if ! cargo +nightly miri --version > /dev/null 2>&1; then
        print_error_and_exit "Miri is not installed, run: $0 --install"
    fi

    # Miri interprets the code for the host it runs on, so the native CPU flags of .cargo/config.toml do not apply.
    export RUSTFLAGS=""
    # Stacked Borrows is Miri's default. Keep leak and thread-exit checking enabled except for the tests
    # that start the process-lifetime UI thread in vst3-host's test_helpers::dispatcher.
    # All instance tests and four processor tests use it, as do three dispatcher tests and the UiBound test.
    # Caller-supplied MIRIFLAGS still apply to every command.
    UI_THREAD_TESTS='test(/^instance::/) | test(/^processor::test_in_place_processing::(failed_processing_restores_owned_bus_descriptions|in_place_matches_buffered_gain_delay_and_echo|invalid_buffers_are_rejected_without_touching_audio|short_blocks_preserve_suffixes_and_owned_buffers)$/) | test(/^ui_bound::/) | test(/^thread::test_dispatcher::(another_thread_cannot_register_as_the_ui_thread|call_returns_the_result_from_the_ui_thread|post_runs_jobs_in_order)$/)'

    print_magenta "Running the host objects and parsers under Miri with leak checking..."
    cargo +nightly miri nextest run --no-fail-fast -p vst3-host -p plugin-host-core -p audiobatch-test-plugin -p serato-tags --lib \
        -E "not ($UI_THREAD_TESTS)"

    # These tests leave the dedicated UI thread alive at process exit.
    # This flag also disables allocation leak detection, but does not relax pointer or aliasing checks.
    print_magenta "Running the process-lifetime VST3 UI thread tests under Miri..."
    MIRIFLAGS="${MIRIFLAGS:+$MIRIFLAGS }-Zmiri-ignore-leaks" \
        cargo +nightly miri nextest run --no-fail-fast -p vst3-host -p plugin-host-core --lib -E "$UI_THREAD_TESTS"

    # In-process CLAP lifecycle tests use the same process-lifetime UI dispatcher.
    # Native module loading tests are explicitly ignored under Miri, never dynamically loaded here.
    print_magenta "Running in-process CLAP fixture contracts under Miri..."
    cargo +nightly miri nextest run --no-fail-fast -p audiobatch-test-clap-plugin --test contracts

    print_magenta "Running in-process CLAP host contracts under Miri..."
    MIRIFLAGS="${MIRIFLAGS:+$MIRIFLAGS }-Zmiri-ignore-leaks" \
        cargo +nightly miri nextest run --no-fail-fast -p clap-host --test contracts --test request_delivery

    # Only the core modules that decode cache blobs run, because the rest of the core decodes audio files,
    # uses SQLite, or runs minutes of DSP, none of which Miri can interpret in reasonable time.
    print_magenta "Running bounded callback admission and retirement under Miri..."
    cargo +nightly miri nextest run --no-fail-fast -p audiobatch-playback --lib \
        -E 'test(/^callback_access::/) | test(/^callback_lifecycle_tests::/) | test(/^callback_transport_tests::/)'

    print_magenta "Running the core cache blob readers under Miri..."
    cargo +nightly miri nextest run --no-fail-fast -p audiobatch-core --lib --test spectrogram \
        -E '(kind(lib) & (test(/^overload::/) | test(/^waveform::/))) | (binary(spectrogram) & test(/^test_codec::/))'
}

run_miri "$@"
