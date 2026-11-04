#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Run the unsafe VST3 hosting code, the test plugin, the Serato tag parsers,
and the analysis cache blob readers under Miri, which detects undefined behaviour.
Requires the nightly toolchain with the miri component and cargo-nextest.
Tests that need the file system, plugin bundles, or minutes of interpretation are ignored under Miri.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    --install
        Install the nightly toolchain with the miri and rust-src components first.

    -v | --verbose
        Display commands being executed.
"

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

INSTALL=false
while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help)
            print_usage_and_exit 0
            ;;
        --install)
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
# Stacked Borrows is the default aliasing model, and the VST3 COM bindings pass it.
# The vst3-host tests share one dedicated UI thread that runs until the test process exits,
# like the UI thread of an application, so Miri's leak check would report that thread after every test.
export MIRIFLAGS="${MIRIFLAGS:--Zmiri-ignore-leaks}"

print_magenta "Running the VST3 host, test plugin, and Serato tag tests under Miri..."
cargo +nightly miri nextest run --no-fail-fast -p vst3-host -p audiobatch-test-plugin -p serato-tags --lib

# Only the core modules that decode cache blobs run, because the rest of the core decodes audio files,
# uses SQLite, or runs minutes of DSP, none of which Miri can interpret in reasonable time.
print_magenta "Running the core cache blob readers under Miri..."
cargo +nightly miri nextest run --no-fail-fast -p audiobatch-core --lib --test spectrogram \
    -E '(kind(lib) & (test(/^overload::/) | test(/^waveform::/))) | (binary(spectrogram) & test(/^test_codec::/))'
