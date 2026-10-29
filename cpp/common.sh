#!/bin/bash
set -eo pipefail

# The C++ project root, which is the directory of this script.
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export REPO

# shellcheck source=../common.sh
source "$REPO/../common.sh"

# On Windows, loads the Visual Studio x64 developer environment into this shell
# when the MSVC compiler is not on PATH, so that Ninja builds work from a plain Git Bash.
# Does nothing on other platforms or when cl is already available.
load_msvc_environment() {
    if [ "$BASH_PLATFORM" != "windows" ] || command -v cl > /dev/null 2>&1; then
        return 0
    fi

    local program_files_x86 vswhere install_dir vcvars
    program_files_x86=$(cmd.exe //c "echo %ProgramFiles(x86)%" 2> /dev/null | tr -d '\r')
    vswhere="$(cygpath -u "$program_files_x86")/Microsoft Visual Studio/Installer/vswhere.exe"
    if [ ! -x "$vswhere" ]; then
        print_error_and_exit "cl not found on PATH and vswhere.exe is missing. Install Visual Studio with the C++ tools or use a developer command prompt."
    fi
    install_dir=$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r')
    if [ -z "$install_dir" ]; then
        print_error_and_exit "No Visual Studio installation with the C++ tools found."
    fi
    vcvars=$(cygpath -w "$install_dir/VC/Auxiliary/Build/vcvars64.bat")

    print_magenta "Loading the Visual Studio developer environment..."
    # A temporary batch file avoids the quoting problems of passing a path with spaces through cmd.exe /c.
    local batch_file
    batch_file=$(mktemp --suffix=.bat)
    printf '@echo off\r\ncall "%s" > nul 2> nul\r\nset\r\n' "$vcvars" > "$batch_file"
    local env_dump
    env_dump=$(cmd.exe //c "$(cygpath -w "$batch_file")")
    rm -f "$batch_file"

    local name value line
    while IFS= read -r line; do
        line=${line%$'\r'}
        name=${line%%=*}
        value=${line#*=}
        # Skip cmd's per-drive entries such as =C: and anything that is not a valid variable name.
        if [[ ! "$name" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
            continue
        fi
        if [ "${name^^}" = "PATH" ]; then
            PATH="$(cygpath -u -p "$value")"
            export PATH
        else
            export "$name=$value"
        fi
    done <<< "$env_dump"

    if ! command -v cl > /dev/null 2>&1; then
        print_error_and_exit "cl is still not on PATH after loading $vcvars"
    fi
}
