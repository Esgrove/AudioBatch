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
            print_usage_and_exit
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

# Determine which shells to install completions for based on platform
case "$BASH_PLATFORM" in
    "windows")
        SHELLS=(bash powershell)
        ;;
    "mac")
        SHELLS=(zsh)
        ;;
    "linux")
        SHELLS=(zsh bash)
        ;;
    *)
        print_error_and_exit "Unknown platform: $BASH_PLATFORM"
        ;;
esac

print_magenta "Installing $CLI_BINARY_NAME shell completions for: ${SHELLS[*]}"

COMPLETION_ARGS=(--install)
if [ "$SILENT" = false ]; then
    COMPLETION_ARGS+=(--verbose)
fi

FAILED_SHELLS=()
INSTALLED_COUNT=0

for shell in "${SHELLS[@]}"; do
    if "$CLI_BINARY_NAME" completion "$shell" "${COMPLETION_ARGS[@]}"; then
        INSTALLED_COUNT=$((INSTALLED_COUNT + 1))
    else
        print_red "Failed to install $shell completion for $CLI_BINARY_NAME"
        FAILED_SHELLS+=("$shell")
    fi
done

if [ ${#FAILED_SHELLS[@]} -gt 0 ]; then
    print_error_and_exit "Failed completions: ${FAILED_SHELLS[*]}"
fi

print_green "Successfully installed $INSTALLED_COUNT completion(s)"

# Print shell-specific sourcing instructions unless in silent mode.
# The single quoted lines are printed literally for the user to copy.
# shellcheck disable=SC2016
if [ "$SILENT" = false ]; then
    for shell in "${SHELLS[@]}"; do
        case "$shell" in
            bash)
                echo ""
                print_yellow "Note: For bash completions, ensure your .bashrc sources files from ~/.bash_completion.d/"
                echo "Add to your .bashrc if not already present:"
                echo '  for file in ~/.bash_completion.d/*; do'
                echo '      [ -f "$file" ] && source "$file"'
                echo '  done'
                ;;
            zsh)
                echo ""
                if [ -d "$HOME/.oh-my-zsh/custom/plugins" ]; then
                    print_yellow "Note: oh-my-zsh detected. Add the plugin to your .zshrc plugins list:"
                    echo "  plugins=(... $CLI_BINARY_NAME)"
                else
                    print_yellow "Note: For zsh completions, ensure your .zshrc includes the completions directory in fpath:"
                    echo '  fpath=(~/.zsh/completions $fpath)'
                    echo '  autoload -Uz compinit && compinit'
                fi
                ;;
            powershell)
                echo ""
                print_yellow "Note: PowerShell completions are installed to ~/Documents/PowerShell/completions/"
                echo "Ensure your PowerShell profile loads completions from that directory."
                echo 'Add to your $PROFILE if not already present:'
                echo '  Get-ChildItem "$HOME\Documents\PowerShell\completions\*.ps1" | ForEach-Object { . $_ }'
                ;;
        esac
    done
fi
