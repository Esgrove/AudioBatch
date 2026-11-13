#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [cpp|rust|image-control] [OPTIONS]

Install the CLIs, GUI apps, and shell completions of the C++ and Rust projects with their own install.sh scripts.
The C++ and Rust ImageControl plugins are installed with image-control-plugin/install.sh
and image-control-plugin/rust/install.sh, after the applications.
Name cpp or rust to install only that application, or image-control to install both plugins.

The options are passed to the install script of each project.
See cpp/install.sh --help and rust/install.sh --help for them.
Installing both projects accepts only the options both scripts have, such as --cli-only and --verbose.
Use -c or --clean to delete the C++ release CMake build directory before rebuilding.
Rust build artifacts are not cleaned. Use -l or --cli-only to install only the CLIs.
The options are not passed to either ImageControl plugin installer.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.
        After a project name, display the instructions of that project's script.

    -l | --cli-only
        Install only the CLIs, followed by both ImageControl plugins unless a project is named.

    -a | --app-only
        Install only the GUI apps, followed by both ImageControl plugins unless a project is named.

    -c | --clean
        Clean the selected C++ build directory before rebuilding.

    -r | --reuse
        Reuse the existing C++ build directory. Rust accepts this compatibility option without changing its build.

    -z | --notarize
        Sign with Developer ID and notarize the GUI apps and C++ ImageControl bundles (macOS only).
        CLIs are notarized only through package-macos.sh. Cannot be combined with --cli-only.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    parse_project_arguments install.sh true "$@"
    PLUGIN_INSTALL_ARGUMENTS=()
    local argument cli_only=false notarize=false
    for argument in "${PROJECT_ARGUMENTS[@]}"; do
        case "$argument" in
            -l | --cli-only) cli_only=true ;;
            -a | --app-only) cli_only=false ;;
            -z | --notarize) notarize=true ;;
        esac
        case "$argument" in
            -z | --notarize | -v | --verbose)
                PLUGIN_INSTALL_ARGUMENTS+=("$argument")
                ;;
        esac
    done
    if [ "$cli_only" = true ] && [ "$notarize" = true ]; then
        print_error_and_exit "--cli-only cannot be combined with --notarize. Use package-macos.sh for CLI distribution."
    fi
}

# Dispatch application options separately from the plugin's independent installer interface.
install_projects() {
    parse_arguments "$@"
    run_project_scripts install.sh "${PROJECT_ARGUMENTS[@]}"
    if [ "$PROJECT_SCOPE" = cpp ] || [ "$PROJECT_SCOPE" = rust ]; then
        return
    fi
    print_magenta "Running image-control-plugin/install.sh ${PLUGIN_INSTALL_ARGUMENTS[*]}"
    "$REPO_ROOT/image-control-plugin/install.sh" "${PLUGIN_INSTALL_ARGUMENTS[@]}"
    print_magenta "Running image-control-plugin/rust/install.sh"
    "$REPO_ROOT/image-control-plugin/rust/install.sh"
}

install_projects "$@"
