#!/bin/bash
# Audits exact build-tree format packages with instrumented Rust, native wrappers, and isolated callback hosts.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=image-control-plugin/rust/common.sh
source "$SCRIPT_DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Run process-isolated native RealtimeSanitizer checks against ImageControl RS build-tree packages.
VST3 is the default. CLAP and AU have independent hosts and callback evidence.
No dependency reports are suppressed. These checks do not establish audio deadlines.
Requires macOS, nightly Rust, uv, jq, and a matching Homebrew LLVM runtime.

OPTIONS:
    -h | --help
        Display these instructions without building or checking dependencies.

    -f | --format <vst3|clap|au|all>
        Select independently audited formats (default: vst3).
        AU requires the native host's RTSan-only detector and marked render/reset cases.

    -G | --diagnostic
        Verify the pinned AU dependency findings as BLOCKED audits, never clean results.
        Only --format au accepts this mode.

    -b | --bundle <path>
        Audit this exact instrumented build-tree package. Requires --no-build and a single format, not all.
        IMAGE_CONTROL_OWN_BUNDLE remains the VST3 default override.

    -N | --no-build
        Reuse the supplied/default package instead of building the instrumented Release plugin.
        The caller must supply an instrumented package. The callback host is still built.
        --skip-build is a deprecated alias for --no-build.

CASES:
    Detector controls for each format, cold automation, and reset.
    VST3 also checks concurrent detached state save/restore and native editor lifecycle.
    CLAP also checks concurrent detached state save/restore.
    AU requires separate native wrapper qualification and never inherits VST3/CLAP results.

ENVIRONMENT:
    AUDIOBATCH_RTSAN_TOOLCHAIN  Installed nightly toolchain, default nightly.
    AUDIOBATCH_RTSAN_CLANG      Matching clang executable, default Homebrew LLVM.
"
parse_arguments() {
    BUNDLE=""
    USE_DEFAULT_BUNDLE=true
    FORMAT=vst3
    DIAGNOSTIC=false
    SKIP_BUILD=false
    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                printf '%s\n' "$USAGE"
                exit 0
                ;;
            -b | --bundle)
                [ $# -ge 2 ] || print_error_and_exit "--bundle requires a path"
                BUNDLE="$2"
                USE_DEFAULT_BUNDLE=false
                shift
                ;;
            -f | --format)
                [ $# -ge 2 ] && [ -n "$2" ] && [[ "$2" != -* ]] || print_error_and_exit "--format requires vst3, clap, au, or all"
                FORMAT="$2"
                shift
                ;;
            -G | --diagnostic) DIAGNOSTIC=true ;;
            -N | --no-build | --skip-build) SKIP_BUILD=true ;;
            *) print_error_and_exit "Unknown option: $1. See --help." ;;
        esac
        shift
    done
    case "$FORMAT" in
        vst3 | clap | au) FORMATS=("$FORMAT") ;;
        all)
            [ "$USE_DEFAULT_BUNDLE" = true ] || print_error_and_exit "--bundle requires a single --format, not all"
            FORMATS=(vst3 clap au)
            ;;
        *) print_error_and_exit "Unsupported --format: $FORMAT. Expected vst3, clap, au, or all." ;;
    esac
    if [ "$DIAGNOSTIC" = true ] && [ "$FORMAT" != au ]; then
        print_error_and_exit "--diagnostic requires --format au"
    fi
    if [ "$USE_DEFAULT_BUNDLE" = true ] && [ "$FORMAT" = vst3 ] && [ -n "${IMAGE_CONTROL_OWN_BUNDLE:-}" ]; then
        BUNDLE="$IMAGE_CONTROL_OWN_BUNDLE"
        USE_DEFAULT_BUNDLE=false
    fi
    if [ "$USE_DEFAULT_BUNDLE" = false ] && [ "$SKIP_BUILD" = false ]; then
        print_error_and_exit "An explicit bundle requires --no-build and a matching instrumented package. Omit --bundle to build and audit the instrumented plugin."
    fi
}

