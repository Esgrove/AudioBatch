#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=image-control-plugin/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build a macOS .pkg installer for C++ ImageControl (Apple Silicon arm64, macOS 15.0+).
Produces ImageControl-<version>.pkg in dist/macos/ by default, with customizable format checkboxes
in macOS Installer's Customize pane:
  - ImageControl VST3 (/Library/Audio/Plug-Ins/VST3/ImageControl.vst3)
  - ImageControl Audio Unit (/Library/Audio/Plug-Ins/Components/ImageControl.component)

By default, plugin bundles are signed with CODESIGN_IDENTITY (ad-hoc '-' when unset) and an unsigned .pkg
is written for local verification. Pass --sign or --notarize with Apple Developer credentials for distribution.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -o | --output-dir <dir>
        Directory for the generated .pkg file. Defaults to dist/macos in the repository root.

    -F | --formats <list>
        Plugin formats to build and include in the installer: 'all' (default), 'vst3', 'au', or 'vst3,au'.

    -m | --target-cpu <cpu>
        Compatibility option. Only 'apple-m1' is accepted. Distribution always targets M1 and newer.

    -c | --clean
        Delete the isolated dist CMake build directory before configuring.
        Cannot be used with --reuse or --no-build.

    -r | --reuse
        Skip CMake configuration and reuse the existing dist build directory.

    -N | --no-build
        Package existing dist build artifacts without invoking CMake.

    -s | --sign
        Sign plugin bundles with Developer ID Application and the .pkg with Developer ID Installer.

    -z | --notarize
        Implies --sign, submits the signed .pkg to Apple's notary service, and staples the ticket.

    -v | --verbose
        Display commands being executed.

ENVIRONMENT (with placeholder defaults):
    APPLE_TEAM_ID              Developer Team ID (default: TEAMID0000)
    CODESIGN_IDENTITY          Application signing identity (default: '-' locally, Developer ID with --sign)
    INSTALLER_SIGN_IDENTITY    Installer signing identity (default: Developer ID Installer: Esgrove (\$APPLE_TEAM_ID))
    APPLE_NOTARY_PROFILE       notarytool keychain profile name (default: audiobatch-notary)
    APPLE_ID                   Apple ID email when using APPLE_APP_SPECIFIC_PASSWORD
    APPLE_APP_SPECIFIC_PASSWORD App-specific password when not using APPLE_NOTARY_PROFILE
"

parse_formats() {
    local raw
    raw=$(echo "$1" | tr '[:upper:]' '[:lower:]')
    INCLUDE_VST3=false
    INCLUDE_AU=false

    if [ "$raw" = "all" ]; then
        INCLUDE_VST3=true
        INCLUDE_AU=true
        return
    fi

    local token
    local IFS=','
    for token in $raw; do
        case "$token" in
            vst3)
                INCLUDE_VST3=true
                ;;
            au | audiounit)
                INCLUDE_AU=true
                ;;
            *)
                print_error_and_exit "Unknown plugin format '$token' in --formats. Expected all, vst3, au, or vst3,au."
                ;;
        esac
    done

    if [ "$INCLUDE_VST3" = false ] && [ "$INCLUDE_AU" = false ]; then
        print_error_and_exit "--formats must select at least one plugin format."
    fi
}

parse_arguments() {
    OUTPUT_DIR="$REPO_ROOT/dist/macos"
    FORMATS_SPEC="all"
    TARGET_CPU="apple-m1"
    CLEAN_BUILD_DIR=false
    REUSE_BUILD_DIR=false
    NO_BUILD=false
    SIGN_PKG=false
    NOTARIZE_PKG=false

    if [ -n "${INSTALLER_SIGN_IDENTITY:-}" ] && [[ "$INSTALLER_SIGN_IDENTITY" != *"TEAMID0000"* ]]; then
        SIGN_PKG=true
    fi

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -o | --output-dir)
                if [ $# -lt 2 ] || [ -z "$2" ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                OUTPUT_DIR="$2"
                shift
                ;;
            -F | --formats)
                if [ $# -lt 2 ] || [ -z "$2" ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                FORMATS_SPEC="$2"
                shift
                ;;
            -m | --target-cpu)
                if [ $# -lt 2 ] || [ -z "$2" ]; then
                    print_error_and_exit "Missing value for $1."
                fi
                TARGET_CPU="$2"
                shift
                ;;
            -c | --clean)
                CLEAN_BUILD_DIR=true
                ;;
            -r | --reuse)
                REUSE_BUILD_DIR=true
                ;;
            -N | --no-build)
                NO_BUILD=true
                ;;
            -s | --sign)
                SIGN_PKG=true
                ;;
            -z | --notarize)
                SIGN_PKG=true
                NOTARIZE_PKG=true
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

    parse_formats "$FORMATS_SPEC"

    if [ "$CLEAN_BUILD_DIR" = true ] && { [ "$REUSE_BUILD_DIR" = true ] || [ "$NO_BUILD" = true ]; }; then
        print_error_and_exit "--clean cannot be combined with --reuse or --no-build."
    fi

    require_mac_dist_cpu "$TARGET_CPU"
    BUILD_DIR="$REPO/cmake-build-mac-dist"
    export CCACHE_DIR="$BUILD_DIR/ccache"
}

