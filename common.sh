#!/bin/bash
set -eo pipefail

# Shell helpers shared by the C++ and Rust project scripts.
# Each project sources this through its own common.sh, which sets REPO to the project directory
# and adds the helpers specific to that project.

# The repository root, which is the directory of this script.
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export REPO_ROOT

# Set Bash platform script is running on (mac/windows/linux).
# BSD (Mac) and GNU (Linux & Git for Windows) core-utils implementations have some differences
# for example in the available command line options.
case "$(uname -s)" in
    "Darwin")
        BASH_PLATFORM="mac"
        ;;
    "MINGW"*)
        BASH_PLATFORM="windows"
        ;;
    "Windows_NT"*)
        BASH_PLATFORM="windows"
        ;;
    *)
        BASH_PLATFORM="linux"
        ;;
esac
export BASH_PLATFORM

# Print a message with green color
print_green() {
    printf "\e[1;49;32m%s\e[0m\n" "$1"
}

# Print a message with magenta color
print_magenta() {
    printf "\e[1;49;35m%s\e[0m\n" "$1"
}

# Print a message with red color
print_red() {
    printf "\e[1;49;31m%s\e[0m\n" "$1"
}

# Print a message with yellow color
print_yellow() {
    printf "\e[1;49;33m%s\e[0m\n" "$1"
}

