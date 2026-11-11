#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Install shell completions for the audiobatch CLI.
Automatically detects the platform and installs completions for the appropriate shells:
  - Windows: bash and powershell
  - macOS:   zsh

Uses the audiobatch in path, or the one install.sh puts in ~/.local/bin.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -s | --silent
        Only print the result summary.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    SILENT=false

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -s | --silent)
                SILENT=true
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

# Finds the installed CLI without changing application storage or building it.
find_installed_cli() {
    CLI_BINARY="$(command -v audiobatch || true)"
    if [ -z "$CLI_BINARY" ] && [ -x "$HOME/.local/bin/audiobatch" ]; then
        CLI_BINARY="$HOME/.local/bin/audiobatch"
    fi
    if [ -z "$CLI_BINARY" ]; then
        print_error_and_exit "audiobatch not found in path or ~/.local/bin. Run install.sh first."
    fi
}

install_completions() {
    parse_arguments "$@"
    find_installed_cli
    install_shell_completions "$CLI_BINARY" "$SILENT"
}

install_completions "$@"
