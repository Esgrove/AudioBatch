#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [cpp|rust|image-control] [OPTIONS]

Build macOS .pkg distribution installers for C++ AudioBatch, Rust AudioBatchRs, and C++ ImageControl
(Apple Silicon arm64, macOS 15.0+, targeting apple-m1 by default).
Writes AudioBatch-<version>.pkg, AudioBatchRs-<version>.pkg, and ImageControl-<version>.pkg to dist/macos/.
Name cpp, rust, or image-control to build only that installer.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.
        After a project name, display the instructions of that project's script.

    -o | --output-dir <dir>
        Directory for the generated .pkg files. Defaults to dist/macos in the repository root.

    -m | --target-cpu <cpu>
        Apple Silicon target CPU baseline. Defaults to 'apple-m1'.

    -c | --clean
        Clean isolated dist CMake build directories before configuring.

    -r | --reuse
        Reuse existing dist CMake build directories.

    -N | --no-build
        Package existing dist artifacts without rebuilding.

    -s | --sign
        Sign payloads with Developer ID Application and .pkg files with Developer ID Installer.

    -z | --notarize
        Implies --sign, submits signed .pkg files to Apple's notary service, and staples tickets.

    -v | --verbose
        Display commands being executed.
"

parse_arguments() {
    parse_project_arguments package-macos.sh true "$@"
}

package_projects() {
    parse_arguments "$@"
    if [ "$BASH_PLATFORM" != mac ]; then
        print_error_and_exit "macOS .pkg packaging is only supported on macOS."
    fi
    run_project_scripts package-macos.sh "${PROJECT_ARGUMENTS[@]}"
    if [ "$PROJECT_SCOPE" = cpp ] || [ "$PROJECT_SCOPE" = rust ]; then
        return
    fi
    print_magenta "Running image-control-plugin/package-macos.sh ${PROJECT_ARGUMENTS[*]}"
    "$REPO_ROOT/image-control-plugin/package-macos.sh" "${PROJECT_ARGUMENTS[@]}"
}

package_projects "$@"