# Print each given file as "<action> [i/n] <file>", like clang-format --verbose does.
# Paths are shown relative to the repository root.
print_file_list() {
    local action="$1"
    shift
    local total=$#
    local index=0
    local file relative directory prefix repository_root
    repository_root=$(cd "$REPO_ROOT" && pwd -P)
    for file in "$@"; do
        index=$((index + 1))
        relative=$(realpath "$file")
        directory=$repository_root
        prefix=
        while [[ "$relative" != "$directory/"* && "$directory" != / ]]; do
            directory=${directory%/*}
            directory=${directory:-/}
            prefix="../$prefix"
        done
        relative="$prefix${relative#"${directory%/}/"}"
        printf '%s [%d/%d] %s\n' "$action" "$index" "$total" "${relative//\\//}"
    done
}

# Print an error message to stderr without exiting
print_error() {
    print_red "ERROR: $1" >&2
}

# Print the calling script's USAGE text and exit.
# Use exit code if given as argument, otherwise default to 1.
print_usage_and_exit() {
    echo "$USAGE"
    exit "${1:-1}"
}

# Print an error message to stderr and exit the program.
# Use exit code if given as the second argument, otherwise default to 1.
print_error_and_exit() {
    print_error "$1"
    exit "${2:-1}"
}

# Exit with an error if the command given as the first argument is not found in path.
# The second argument is the error message, which defaults to saying the command was not found.
require_command() {
    if [ -z "$(command -v "$1")" ]; then
        print_error_and_exit "${2:-$1 not found in path}"
    fi
}

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

# Parse root wrapper scope and help once, leaving project-specific options intact for their own parsers.
# The first two arguments select the child script and whether the wrapper exposes the independent plugin.
# Parsed output variables are consumed by the root wrapper that sources this library.
# shellcheck disable=SC2034
parse_project_arguments() {
    local script="$1" allow_plugin="$2" argument
    shift 2
    PROJECT_SCOPE=all
    PROJECTS=(cpp rust)
    case "${1:-}" in
        cpp | rust)
            PROJECT_SCOPE="$1"
            PROJECTS=("$1")
            shift
            ;;
        image-control)
            if [ "$allow_plugin" = true ]; then
                PROJECT_SCOPE=image-control
                PROJECTS=()
                shift
            fi
            ;;
    esac
    PROJECT_ARGUMENTS=("$@")
    PROJECT_CHECK=false
    for argument in "$@"; do
        case "$argument" in
            -c | --check)
                PROJECT_CHECK=true
                ;;
            -h | --help)
                case "$PROJECT_SCOPE" in
                    all) print_usage_and_exit 0 ;;
                    image-control) "$REPO_ROOT/image-control-plugin/$script" --help ;;
                    *) "$REPO_ROOT/$PROJECT_SCOPE/$script" "$@" ;;
                esac
                exit 0
                ;;
        esac
    done
}

# Execute the selected applications after parsing, stopping at the first failed child script.
run_project_scripts() {
    local script="$1" project
    shift
    for project in "${PROJECTS[@]}"; do
        print_magenta "Running $project/$script $*"
        "$REPO_ROOT/$project/$script" "$@"
    done
}

# Install the shell completions of a CLI binary that has the completion subcommand,
# for the shells of this platform: bash and PowerShell on Windows, zsh on macOS, and zsh and bash on Linux.
# The first argument is the binary, a name in path or a path to it.
# The second argument is "true" to only print the result summary, without the setup notes for each shell.
install_shell_completions() {
    local binary="$1"
    local silent="${2:-false}"
    local command_name
    command_name=$(basename "$binary" .exe)

    local shells
    case "$BASH_PLATFORM" in
        "windows")
            shells=(bash powershell)
            ;;
        "mac")
            shells=(zsh)
            ;;
        *)
            shells=(zsh bash)
            ;;
    esac

    print_magenta "Installing $command_name shell completions for: ${shells[*]}"

    local completion_args=(--install)
    if [ "$silent" = false ]; then
        completion_args+=(--verbose)
    fi

    local failed_shells=()
    local installed_count=0
    local shell
    for shell in "${shells[@]}"; do
        if "$binary" completion "$shell" "${completion_args[@]}"; then
            installed_count=$((installed_count + 1))
        else
            print_red "Failed to install $shell completion for $command_name"
            failed_shells+=("$shell")
        fi
    done

    if [ ${#failed_shells[@]} -gt 0 ]; then
        print_error_and_exit "Failed completions: ${failed_shells[*]}"
    fi

    print_green "Successfully installed $installed_count completion(s)"

    if [ "$silent" = true ]; then
        return
    fi

    # The single quoted lines are printed literally for the user to copy.
    # shellcheck disable=SC2016
    for shell in "${shells[@]}"; do
        case "$shell" in
            bash)
                echo ""
                print_yellow "Note: For bash completions, ensure your .bashrc sources files from ~/.bash_completion.d/"
                echo "Add to your .bashrc if not already present:"
                echo '  for file in ~/.bash_completion.d/*; do'
                echo '      [ -f "$file" ] && source "$file"'
                echo '  done'
                ;;
            zsh)
                echo ""
                if [ -d "$HOME/.oh-my-zsh/custom/plugins" ]; then
                    print_yellow "Note: oh-my-zsh detected. Add the plugin to your .zshrc plugins list:"
                    echo "  plugins=(... $command_name)"
                else
                    print_yellow "Note: For zsh completions, ensure your .zshrc includes the completions directory in fpath:"
                    echo '  fpath=(~/.zsh/completions $fpath)'
                    echo '  autoload -Uz compinit && compinit'
                fi
                ;;
            powershell)
                echo ""
                print_yellow "Note: PowerShell completions are installed to ~/Documents/PowerShell/completions/"
                echo "Ensure your PowerShell profile loads completions from that directory."
                echo 'Add to your $PROFILE if not already present:'
                echo '  Get-ChildItem "$HOME\Documents\PowerShell\completions\*.ps1" | ForEach-Object { . $_ }'
                ;;
        esac
    done
}

# Placeholder Apple Developer account configuration for macOS Developer ID signing and notarization.
# Local builds remain ad-hoc (-) by default until CODESIGN_IDENTITY or --sign/--notarize is requested.
APPLE_TEAM_ID="${APPLE_TEAM_ID:-TEAMID0000}"
APPLE_ID="${APPLE_ID:-developer@example.com}"
APPLE_NOTARY_PROFILE="${APPLE_NOTARY_PROFILE:-audiobatch-notary}"
APPLE_DEVELOPER_ORG="${APPLE_DEVELOPER_ORG:-Esgrove}"
export APPLE_TEAM_ID APPLE_ID APPLE_NOTARY_PROFILE APPLE_DEVELOPER_ORG

# Select --timestamp=none for local ad-hoc signing and --timestamp for Developer ID signing.
mac_codesign_timestamp_arg() {
    local identity="${1:--}"
    if [ "$identity" = "-" ]; then
        echo "--timestamp=none"
    else
        echo "--timestamp"
    fi
}

# Return 0 when the given codesign identity is a real certificate rather than ad-hoc (-).
mac_is_developer_id_identity() {
    local identity="${1:-}"
    [ -n "$identity" ] && [ "$identity" != "-" ]
}

# Print the Developer ID Application identity configured via CODESIGN_IDENTITY or derived from APPLE_TEAM_ID.
default_mac_developer_app_identity() {
    if mac_is_developer_id_identity "${CODESIGN_IDENTITY:-}"; then
        echo "$CODESIGN_IDENTITY"
    else
        echo "Developer ID Application: $APPLE_DEVELOPER_ORG ($APPLE_TEAM_ID)"
    fi
}

# Print the Developer ID Installer identity configured via INSTALLER_SIGN_IDENTITY or derived from APPLE_TEAM_ID.
default_mac_installer_identity() {
    if [ -n "${INSTALLER_SIGN_IDENTITY:-}" ]; then
        echo "$INSTALLER_SIGN_IDENTITY"
    else
        echo "Developer ID Installer: $APPLE_DEVELOPER_ORG ($APPLE_TEAM_ID)"
    fi
}

# Print setup instructions for the separate Apple Developer account placeholders and exit with status 1.
print_mac_signing_setup_and_exit() {
    local reason="$1"
    print_error "$reason"
    cat >&2 << EOF

Configure your Apple Developer credentials in the environment before running with --sign or --notarize:
  export APPLE_TEAM_ID="YOURTEAMID"
  export CODESIGN_IDENTITY="Developer ID Application: $APPLE_DEVELOPER_ORG (\$APPLE_TEAM_ID)"
  export INSTALLER_SIGN_IDENTITY="Developer ID Installer: $APPLE_DEVELOPER_ORG (\$APPLE_TEAM_ID)"

For notarization, store an app-specific password once in your login keychain:
  xcrun notarytool store-credentials "$APPLE_NOTARY_PROFILE" \\
      --apple-id "your-apple-id@example.com" \\
      --team-id "\$APPLE_TEAM_ID" \\
      --password "<app-specific-password>"
  export APPLE_NOTARY_PROFILE="$APPLE_NOTARY_PROFILE"

Or provide credentials directly via APPLE_ID, APPLE_TEAM_ID, and APPLE_APP_SPECIFIC_PASSWORD.
Inspect installed signing identities with: security find-identity -v
EOF
    exit 1
}

# Validate that Developer ID signing certificates are configured and present in the keychain.
# Pass "true" as the first argument when an Installer certificate (.pkg) is also required.
require_mac_developer_signing() {
    local need_installer="${1:-false}"
    if [ "$BASH_PLATFORM" != mac ]; then
        print_error_and_exit "macOS code signing and notarization are only supported on macOS."
    fi
    require_command codesign
    require_command security

    local app_identity
    app_identity=$(default_mac_developer_app_identity)
    if [[ "$app_identity" == *"TEAMID0000"* ]] || ! mac_is_developer_id_identity "$app_identity"; then
        print_mac_signing_setup_and_exit "CODESIGN_IDENTITY is still using placeholder '$app_identity'."
    fi

    local available_identities
    available_identities=$(security find-identity -v 2> /dev/null || true)
    if ! grep -Fq "$app_identity" <<< "$available_identities"; then
        print_mac_signing_setup_and_exit "Signing certificate not found in keychain: $app_identity"
    fi
    export CODESIGN_IDENTITY="$app_identity"

    if [ "$need_installer" = true ]; then
        local installer_identity
        installer_identity=$(default_mac_installer_identity)
        if [[ "$installer_identity" == *"TEAMID0000"* ]]; then
            print_mac_signing_setup_and_exit "INSTALLER_SIGN_IDENTITY is still using placeholder '$installer_identity'."
        fi
        if ! grep -Fq "$installer_identity" <<< "$available_identities"; then
            print_mac_signing_setup_and_exit "Installer signing certificate not found in keychain: $installer_identity"
        fi
        export INSTALLER_SIGN_IDENTITY="$installer_identity"
    fi
}

# Sign a completed macOS .app bundle after every executable, icon, license, and resource has been copied.
# An explicit identity must succeed. An unset or empty identity selects ad-hoc signing, never keychain discovery.
# Hardened Runtime is enabled with plugin-hosting entitlements so third-party VST3/AU/CLAP plugins can load in-process.
sign_mac_app_bundle() {
    local bundle="$1"
    local entitlements="$REPO_ROOT/assets/macos.entitlements"
    local identity="${CODESIGN_IDENTITY:--}"
    local timestamp_arg
    timestamp_arg=$(mac_codesign_timestamp_arg "$identity")

    if [ "$BASH_PLATFORM" != mac ]; then
        print_error "macOS app signing is only supported on macOS."
        return 1
    fi
    plutil -lint -s "$entitlements" || return
    plutil -lint -s "$bundle/Contents/Info.plist" || return
    print_magenta "Signing $(basename "$bundle") with identity: $identity"
    codesign --force --sign "$identity" --options runtime "$timestamp_arg" \
        --entitlements "$entitlements" "$bundle" || return
    codesign --verify --deep --strict --verbose=2 "$bundle" || return
}

# Sign a standalone macOS CLI Mach-O executable with Hardened Runtime and plugin-hosting entitlements.
# Arguments: <binary-path> <bundle-identifier>
sign_mac_cli_binary() {
    local binary="$1"
    local identifier="$2"
    local entitlements="$REPO_ROOT/assets/macos.entitlements"
    local identity="${CODESIGN_IDENTITY:--}"
    local timestamp_arg
    timestamp_arg=$(mac_codesign_timestamp_arg "$identity")

    if [ "$BASH_PLATFORM" != mac ]; then
        print_error "macOS CLI signing is only supported on macOS."
        return 1
    fi
    if [ ! -f "$binary" ]; then
        print_error "CLI binary not found for signing: $binary"
        return 1
    fi
    if [ -z "$identifier" ]; then
        print_error "A Mach-O identifier is required when signing $binary."
        return 1
    fi
    plutil -lint -s "$entitlements" || return
    print_magenta "Signing $(basename "$binary") ($identifier) with identity: $identity"
    codesign --force --sign "$identity" --identifier "$identifier" --options runtime "$timestamp_arg" \
        --entitlements "$entitlements" "$binary" || return
    codesign --verify --strict --verbose=2 "$binary" || return
}

# Sign a loadable macOS plugin bundle (.vst3 or .component) with Hardened Runtime and no entitlements.
# Loadable MH_BUNDLE binaries inherit process entitlements from their host DAW and must not carry host entitlements.
sign_mac_plugin_bundle() {
    local bundle="$1"
    local identity="${CODESIGN_IDENTITY:--}"
    local timestamp_arg
    timestamp_arg=$(mac_codesign_timestamp_arg "$identity")

    if [ "$BASH_PLATFORM" != mac ]; then
        print_error "macOS plugin signing is only supported on macOS."
        return 1
    fi
    if [ ! -d "$bundle" ]; then
        print_error "Plugin bundle not found for signing: $bundle"
        return 1
    fi
    plutil -lint -s "$bundle/Contents/Info.plist" || return
    print_magenta "Signing $(basename "$bundle") with identity: $identity"
    codesign --force --sign "$identity" --options runtime "$timestamp_arg" "$bundle" || return
    codesign --verify --deep --strict --verbose=2 "$bundle" || return
}

# Submit a signed macOS .pkg, bundle (.app/.vst3/.component), or CLI binary to Apple's notary service and wait.
# Pass --staple as the second argument to staple the notarization ticket to bundles and .pkg installers.
notarize_mac_artifact() (
    # Credentials must never reach shell traces. The subshell preserves the caller's tracing state on every exit.
    set +x
    local artifact="$1"
    local staple_flag="${2:-}"
    if [ "$BASH_PLATFORM" != mac ]; then
        print_error "macOS notarization is only supported on macOS."
        return 1
    fi
    if [ ! -e "$artifact" ]; then
        print_error "Artifact to notarize does not exist: $artifact"
        return 1
    fi
    require_command xcrun

    local auth_args=()
    if [ -n "${APPLE_APP_SPECIFIC_PASSWORD:-}" ]; then
        if [ "$APPLE_ID" = "developer@example.com" ] || [ "$APPLE_TEAM_ID" = "TEAMID0000" ]; then
            print_mac_signing_setup_and_exit "APPLE_ID or APPLE_TEAM_ID is still set to its placeholder value."
        fi
        auth_args=(--apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_SPECIFIC_PASSWORD")
    else
        if [ "$APPLE_TEAM_ID" = "TEAMID0000" ] && [ "$APPLE_NOTARY_PROFILE" = "audiobatch-notary" ]; then
            print_mac_signing_setup_and_exit "Notarization credentials are still set to placeholder defaults."
        fi
        auth_args=(--keychain-profile "$APPLE_NOTARY_PROFILE")
    fi

    case "$artifact" in
        *.pkg)
            if ! pkgutil --check-signature "$artifact" > /dev/null 2>&1; then
                print_mac_signing_setup_and_exit "Cannot notarize unsigned installer package: $artifact"
            fi
            ;;
        *)
            local signature_details
            signature_details=$(codesign -dv "$artifact" 2>&1 || true)
            if grep -Fq "Signature=adhoc" <<< "$signature_details" || ! grep -Fq "Authority=Developer ID" <<< "$signature_details"; then
                print_mac_signing_setup_and_exit "Artifact must be signed with a Developer ID certificate before notarization: $artifact"
            fi
            ;;
    esac

    local submit_path="$artifact"
    local temp_dir=""
    case "$artifact" in
        *.pkg | *.dmg | *.zip) ;;
        *)
            temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/audiobatch-notary.XXXXXXXX")
            submit_path="$temp_dir/$(basename "$artifact").zip"
            /usr/bin/ditto -c -k --keepParent "$artifact" "$submit_path"
            ;;
    esac

    print_magenta "Submitting $(basename "$artifact") to Apple notary service..."
    local status=0
    xcrun notarytool submit "$submit_path" "${auth_args[@]}" --wait || status=$?
    if [ -n "$temp_dir" ]; then
        rm -rf "$temp_dir"
    fi
    if [ "$status" -ne 0 ]; then
        print_error "Notarization failed for $artifact"
        return "$status"
    fi

    if [ "$staple_flag" = "--staple" ]; then
        case "$artifact" in
            *.pkg | *.dmg | *.app | *.vst3 | *.component)
                print_magenta "Stapling notarization ticket to $(basename "$artifact")..."
                xcrun stapler staple "$artifact" || return
                xcrun stapler validate "$artifact" || return
                ;;
        esac
    fi

    case "$artifact" in
        *.pkg)
            spctl --assess -vv --type install "$artifact" || return
            ;;
        *.app)
            spctl --assess -vv --type execute "$artifact" || return
            ;;
    esac
    print_green "Notarized: $artifact"
)

# Require compiler support for the fixed M1 distribution baseline without an early-exiting output pipeline.
require_mac_dist_rustc() {
    require_command rustc
    local configuration
    if ! configuration=$(rustc --target aarch64-apple-darwin --print cfg -C target-cpu=apple-m1 2>&1); then
        print_error_and_exit "Cannot check Rust M1 distribution support: $configuration"
    fi
    if [[ "$configuration" == *"not a recognized processor"* ]]; then
        print_error_and_exit "Installed rustc does not support the required apple-m1 distribution baseline."
    fi
}

# Accept the historical CPU option only for the fixed distribution baseline.
require_mac_dist_cpu() {
    [ "$1" = apple-m1 ] || print_error_and_exit "Distribution installers target apple-m1 only. Use build.sh for native builds."
}

# Refuse cached CMake configurations that could silently reuse native or incompatible distribution targets.
require_mac_dist_cmake_cache() {
    local cache="$1/CMakeCache.txt" entry key value
    [ -f "$cache" ] || print_error_and_exit "Missing distribution configuration. Rebuild without --reuse or --no-build."
    for entry in \
        'CMAKE_BUILD_TYPE=Release' \
        'CMAKE_OSX_ARCHITECTURES=arm64' \
        'CMAKE_OSX_DEPLOYMENT_TARGET=15.0' \
        'AUDIOBATCH_NATIVE_CPU=OFF' \
        'AUDIOBATCH_TARGET_CPU=apple-m1'; do
        key="${entry%%=*}"
        value=$(awk -F= -v key="$key" '$1 ~ "^" key ":[A-Z_]+$" { print substr($0, index($0, "=") + 1) }' "$cache")
        if [ "$value" != "${entry#*=}" ]; then
            print_error_and_exit "Incompatible distribution configuration: $entry. Rebuild without --reuse or --no-build."
        fi
    done
}

# Hash the exact build artifact, including all bundle resources and symlinks, before staging changes its signature.
mac_dist_artifact_hash() {
    local artifact="$1"
    if [ -d "$artifact" ]; then
        COPYFILE_DISABLE=1 /usr/bin/tar -cf - -C "$artifact" . | shasum -a 256 | cut -d ' ' -f 1
    elif [ -f "$artifact" ]; then
        shasum -a 256 "$artifact" | cut -d ' ' -f 1
    else
        print_error "Distribution artifact is missing: $artifact"
        return 1
    fi
}

# Record a successful fixed-baseline build or require its unchanged bytes when packaging without a build.
# Arguments: <stamp> <artifact> <version> <no-build>
verify_mac_dist_artifact() {
    local stamp="$1" artifact="$2" version="$3" no_build="$4" identity stored
    identity="arm64 apple-m1 macOS-15.0 Release $version $(mac_dist_artifact_hash "$artifact")" || return
    if [ "$no_build" = true ]; then
        stored=""
        if [ -f "$stamp" ]; then
            IFS= read -r stored < "$stamp" || true
        fi
        [ "$stored" = "$identity" ] || print_error_and_exit "Unverified or changed distribution artifact: $artifact. Rebuild without --no-build."
    else
        printf '%s\n' "$identity" > "$stamp"
    fi
}

# Build a non-relocatable macOS component .pkg for a staged root containing one bundle (.app, .vst3, or .component).
# Disabling BundleIsRelocatable prevents macOS Installer from redirecting installs into a developer's build folder.
build_mac_bundle_component_pkg() {
    local component_root="$1"
    local identifier="$2"
    local version="$3"
    local output_pkg="$4"
    local scripts_dir="${5:-}"
    local component_plist="${output_pkg%.pkg}-component.plist"

    pkgbuild --analyze --root "$component_root" "$component_plist" > /dev/null
    plutil -replace 0.BundleIsRelocatable -bool false "$component_plist"
    plutil -replace 0.BundleIsVersionChecked -bool false "$component_plist"
    plutil -lint -s "$component_plist"

    local pkg_args=(
        --root "$component_root"
        --component-plist "$component_plist"
        --identifier "$identifier"
        --version "$version"
        --install-location /
    )
    if [ -n "$scripts_dir" ]; then
        pkg_args+=(--scripts "$scripts_dir")
    fi
    pkgbuild "${pkg_args[@]}" "$output_pkg" > /dev/null
}

# Assemble, optionally sign with Developer ID Installer, and optionally notarize + staple a productbuild .pkg archive.
finalize_mac_distribution_pkg() {
    local distribution_xml="$1"
    local resources_dir="$2"
    local packages_dir="$3"
    local output_pkg="$4"
    local sign_pkg="$5"
    local notarize_pkg="$6"

    mkdir -p "$(dirname "$output_pkg")"
    rm -f "$output_pkg"

    local product_args=(
        --distribution "$distribution_xml"
        --resources "$resources_dir"
        --package-path "$packages_dir"
    )
    if [ "$sign_pkg" = true ]; then
        product_args+=(--sign "$INSTALLER_SIGN_IDENTITY" --timestamp)
    fi
    productbuild "${product_args[@]}" "$output_pkg" > /dev/null

    if [ "$sign_pkg" = true ]; then
        pkgutil --check-signature "$output_pkg"
    fi
    if [ "$notarize_pkg" = true ]; then
        notarize_mac_artifact "$output_pkg" --staple
    fi
}

# Enable calling functions directly as arguments:
# ./common.sh print_green "hello"
# shellcheck disable=SC2128
if [ $# -gt 0 ] && [ "$0" = "$BASH_SOURCE" ]; then
    "$@"
fi