# Missing explicitly reused packages fail before native toolchain lookup, preserving the no-build contract.
check_bundle() {
    local description
    case "$CURRENT_FORMAT" in
        vst3) description=VST3 ;;
        clap) description=CLAP ;;
        au) description="Audio Unit" ;;
    esac
    [ -d "$BUNDLE/Contents" ] || print_error_and_exit "Build-tree $description is missing: $BUNDLE"
    BUNDLE=$(cd "$BUNDLE" && pwd)
}

configure_toolchain() {
    TOOLCHAIN=${AUDIOBATCH_RTSAN_TOOLCHAIN:-nightly}
    CLANG=${AUDIOBATCH_RTSAN_CLANG:-$(brew --prefix llvm)/bin/clang}
    RUST_VERSION=$(rustc +"$TOOLCHAIN" --version --verbose)
    local LLVM_VERSION
    LLVM_VERSION=$(printf '%s\n' "$RUST_VERSION" | rg '^LLVM version: ' | cut -d ' ' -f 3)
    [[ $("$CLANG" --version) == *"clang version $LLVM_VERSION"* ]] || print_error_and_exit "Rust and clang LLVM versions must match"
    HOST=$(printf '%s\n' "$RUST_VERSION" | rg '^host: ' | cut -d ' ' -f 2)
    case "$HOST" in
        aarch64-apple-darwin) ARCHITECTURE=arm64 ;;
        x86_64-apple-darwin) ARCHITECTURE=x86_64 ;;
        *) print_error_and_exit "Unsupported native RTSan host: $HOST" ;;
    esac
    RUNTIME=$("$CLANG" --print-file-name=libclang_rt.rtsan_osx_dynamic.dylib)
    [ -f "$RUNTIME" ] || print_error_and_exit "Missing native RTSan runtime"
    export CARGO_TARGET_DIR="$SCRIPT_DIR/target/rtsan-own"
    export CARGO_INCREMENTAL=0
    # Explicit target selection keeps instrumentation out of build scripts and proc macros.
    # Encoded flags preserve clang/runtime paths containing spaces as individual rustc arguments.
    local flags=(--cfg audiobatch_rtsan -Zsanitizer=realtime -Zexternal-clangrt -Cdebuginfo=2 -Cforce-frame-pointers=yes
        "-Clinker=$CLANG" "-Clink-arg=$RUNTIME" "-Clink-arg=-Wl,-rpath,$(dirname "$RUNTIME")")
    printf -v CARGO_ENCODED_RUSTFLAGS '%s\x1f' "${flags[@]}"
    export CARGO_ENCODED_RUSTFLAGS="${CARGO_ENCODED_RUSTFLAGS%$'\x1f'}"
    RTSAN_SYMBOLIZER_PATH="$(dirname "$CLANG")/llvm-symbolizer"
    export RTSAN_SYMBOLIZER_PATH
    export MACOSX_DEPLOYMENT_TARGET=15.0
    unset RUSTFLAGS RTSAN_OPTIONS DYLD_INSERT_LIBRARIES
    ARTIFACTS="$CARGO_TARGET_DIR/artifacts"
    mkdir -p "$ARTIFACTS"
    printf '%s\n' "$RUST_VERSION" "Runtime: $RUNTIME" "Formats: $FORMAT" > "$ARTIFACTS/versions.txt"
}

# Compile the actual cdylib and AU static archive with the same target, runtime, and instrumentation as the hosts.
build_realtime_plugin() {
    cargo +"$TOOLCHAIN" build --locked --manifest-path "$REPO_ROOT/Cargo.toml" --target "$HOST" --release \
        -p image-control-plugin-rs --lib --message-format=json-render-diagnostics > "$ARTIFACTS/plugin-build.jsonl"
}

