#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=rust/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Build a macOS .pkg installer for Rust AudioBatchRs (Apple Silicon arm64, macOS 15.0+).
Produces AudioBatchRs-<version>.pkg in dist/macos/ by default, with customizable Installer choices:
  - AudioBatchRs Application (/Applications/AudioBatchRs.app)
  - audiobatch-rs Command-Line Tool (/usr/local/bin/audiobatch-rs, zsh completion in
    /usr/local/share/zsh/site-functions/_audiobatch-rs, and third-party license notices)

By default, payloads are signed with CODESIGN_IDENTITY (ad-hoc '-' when unset) and an unsigned .pkg is written
for local verification. Pass --sign or --notarize with Apple Developer credentials for distribution.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -o | --output-dir <dir>
        Directory for the generated .pkg file. Defaults to dist/macos in the repository root.

    -m | --target-cpu <cpu>
        Apple Silicon target CPU baseline. Defaults to 'apple-m1' (compatible with M1 through M6).

    -c | --clean
        Accepted for compatibility with the root packaging wrapper.

    -r | --reuse
        Accepted for compatibility with the C++ packager. Cargo reuses its incremental dist cache automatically.

    -N | --no-build
        Package existing Cargo dist artifacts without invoking cargo build.

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
}

build_dist_binaries() {
    require_command cargo "Cargo not found in path. Maybe install rustup?"
    print_magenta "Building Rust AudioBatchRs dist profile ($TARGET_CPU, aarch64-apple-darwin, macOS 15.0)..."
    MACOSX_DEPLOYMENT_TARGET=15.0 \
        CARGO_TARGET_AARCH64_APPLE_DARWIN_RUSTFLAGS="-C target-cpu=$TARGET_CPU" \
        cargo build --manifest-path "$REPO_ROOT/Cargo.toml" --locked --profile dist --target aarch64-apple-darwin \
        -p audiobatch-cli -p audiobatch-gui --bins
}

write_distribution_xml() {
    local destination="$1"
    local version="$2"
    cat > "$destination" << EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>AudioBatchRs $version</title>
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
        title="AudioBatchRs Application"
        description="Install AudioBatchRs.app into /Applications."
    >
        <pkg-ref id="com.esgrove.audiobatch-rs.pkg.app"/>
    </choice>
    <choice
        id="choice_cli"
        visible="true"
        start_selected="true"
        title="audiobatch-rs Command-Line Tool"
        description="Install the audiobatch-rs CLI into /usr/local/bin, zsh completions into /usr/local/share/zsh/site-functions, and third-party notices."
    >
        <pkg-ref id="com.esgrove.audiobatch-rs.pkg.cli"/>
    </choice>
    <pkg-ref id="com.esgrove.audiobatch-rs.pkg.app" version="$version" onConclusion="none">AudioBatchRs-app.pkg</pkg-ref>
    <pkg-ref id="com.esgrove.audiobatch-rs.pkg.cli" version="$version" onConclusion="none">AudioBatchRs-cli.pkg</pkg-ref>
</installer-gui-script>
EOF
}

package_rust_macos() {
    parse_arguments "$@"
    if [ "$BASH_PLATFORM" != mac ]; then
        print_error_and_exit "macOS .pkg packaging is only supported on macOS."
    fi
    validate_mac_target_cpu "$TARGET_CPU" true
    require_command pkgbuild
    require_command productbuild
    require_command plutil

    if [ "$SIGN_PKG" = true ]; then
        require_mac_developer_signing true
    fi

    if [ "$NO_BUILD" = false ]; then
        build_dist_binaries
    fi

    local dist_target_dir="${CARGO_TARGET_DIR:-$REPO/target}/aarch64-apple-darwin/dist"
    local gui_source="$dist_target_dir/$GUI_BINARY_NAME"
    local cli_source="$dist_target_dir/$CLI_BINARY_NAME"
    if [ ! -f "$gui_source" ]; then
        print_error_and_exit "Built GUI executable not found: $gui_source"
    fi
    if [ ! -f "$cli_source" ]; then
        print_error_and_exit "Built CLI executable not found: $cli_source"
    fi

    local version
    version=$(get_workspace_version)
    local cpu_suffix=""
    if [ "$TARGET_CPU" != "apple-m1" ]; then
        cpu_suffix="-$TARGET_CPU"
    fi
    local output_pkg="$OUTPUT_DIR/AudioBatchRs-${version}${cpu_suffix}.pkg"

    local work_dir
    work_dir=$(mktemp -d "${TMPDIR:-/tmp}/audiobatch-rust-pkg.XXXXXXXX")
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
        "$cli_root/usr/local/share/licenses/audiobatch-rs" \
        "$pkgs_dir" \
        "$resources_dir"

    create_mac_app_bundle "$gui_source" "$app_root/Applications/$GUI_APP_BUNDLE"

    local staged_cli="$cli_root/usr/local/bin/$CLI_BINARY_NAME"
    cp "$cli_source" "$staged_cli"
    chmod 755 "$staged_cli"
    sign_mac_cli_binary "$staged_cli" "$CLI_BUNDLE_ID"
    "$staged_cli" completion zsh > "$cli_root/usr/local/share/zsh/site-functions/_$CLI_BINARY_NAME"
    chmod 644 "$cli_root/usr/local/share/zsh/site-functions/_$CLI_BINARY_NAME"
    copy_third_party_licenses "$cli_root/usr/local/share/licenses/audiobatch-rs"

    cp "$REPO_ROOT/LICENSE" "$resources_dir/LICENSE.txt"

    print_magenta "Building component packages for AudioBatchRs $version..."
    build_mac_bundle_component_pkg \
        "$app_root" \
        "com.esgrove.audiobatch-rs.pkg.app" \
        "$version" \
        "$pkgs_dir/AudioBatchRs-app.pkg"

    pkgbuild \
        --root "$cli_root" \
        --identifier "com.esgrove.audiobatch-rs.pkg.cli" \
        --version "$version" \
        --install-location / \
        "$pkgs_dir/AudioBatchRs-cli.pkg" > /dev/null

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

package_rust_macos "$@"
