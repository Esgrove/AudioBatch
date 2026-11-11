#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Run a Rust test tier with the same selection and diagnostics as CI.
Defaults to the application unit and service tests.
Run ../test-plugin/build.sh first to include the real VST3 tests.

OPTIONS: Choose at most one test tier
    -h | --help
        Display these instructions.

    -u | --unit
        Run application unit and service tests, excluding GUI integration workflows.

    -g | --gui-integration
        Run nonvisual GUI workflows and save their output to gui.log in the artifacts directory.

    -O | --optimized
        Run release core tests, plus the CLI normalization subset on Windows.

    -i | --cli-integration
        Build the debug CLI and run its executable contracts through uv.
        CARGO_TARGET_DIR selects the build directory, defaulting to target/.

    -T | --tooling
        Check the parity oracles and benchmark tooling through uv.

    -A | --artifacts DIRECTORY
        Set the diagnostic directory for an integration tier.
        Defaults to target/integration-artifacts for GUI and target/integration-artifacts/cli for CLI.
        Relative paths are resolved from the rust/ application directory.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    TIER=""
    ARTIFACTS=""

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -u | --unit | -g | --gui-integration | -O | --optimized | -i | --cli-integration | -T | --tooling)
                if [ -n "$TIER" ]; then
                    print_error_and_exit "Choose only one test tier."
                fi
                case "$1" in
                    -u | --unit) TIER=--unit ;;
                    -g | --gui-integration) TIER=--gui-integration ;;
                    -O | --optimized) TIER=--optimized ;;
                    -i | --cli-integration) TIER=--cli-integration ;;
                    -T | --tooling) TIER=--tooling ;;
                esac
                ;;
            -A | --artifacts)
                if [ $# -lt 2 ] || [ -z "$2" ] || [[ "$2" == -* ]]; then
                    print_error_and_exit "--artifacts requires a directory."
                fi
                ARTIFACTS="$2"
                shift
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

    TIER="${TIER:---unit}"
    if [ -n "$ARTIFACTS" ] && [ "$TIER" != --gui-integration ] && [ "$TIER" != --cli-integration ]; then
        print_error_and_exit "--artifacts requires an integration tier."
    fi
}

run_tests() {
    parse_arguments "$@"
    cd "$REPO"

    if [ "$TIER" != --tooling ]; then
        require_command cargo "Cargo not found in path. Maybe install rustup?"
    fi

    case "$TIER" in
        --unit)
            cargo nextest run "${APPLICATION_PACKAGE_ARGS[@]}" --no-fail-fast --success-output final -E 'not test(integration_tests::)'
            ;;
        --gui-integration)
            ARTIFACTS="${ARTIFACTS:-target/integration-artifacts}"
            mkdir -p "$ARTIFACTS"
            cargo nextest run -p audiobatch-gui -E 'test(integration_tests::)' --no-fail-fast \
                --failure-output immediate-final 2>&1 | tee "$ARTIFACTS/gui.log"
            ;;
        --optimized)
            options=()
            if [ "$BASH_PLATFORM" = windows ]; then
                options=(-p audiobatch-cli -E 'package(audiobatch-core) | (package(audiobatch-cli) & test(run::test_run_analysis::normalize_))')
            fi
            cargo nextest run --release -p audiobatch-core "${options[@]}" --no-fail-fast
            ;;
        --cli-integration)
            require_command uv "uv not found in path, it runs the shared CLI integration suite"
            target_directory="${CARGO_TARGET_DIR:-$REPO/target}"
            cargo build --target-dir "$target_directory" --bin "$CLI_BINARY_NAME"
            cli_binary="$target_directory/debug/$(executable_name "$CLI_BINARY_NAME")"
            # The suite resolves paths, but an absolute binary also works from any subprocess directory.
            cli_binary="$(cd "$(dirname "$cli_binary")" && pwd)/$(basename "$cli_binary")"
            uv run --script ../tests/cli_integration.py --cli "$cli_binary" \
                --artifacts "${ARTIFACTS:-target/integration-artifacts/cli}"
            ;;
        --tooling)
            require_command uv "uv not found in path, it runs the Python tooling tests"
            uv run --script scripts/test_parity.py
            uv run --script scripts/test_cli_benchmark.py
            uv run --script ../tests/test_callback_benchmark.py
            ;;
    esac
}

run_tests "$@"
