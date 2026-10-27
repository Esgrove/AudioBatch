#!/bin/bash
set -eo pipefail

# Shell helpers shared by the C++ and Rust project scripts.
# Each project sources this through its own common.sh, which sets REPO to the project directory
# and adds the helpers specific to that project.

# Set Bash platform script is running on (mac/windows/linux).
# BSD (Mac) and GNU (Linux & Git for Windows) core-utils implementations have some differences
# for example in the available command line options.
case "$(uname -s)" in
    "Darwin")
        BASH_PLATFORM="mac"
        ;;
    "MINGW"*)
        BASH_PLATFORM="windows"
        ;;
    "Windows_NT"*)
        BASH_PLATFORM="windows"
        ;;
    *)
        BASH_PLATFORM="linux"
        ;;
esac
export BASH_PLATFORM

# Print a message with green color
print_green() {
    printf "\e[1;49;32m%s\e[0m\n" "$1"
}

# Print a message with magenta color
print_magenta() {
    printf "\e[1;49;35m%s\e[0m\n" "$1"
}

# Print a message with red color
print_red() {
    printf "\e[1;49;31m%s\e[0m\n" "$1"
}

# Print a message with yellow color
print_yellow() {
    printf "\e[1;49;33m%s\e[0m\n" "$1"
}

# Print an error message to stderr without exiting
print_error() {
    print_red "ERROR: $1" >&2
}

# Print the calling script's USAGE text and exit.
# Use exit code if given as argument, otherwise default to 1.
print_usage_and_exit() {
    echo "$USAGE"
    exit "${1:-1}"
}

# Print an error message and exit the program
print_error_and_exit() {
    print_red "ERROR: $1"
    # use exit code if given as argument, otherwise default to 1
    exit "${2:-1}"
}

# Exit with an error if the command given as the first argument is not found in path.
# The second argument is the error message, which defaults to saying the command was not found.
require_command() {
    if [ -z "$(command -v "$1")" ]; then
        print_error_and_exit "${2:-$1 not found in path}"
    fi
}

# Install the shell completions of a CLI binary that has the completion subcommand,
# for the shells of this platform: bash and PowerShell on Windows, zsh on macOS, and zsh and bash on Linux.
# The first argument is the binary, a name in path or a path to it.
# The second argument is "true" to only print the result summary, without the setup notes for each shell.
install_shell_completions() {
    local binary="$1"
    local silent="${2:-false}"
    local command_name
    command_name=$(basename "$binary" .exe)

    local shells
    case "$BASH_PLATFORM" in
        "windows")
            shells=(bash powershell)
            ;;
        "mac")
            shells=(zsh)
            ;;
        *)
            shells=(zsh bash)
            ;;
    esac

    print_magenta "Installing $command_name shell completions for: ${shells[*]}"

    local completion_args=(--install)
    if [ "$silent" = false ]; then
        completion_args+=(--verbose)
    fi

    local failed_shells=()
    local installed_count=0
    local shell
    for shell in "${shells[@]}"; do
        if "$binary" completion "$shell" "${completion_args[@]}"; then
            installed_count=$((installed_count + 1))
        else
            print_red "Failed to install $shell completion for $command_name"
            failed_shells+=("$shell")
        fi
    done

    if [ ${#failed_shells[@]} -gt 0 ]; then
        print_error_and_exit "Failed completions: ${failed_shells[*]}"
    fi

    print_green "Successfully installed $installed_count completion(s)"

    if [ "$silent" = true ]; then
        return
    fi

    # The single quoted lines are printed literally for the user to copy.
    # shellcheck disable=SC2016
    for shell in "${shells[@]}"; do
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
                    echo "  plugins=(... $command_name)"
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
}

# Enable calling functions directly as arguments:
# ./common.sh print_green "hello"
# shellcheck disable=SC2128
if [ $# -gt 0 ] && [ "$0" = "$BASH_SOURCE" ]; then
    "$@"
fi
