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
