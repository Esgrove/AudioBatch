#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=cpp/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build a macOS .pkg installer for C++ AudioBatch (Apple Silicon arm64, macOS 15.0+).
Produces AudioBatch-<version>.pkg in dist/macos/ by default, with customizable Installer choices:
  - AudioBatch Application (/Applications/AudioBatch.app)
  - audiobatch Command-Line Tool (/usr/local/bin/audiobatch + /usr/local/share/zsh/site-functions/_audiobatch)

By default, payloads are signed with CODESIGN_IDENTITY (ad-hoc '-' when unset) and an unsigned .pkg is written
for local verification. Pass --sign or --notarize with Apple Developer credentials for distribution.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -o | --output-dir <dir>
        Directory for the generated .pkg file. Defaults to dist/macos in the repository root.

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
        Sign payloads with Developer ID Application and the .pkg with Developer ID Installer.

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

parse_arguments() {
    OUTPUT_DIR="$REPO_ROOT/dist/macos"
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

    if [ "$CLEAN_BUILD_DIR" = true ] && { [ "$REUSE_BUILD_DIR" = true ] || [ "$NO_BUILD" = true ]; }; then
        print_error_and_exit "--clean cannot be combined with --reuse or --no-build."
    fi

    require_mac_dist_cpu "$TARGET_CPU"
    BUILD_DIR="$REPO/cmake-build-mac-dist"
    export CCACHE_DIR="$BUILD_DIR/ccache"
}

check_juce_submodule() {
    if [ ! -f "$REPO/JUCE/CMakeLists.txt" ]; then
        print_yellow "JUCE submodule not found, initializing..."
        (cd "$REPO_ROOT" && git submodule update --init --recursive cpp/JUCE)
    fi
}

build_dist_artifacts() {
    require_command cmake
    require_command ninja "Ninja not found in path. Install it with: brew install ninja"
    check_juce_submodule

    if [ "$CLEAN_BUILD_DIR" = true ]; then
        print_magenta "Removing dist CMake build directory: $BUILD_DIR"
        rm -rf -- "${BUILD_DIR:?}"
    fi

    if [ "$REUSE_BUILD_DIR" = true ]; then
        require_mac_dist_cmake_cache "$BUILD_DIR"
    else
        print_magenta "Configuring C++ AudioBatch dist build ($TARGET_CPU, arm64, macOS 15.0)..."
        cmake -S "$REPO" -B "$BUILD_DIR" -G Ninja \
            -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_OSX_ARCHITECTURES=arm64 \
            -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
            -DAUDIOBATCH_NATIVE_CPU=OFF \
            -DAUDIOBATCH_TARGET_CPU=apple-m1 \
            -DBUILD_TESTING=OFF
    fi

    print_magenta "Building C++ AudioBatch dist targets..."
    cmake --build "$BUILD_DIR" --target AudioBatchSign AudioBatchCli
}

write_distribution_xml() {
    local destination="$1"
    local version="$2"
    cat > "$destination" << EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>AudioBatch $version</title>
    <organization>com.esgrove</organization>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64" rootVolumeOnly="true"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions>
            <os-version min="15.0"/>
        </allowed-os-versions>
    </volume-check>
    <license file="LICENSE.txt" mime-type="text/plain"/>
    <choices-outline>
        <line choice="choice_app"/>
        <line choice="choice_cli"/>
    </choices-outline>
    <choice
        id="choice_app"
        visible="true"
        start_selected="true"
        title="AudioBatch Application"
        description="Install AudioBatch.app into /Applications."
    >
        <pkg-ref id="com.esgrove.audiobatch.pkg.app"/>
    </choice>
    <choice
        id="choice_cli"
        visible="true"
        start_selected="true"
        title="audiobatch Command-Line Tool"
        description="Install the audiobatch CLI into /usr/local/bin and zsh completions into /usr/local/share/zsh/site-functions."
    >
        <pkg-ref id="com.esgrove.audiobatch.pkg.cli"/>
    </choice>
    <pkg-ref id="com.esgrove.audiobatch.pkg.app" version="$version" onConclusion="none">AudioBatch-app.pkg</pkg-ref>
    <pkg-ref id="com.esgrove.audiobatch.pkg.cli" version="$version" onConclusion="none">AudioBatch-cli.pkg</pkg-ref>
</installer-gui-script>
EOF
}