# The noninstrumented bundler only copies the explicit instrumented target/profile artifacts. It never loads them.
package_rust_format() {
    CARGO_ENCODED_RUSTFLAGS='' RUSTFLAGS='' CARGO_TARGET_DIR="$CARGO_TARGET_DIR/$HOST" \
        cargo +"$TOOLCHAIN" run --locked --manifest-path "$REPO_ROOT/Cargo.toml" -p image-control-xtask -- \
        bundle image-control-plugin-rs --release --no-build --format "$CURRENT_FORMAT"
    if [ "$USE_DEFAULT_BUNDLE" = true ]; then
        BUNDLE="$CARGO_TARGET_DIR/$HOST/bundled/ImageControl RS.$CURRENT_FORMAT"
    fi
}

# Native AU code and the Rust archive use one architecture, deployment target, LLVM, runtime, and Release profile.
# The host must advertise genuine marked scenarios. A normal qualification host cannot produce AU realtime evidence.
build_audio_unit() {
    local directory="$CARGO_TARGET_DIR/au/$ARCHITECTURE/release" clang_directory version native_flags
    clang_directory=$(dirname "$CLANG")
    version=$(cargo +"$TOOLCHAIN" metadata --locked --manifest-path "$REPO_ROOT/Cargo.toml" --no-deps --format-version 1 |
        jq -r '.packages[] | select(.name == "image-control-plugin-rs") | .version')
    native_flags="-fsanitize=realtime -g -fno-omit-frame-pointer"
    cmake -S "$SCRIPT_DIR/au" -B "$directory" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=$ARCHITECTURE" -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
        "-DCMAKE_C_COMPILER=$CLANG" "-DCMAKE_CXX_COMPILER=$clang_directory/clang++" \
        "-DCMAKE_OBJC_COMPILER=$CLANG" "-DCMAKE_OBJCXX_COMPILER=$clang_directory/clang++" \
        "-DCMAKE_C_FLAGS=$native_flags" "-DCMAKE_CXX_FLAGS=$native_flags" \
        "-DCMAKE_OBJC_FLAGS=$native_flags" "-DCMAKE_OBJCXX_FLAGS=$native_flags" \
        "-DCMAKE_BUILD_RPATH=$(dirname "$RUNTIME")" \
        "-DIMAGE_CONTROL_RUST_LIBRARY=$CARGO_TARGET_DIR/$HOST/release/libimage_control_plugin_rs.a" \
        "-DIMAGE_CONTROL_VERSION=$version" -DIMAGE_CONTROL_AU_BUILD_TESTS=ON -DIMAGE_CONTROL_AU_RTSAN=ON
    cmake --build "$directory" --config Release --target ImageControlRSAURealtime --parallel 4
    EXECUTABLE="$directory/ImageControlRSAURealtime"
    if [ "$USE_DEFAULT_BUNDLE" = true ]; then
        BUNDLE="$directory/ImageControl RS.component"
    fi
}

build_realtime_host() {
    local example=realtime_host
    [ "$CURRENT_FORMAT" != clap ] || example=clap_host
    cargo +"$TOOLCHAIN" build --locked --manifest-path "$REPO_ROOT/Cargo.toml" --target "$HOST" -p image-control-plugin-rs \
        --example "$example" --message-format=json-render-diagnostics > "$ARTIFACTS/$example-build.jsonl"
    EXECUTABLE=$(jq -r "select(.reason == \"compiler-artifact\" and .target.name == \"$example\") | .executable // empty" "$ARTIFACTS/$example-build.jsonl")
    [ -x "$EXECUTABLE" ] || print_error_and_exit "No packaged realtime host was built"
}

run_case() {
    local name="$1"
    shift
    CARGO_TARGET_DIR="$BUNDLE_TARGET_DIRECTORY" uv run --script "$REPO_ROOT/tests/realtime_sanitizer.py" --executable "$EXECUTABLE" \
        --case "own-$CURRENT_FORMAT-$name" --artifacts "$ARTIFACTS" --timeout 45 \
        --argument=--case --argument="$name" --argument=--bundle --argument="$BUNDLE" "$@"
}