build_dist_plugins() {
    require_command cmake
    if [ ! -f "$REPO_ROOT/cpp/JUCE/CMakeLists.txt" ]; then
        print_error_and_exit "JUCE submodule not found, run: git submodule update --init cpp/JUCE"
    fi

    if [ "$CLEAN_BUILD_DIR" = true ]; then
        print_magenta "Removing dist CMake build directory: $BUILD_DIR"
        rm -rf -- "${BUILD_DIR:?}"
    fi

    local cmake_formats=()
    local build_targets=()
    if [ "$INCLUDE_VST3" = true ]; then
        cmake_formats+=(VST3)
        build_targets+=(ImageControl_VST3Sign)
    fi
    if [ "$INCLUDE_AU" = true ]; then
        cmake_formats+=(AU)
        build_targets+=(ImageControl_AUSign)
    fi
    local formats_joined
    formats_joined=$(
        IFS=';'
        echo "${cmake_formats[*]}"
    )

    if [ "$REUSE_BUILD_DIR" = true ]; then
        require_mac_dist_cmake_cache "$BUILD_DIR"
    else
        print_magenta "Configuring ImageControl dist build ($TARGET_CPU, arm64, macOS 15.0, formats: $formats_joined)..."
        plugin_configure "$BUILD_DIR" Release false OFF \
            -DCMAKE_OSX_ARCHITECTURES=arm64 \
            -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
            -DAUDIOBATCH_NATIVE_CPU=OFF \
            -DAUDIOBATCH_TARGET_CPU=apple-m1 \
            -DIMAGE_CONTROL_FORMATS="$formats_joined"
    fi

    print_magenta "Building ImageControl dist targets: ${build_targets[*]}"
    cmake --build "$BUILD_DIR" --config Release --target "${build_targets[@]}"
}

write_distribution_xml() {
    local destination="$1"
    local version="$2"
    local require_scripts="false"
    if [ "$INCLUDE_AU" = true ]; then
        require_scripts="true"
    fi

    {
        cat << EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>ImageControl $version</title>
    <organization>com.esgrove</organization>
    <options customize="allow" require-scripts="$require_scripts" hostArchitectures="arm64" rootVolumeOnly="true"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions>
            <os-version min="15.0"/>
        </allowed-os-versions>
    </volume-check>
    <license file="LICENSE.txt" mime-type="text/plain"/>
    <choices-outline>
EOF
        if [ "$INCLUDE_VST3" = true ]; then
            echo '        <line choice="choice_vst3"/>'
        fi
        if [ "$INCLUDE_AU" = true ]; then
            echo '        <line choice="choice_au"/>'
        fi
        echo '    </choices-outline>'

        if [ "$INCLUDE_VST3" = true ]; then
            cat << EOF
    <choice
        id="choice_vst3"
        visible="true"
        start_selected="true"
        title="ImageControl VST3"
        description="Install ImageControl.vst3 into /Library/Audio/Plug-Ins/VST3."
    >
        <pkg-ref id="com.esgrove.imagecontrol.pkg.vst3"/>
    </choice>
    <pkg-ref id="com.esgrove.imagecontrol.pkg.vst3" version="$version" onConclusion="none">ImageControl-vst3.pkg</pkg-ref>
EOF
        fi

        if [ "$INCLUDE_AU" = true ]; then
            cat << EOF
    <choice
        id="choice_au"
        visible="true"
        start_selected="true"
        title="ImageControl Audio Unit"
        description="Install ImageControl.component into /Library/Audio/Plug-Ins/Components."
    >
        <pkg-ref id="com.esgrove.imagecontrol.pkg.au"/>
    </choice>
    <pkg-ref id="com.esgrove.imagecontrol.pkg.au" version="$version" onConclusion="none">ImageControl-au.pkg</pkg-ref>
EOF
        fi
        echo '</installer-gui-script>'
    } > "$destination"
}

