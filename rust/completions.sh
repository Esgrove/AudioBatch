#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Install shell completions for the audiobatch-rs CLI.
Automatically detects the platform and installs completions for the appropriate shells:
  - Windows: bash and powershell
  - macOS:   zsh
  - Linux:   zsh and bash

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -s | --silent
        Only print the result summary.

    -v | --verbose
        Display commands being executed.
"

# Import common functions
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

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

require_command "$CLI_BINARY_NAME" "$CLI_BINARY_NAME not found in path. Run install.sh first."

install_shell_completions "$CLI_BINARY_NAME" "$SILENT"
