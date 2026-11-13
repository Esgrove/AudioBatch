#!/bin/bash
set -eo pipefail

# The C++ project root, which is the directory of this script.
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export REPO

# shellcheck source=common.sh
source "$REPO/../common.sh"

GUI_BUNDLE_ID="com.esgrove.audiobatch"
CLI_BUNDLE_ID="com.esgrove.audiobatch.cli"
export GUI_BUNDLE_ID CLI_BUNDLE_ID

# Print the project version from cpp/CMakeLists.txt.
get_cpp_version() {
    awk '/^project\(/,/\)/ { if ($1 == "VERSION") { print $2; exit } }' "$REPO/CMakeLists.txt"
}
