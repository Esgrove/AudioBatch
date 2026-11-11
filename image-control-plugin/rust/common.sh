#!/bin/bash
# Shares invocation-relative Cargo artifact paths across Rust ImageControl entry points.

# shellcheck source=../common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

# Resolves Cargo's target directory before changing the caller's working directory.
# Exporting an absolute path keeps nested Cargo commands and installer subprocesses on the same artifact root.
# This does not create directories or authorize an implicit installed-plugin search.
image_control_resolve_target_directory() {
    local directory="${CARGO_TARGET_DIR:-$REPO_ROOT/rust/target}"
    case "$directory" in
        [a-zA-Z]:[\\/]*)
            require_command cygpath
            directory=$(cygpath -u "$directory")
            ;;
        /*) ;;
        *) directory="$PWD/$directory" ;;
    esac
    if [ "$BASH_PLATFORM" = windows ]; then
        require_command cygpath
        directory=$(cygpath -m "$directory")
    fi
    export CARGO_TARGET_DIR="$directory"
}
