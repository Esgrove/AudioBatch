#!/bin/bash
set -eo pipefail

# The Rust workspace root, which is the directory of this script.
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export REPO

# shellcheck source=../common.sh
source "$REPO/../common.sh"

# The command line binary, which supports the completion subcommand.
CLI_BINARY_NAME="audiobatch-rs"
# The GUI binary, which has its own minimal argument parsing.
GUI_BINARY_NAME="AudioBatchRs"
# The macOS app bundle of the GUI, whose bundle id keeps it apart from the C++ AudioBatch.app.
GUI_APP_BUNDLE="$GUI_BINARY_NAME.app"
GUI_BUNDLE_ID="com.esgrove.audiobatch-rs"
export CLI_BINARY_NAME GUI_BINARY_NAME GUI_APP_BUNDLE GUI_BUNDLE_ID

# Add the .exe suffix to an executable name on Windows
executable_name() {
    if [ "$BASH_PLATFORM" = "windows" ]; then
        echo "$1.exe"
    else
        echo "$1"
    fi
}

# Identify a C++ build from its configuration directory or CMake cache.
# Custom binaries need AUDIOBATCH_CPP_BUILD_TYPE=Release to explicitly attest an otherwise unknown release build.
cpp_cli_build_type() {
    local binary="$1"
    case "$binary" in
        */Debug/*)
            echo debug
            return
            ;;
        */Release/*)
            echo release
            return
            ;;
    esac
    local directory
    directory=$(cd "$(dirname "$binary")" && pwd -P)
    while [ "$directory" != / ]; do
        if [ -f "$directory/CMakeCache.txt" ]; then
            local build_type
            build_type=$(awk -F= '/^CMAKE_BUILD_TYPE:STRING=/ { print tolower($2); exit }' "$directory/CMakeCache.txt")
            if [ "$build_type" = release ] || [ "$build_type" = debug ]; then
                echo "$build_type"
                return
            fi
            break
        fi
        directory=$(dirname "$directory")
    done
    case "$binary" in
        */cmake-build-*-debug/*)
            echo debug
            return
            ;;
        */cmake-build-*-release/*)
            echo release
            return
            ;;
    esac
    case "${AUDIOBATCH_CPP_BUILD_TYPE:-}" in
        Release | release) echo release ;;
        Debug | debug) echo debug ;;
        *) echo unknown ;;
    esac
}

# Find the C++ CLI, preferring the newest verified release build over every debug or unclassified build.
# Passing "release" forbids a debug or unclassified baseline, including an explicit override.
find_cpp_cli() {
    local profile="${1:-any}"
    local cpp_directory="$REPO/../cpp"
    local binary_name
    binary_name=$(executable_name audiobatch)
    if [ -n "${AUDIOBATCH_CPP_CLI:-}" ]; then
        if [ ! -x "$AUDIOBATCH_CPP_CLI" ]; then
            print_error "C++ CLI is not executable: $AUDIOBATCH_CPP_CLI"
            return 1
        fi
        local override
        override="$(cd "$(dirname "$AUDIOBATCH_CPP_CLI")" && pwd -P)/$(basename "$AUDIOBATCH_CPP_CLI")"
        if [ "$profile" = release ] && [ "$(cpp_cli_build_type "$override")" != release ]; then
            print_error "A release C++ CLI is required. Use a Release artefact, or attest a custom build with AUDIOBATCH_CPP_BUILD_TYPE=Release."
            return 1
        fi
        echo "$override"
        return
    fi
    local candidate newest="" fallback=""
    for candidate in \
        "$cpp_directory"/cmake-build-*/AudioBatchCli_artefacts/Release/"$binary_name" \
        "$cpp_directory"/cmake-build-*/AudioBatchCli_artefacts/"$binary_name" \
        "$cpp_directory/$binary_name" \
        "$cpp_directory"/cmake-build-*/AudioBatchCli_artefacts/Debug/"$binary_name"; do
        if [ ! -x "$candidate" ]; then
            continue
        fi
        if [ "$(cpp_cli_build_type "$candidate")" = release ]; then
            if [ -z "$newest" ] || [ "$candidate" -nt "$newest" ]; then
                newest="$candidate"
            fi
        elif [ -z "$fallback" ] || [ "$candidate" -nt "$fallback" ]; then
            fallback="$candidate"
        fi
    done
    if [ -n "$newest" ]; then
        echo "$newest"
    elif [ "$profile" != release ] && [ -n "$fallback" ]; then
        echo "$fallback"
    else
        print_error "C++ CLI not found. Build a CMake release preset, or set AUDIOBATCH_CPP_CLI to a release binary."
        return 1
    fi
}