package_cpp_macos() {
    parse_arguments "$@"
    if [ "$BASH_PLATFORM" != mac ]; then
        print_error_and_exit "macOS .pkg packaging is only supported on macOS."
    fi
    validate_mac_target_cpu "$TARGET_CPU" false
    require_command pkgbuild
    require_command productbuild
    require_command plutil

    if [ "$SIGN_PKG" = true ]; then
        require_mac_developer_signing true
    fi

    if [ "$NO_BUILD" = false ]; then
        build_dist_artifacts
    fi
    require_mac_dist_cmake_cache "$BUILD_DIR"

    local app_source="$BUILD_DIR/AudioBatch_artefacts/Release/AudioBatch.app"
    local cli_source="$BUILD_DIR/AudioBatchCli_artefacts/Release/audiobatch"
    if [ ! -d "$app_source" ]; then
        print_error_and_exit "Built app bundle not found: $app_source"
    fi
    if [ ! -f "$cli_source" ]; then
        print_error_and_exit "Built CLI executable not found: $cli_source"
    fi

    local version
    version=$(get_cpp_version)
    verify_mac_dist_artifact "$BUILD_DIR/app-m1.sha256" "$app_source" "$version" "$NO_BUILD"
    verify_mac_dist_artifact "$BUILD_DIR/cli-m1.sha256" "$cli_source" "$version" "$NO_BUILD"
    local output_pkg="$OUTPUT_DIR/AudioBatch-${version}.pkg"

    local work_dir
    work_dir=$(mktemp -d "${TMPDIR:-/tmp}/audiobatch-cpp-pkg.XXXXXXXX")
    # Clean up temporary staging directories on exit.
    # shellcheck disable=SC2064
    trap "rm -rf '$work_dir'" EXIT

    local app_root="$work_dir/app-root"
    local cli_root="$work_dir/cli-root"
    local pkgs_dir="$work_dir/pkgs"
    local resources_dir="$work_dir/resources"
    mkdir -p \
        "$app_root/Applications" \
        "$cli_root/usr/local/bin" \
        "$cli_root/usr/local/share/zsh/site-functions" \
        "$pkgs_dir" \
        "$resources_dir"

    cp -R "$app_source" "$app_root/Applications/AudioBatch.app"
    sign_mac_app_bundle "$app_root/Applications/AudioBatch.app"

    local staged_cli="$cli_root/usr/local/bin/audiobatch"
    cp "$cli_source" "$staged_cli"
    chmod 755 "$staged_cli"
    sign_mac_cli_binary "$staged_cli" "$CLI_BUNDLE_ID"
    "$staged_cli" completion zsh > "$cli_root/usr/local/share/zsh/site-functions/_audiobatch"
    chmod 644 "$cli_root/usr/local/share/zsh/site-functions/_audiobatch"

    cp "$REPO_ROOT/LICENSE" "$resources_dir/LICENSE.txt"

    print_magenta "Building component packages for AudioBatch $version..."
    build_mac_bundle_component_pkg \
        "$app_root" \
        "com.esgrove.audiobatch.pkg.app" \
        "$version" \
        "$pkgs_dir/AudioBatch-app.pkg"

    pkgbuild \
        --root "$cli_root" \
        --identifier "com.esgrove.audiobatch.pkg.cli" \
        --version "$version" \
        --install-location / \
        "$pkgs_dir/AudioBatch-cli.pkg" > /dev/null

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

package_cpp_macos "$@"