package_plugin_macos() {
    parse_arguments "$@"
    if [ "$BASH_PLATFORM" != mac ]; then
        print_error_and_exit "macOS .pkg packaging is only supported on macOS."
    fi
    require_command pkgbuild
    require_command productbuild
    require_command plutil

    if [ "$SIGN_PKG" = true ]; then
        require_mac_developer_signing true
    fi

    if [ "$NO_BUILD" = false ]; then
        build_dist_plugins
    fi
    require_mac_dist_cmake_cache "$BUILD_DIR"

    local vst3_source="$BUILD_DIR/ImageControl_artefacts/Release/VST3/ImageControl.vst3"
    local au_source="$BUILD_DIR/ImageControl_artefacts/Release/AU/ImageControl.component"
    if [ "$INCLUDE_VST3" = true ] && [ ! -d "$vst3_source" ]; then
        print_error_and_exit "Built VST3 bundle not found: $vst3_source"
    fi
    if [ "$INCLUDE_AU" = true ] && [ ! -d "$au_source" ]; then
        print_error_and_exit "Built Audio Unit bundle not found: $au_source"
    fi

    local version
    version=$(get_plugin_version)
    local suffix=""
    if [ "$INCLUDE_VST3" = true ] && [ "$INCLUDE_AU" = false ]; then
        suffix="-vst3"
    elif [ "$INCLUDE_VST3" = false ] && [ "$INCLUDE_AU" = true ]; then
        suffix="-au"
    fi
    if [ "$INCLUDE_VST3" = true ]; then
        verify_mac_dist_artifact "$BUILD_DIR/vst3-m1.sha256" "$vst3_source" "$version" "$NO_BUILD"
    fi
    if [ "$INCLUDE_AU" = true ]; then
        verify_mac_dist_artifact "$BUILD_DIR/au-m1.sha256" "$au_source" "$version" "$NO_BUILD"
    fi
    local output_pkg="$OUTPUT_DIR/ImageControl-${version}${suffix}.pkg"

    local work_dir
    work_dir=$(mktemp -d "${TMPDIR:-/tmp}/imagecontrol-pkg.XXXXXXXX")
    # Clean up temporary staging directories on exit.
    # shellcheck disable=SC2064
    trap "rm -rf '$work_dir'" EXIT

    local pkgs_dir="$work_dir/pkgs"
    local resources_dir="$work_dir/resources"
    mkdir -p "$pkgs_dir" "$resources_dir"
    cp "$REPO_ROOT/LICENSE" "$resources_dir/LICENSE.txt"

    print_magenta "Building component packages for ImageControl $version..."
    if [ "$INCLUDE_VST3" = true ]; then
        local vst3_root="$work_dir/vst3-root"
        mkdir -p "$vst3_root/Library/Audio/Plug-Ins/VST3"
        cp -R "$vst3_source" "$vst3_root/Library/Audio/Plug-Ins/VST3/ImageControl.vst3"
        sign_mac_plugin_bundle "$vst3_root/Library/Audio/Plug-Ins/VST3/ImageControl.vst3"
        build_mac_bundle_component_pkg \
            "$vst3_root" \
            "com.esgrove.imagecontrol.pkg.vst3" \
            "$version" \
            "$pkgs_dir/ImageControl-vst3.pkg"
    fi

    if [ "$INCLUDE_AU" = true ]; then
        local au_root="$work_dir/au-root"
        local au_scripts="$work_dir/au-scripts"
        mkdir -p "$au_root/Library/Audio/Plug-Ins/Components" "$au_scripts"
        cp -R "$au_source" "$au_root/Library/Audio/Plug-Ins/Components/ImageControl.component"
        sign_mac_plugin_bundle "$au_root/Library/Audio/Plug-Ins/Components/ImageControl.component"
        cat > "$au_scripts/postinstall" << 'EOF'
#!/bin/bash
killall -q AudioComponentRegistrar 2> /dev/null || true
exit 0
EOF
        chmod 755 "$au_scripts/postinstall"
        build_mac_bundle_component_pkg \
            "$au_root" \
            "com.esgrove.imagecontrol.pkg.au" \
            "$version" \
            "$pkgs_dir/ImageControl-au.pkg" \
            "$au_scripts"
    fi

    local distribution_xml="$work_dir/Distribution.xml"
    write_distribution_xml "$distribution_xml" "$version"

    print_magenta "Assembling distribution installer: $output_pkg"
    finalize_mac_distribution_pkg \
        "$distribution_xml" \
        "$resources_dir" \
        "$pkgs_dir" \
        "$output_pkg" \
        "$SIGN_PKG" \
        "$NOTARIZE_PKG"

    print_green "Created macOS installer: $output_pkg"
}

package_plugin_macos "$@"
