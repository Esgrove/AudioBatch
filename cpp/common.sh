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

# Writes the third-party notices of the C++ application and CLI to the given file.
# The second argument is the CMake build directory, whose fetched dependencies supply their license files.
write_cpp_third_party_notices() {
    local output="$1" deps="$2/_deps"
    start_third_party_notices "$output" "AudioBatch"
    append_juce_third_party_notices "$output" audiobatch
    append_third_party_notice "$output" "TagLib (LGPL-2.1-only OR MPL-1.1)" \
        "$deps/taglib-src/COPYING.LGPL" "$deps/taglib-src/COPYING.MPL"
    append_third_party_statement "$output" "TagLib source and relinking" \
        "TagLib is statically linked into AudioBatch. Its source code is available at https://github.com/taglib/taglib.
AudioBatch can be rebuilt against a modified TagLib from its complete source at https://github.com/Esgrove/AudioBatch."
    append_third_party_notice "$output" "utfcpp (BSL-1.0)" "$deps/taglib-src/3rdparty/utfcpp/LICENSE"
    append_third_party_notice "$output" "libebur128 (MIT)" "$deps/libebur128-src/COPYING"
    append_third_party_notice "$output" "{fmt} (MIT)" "$deps/fmt-src/LICENSE"
    # Builds without a system SQLite compile the fetched amalgamation into the binaries.
    if [ -d "$deps/sqlite3-src" ]; then
        append_third_party_statement "$output" "SQLite (public domain)" \
            "SQLite is in the public domain. See https://sqlite.org/copyright.html."
    fi
    append_roboto_font_notices "$output"
}

# Print the project version from cpp/CMakeLists.txt.
get_cpp_version() {
    awk '/^project\(/,/\)/ { if ($1 == "VERSION") { print $2; exit } }' "$REPO/CMakeLists.txt"
}