run_cases() {
    local name
    if [ "$CURRENT_FORMAT" = au ]; then
        local help
        help=$("$EXECUTABLE" --help) || print_error_and_exit "Native AU host lacks the RTSan-only --case interface. No AU clean result is available."
        for name in clean-control allocation-control mutex-control au-cold-automation au-reset; do
            [[ "$help" == *"$name"* ]] || print_error_and_exit "Native AU host lacks marked RTSan case $name. Enable IMAGE_CONTROL_AU_RTSAN with nonblocking render/reset boundaries. No AU clean result is available."
        done
    fi
    run_case clean-control --require-output 'CASE_OK clean-control'
    local allocation_stack=rust_allocation_region
    [ "$CURRENT_FORMAT" != au ] || allocation_stack=allocation_region
    run_case allocation-control --control --expect malloc --stack "$allocation_stack"
    run_case mutex-control --control --expect pthread_mutex_lock --stack mutex_region
    case "$CURRENT_FORMAT" in
        vst3)
            for name in native-cold-automation native-reset native-state-interaction native-editor-lifecycle; do
                run_case "$name" --require-output "CASE_OK $name"
            done
            ;;
        clap)
            for name in clap-cold-automation clap-reset clap-state-interaction; do
                run_case "$name" --require-output "CASE_OK $name"
            done
            ;;
        au)
            if [ "$DIAGNOSTIC" = true ]; then
                run_case au-cold-automation --expect malloc --stack 'Clap::Plugin::AlwaysAudioThread'
                run_case au-reset --expect pthread_mutex_lock --stack 'ausdk::AUMethodReset' --stack au_reset_region
            else
                for name in au-cold-automation au-reset; do
                    run_case "$name" --require-output "CASE_OK $name"
                done
            fi
            ;;
    esac
}

audit_plugin() {
    parse_arguments "$@"
    [ "$BASH_PLATFORM" = mac ] || print_error_and_exit "Native realtime checks require macOS."
    image_control_resolve_target_directory
    BUNDLE_TARGET_DIRECTORY="$CARGO_TARGET_DIR"
    local requested_bundle="$BUNDLE" extension
    if [ "$SKIP_BUILD" = true ]; then
        for CURRENT_FORMAT in "${FORMATS[@]}"; do
            BUNDLE="$requested_bundle"
            if [ "$USE_DEFAULT_BUNDLE" = true ]; then
                extension="$CURRENT_FORMAT"
                [ "$CURRENT_FORMAT" != au ] || extension=component
                BUNDLE="$BUNDLE_TARGET_DIRECTORY/bundled/ImageControl RS.$extension"
            fi
            check_bundle
        done
    fi
    configure_toolchain
    if [ "$SKIP_BUILD" = false ]; then build_realtime_plugin; fi
    for CURRENT_FORMAT in "${FORMATS[@]}"; do
        BUNDLE="$requested_bundle"
        if [ "$CURRENT_FORMAT" = au ]; then
            # The CMake host and its linked Rust archive must be instrumented even when reusing an external component.
            if [ "$SKIP_BUILD" = true ]; then build_realtime_plugin; fi
            build_audio_unit
        else
            if [ "$SKIP_BUILD" = false ]; then package_rust_format; fi
            build_realtime_host
        fi
        if [ "$SKIP_BUILD" = true ] && [ "$USE_DEFAULT_BUNDLE" = true ]; then
            extension="$CURRENT_FORMAT"
            [ "$CURRENT_FORMAT" != au ] || extension=component
            BUNDLE="$BUNDLE_TARGET_DIRECTORY/bundled/ImageControl RS.$extension"
        fi
        check_bundle
        printf '%s\n' "Format: $CURRENT_FORMAT" "Bundle: $BUNDLE" >> "$ARTIFACTS/versions.txt"
        run_cases
    done
}

audit_plugin "$@"