# Get all Rust executable names from the [[bin]] sections of the workspace crates
get_rust_executable_names() {
    local executables
    executables=$(awk -F'=' '/\[\[bin\]\]/,/name/ {if($1 ~ /name/) print $2}' "$REPO"/crates/*/Cargo.toml | tr -d ' "')
    if [ "$BASH_PLATFORM" = "windows" ]; then
        # shellcheck disable=SC2001
        executables=$(echo "$executables" | sed 's/$/.exe/')
    fi
    echo "$executables"
}

# Print the version from the [workspace.package] section of the workspace manifest
get_workspace_version() {
    awk -F'"' '/^\[workspace\.package\]/ { section = 1; next } /^\[/ { section = 0 } section && /^version/ { print $2; exit }' \
        "$REPO/Cargo.toml"
}

# Compile the layered Icon Composer icon into Assets.car in the given resources directory.
# Like the C++ build, this needs actool from Xcode 26 or later, and returns 1 when it is not available.
compile_icon_composer_icon() {
    local icon="$1"
    local resources="$2"
    local actool_version
    actool_version=$(xcrun actool --version 2> /dev/null | awk -F'[<>]' '/short-bundle-version/ { getline; print $3; exit }')
    if [ -z "$actool_version" ] || [ "${actool_version%%.*}" -lt 26 ]; then
        print_yellow "actool from Xcode 26 or later not found, the app gets only the icns icon"
        return 1
    fi
    xcrun actool --compile "$resources" \
        --platform macosx \
        --minimum-deployment-target 15.0 \
        --output-partial-info-plist /dev/null \
        --app-icon "$(basename "$icon" .icon)" \
        "$icon" > /dev/null
}

# Create the macOS app bundle of the GUI from a built executable.
# It carries the same icons as the C++ app from the shared assets directory:
# the layered Icon Composer icon for macOS 26 and the icns file for older versions.
create_mac_app_bundle() {
    local executable="$1"
    local bundle="$2"
    local assets="$REPO/../assets"
    local contents="$bundle/Contents"
    local icon_name_entry=""

    rm -rf "${bundle:?}"
    mkdir -p "$contents/MacOS" "$contents/Resources"
    cp "$executable" "$contents/MacOS/$GUI_BINARY_NAME"
    if compile_icon_composer_icon "$assets/icon_mac.icon" "$contents/Resources"; then
        icon_name_entry="<key>CFBundleIconName</key>
    <string>icon_mac</string>"
    fi
    # Copied after actool, which writes its own icns file, so older systems show the designed fallback.
    cp "$assets/icon_mac.icns" "$contents/Resources/icon_mac.icns"

    local version
    version=$(get_workspace_version)
    cat > "$contents/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleDisplayName</key>
    <string>$GUI_BINARY_NAME</string>
    <key>CFBundleExecutable</key>
    <string>$GUI_BINARY_NAME</string>
    <key>CFBundleIconFile</key>
    <string>icon_mac.icns</string>
    $icon_name_entry
    <key>CFBundleIdentifier</key>
    <string>$GUI_BUNDLE_ID</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>$GUI_BINARY_NAME</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$version</string>
    <key>CFBundleVersion</key>
    <string>$version</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.developer-tools</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF
    plutil -lint -s "$contents/Info.plist"
    printf "APPL????" > "$contents/PkgInfo"

    # An ad hoc signature seals the resources added after linking, so the bundle verifies as a whole.
    codesign --force --sign - "$bundle" 2> /dev/null
}
