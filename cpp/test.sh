#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=cpp/common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Configure and build the C++ tests, then run them with CTest.
Runs unit suites and the CLI and driven GUI integration tier by default.
Uses the same platform build directories and compilation cache as build.sh.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -b | --build-type <type>
        Select a build configuration. Defaults to Debug.

    -p | --build-dir <directory>
        Use an existing CMake workspace, including Ninja, Xcode, or Visual Studio workspaces.
        A new directory uses the platform's default debug preset and the selected build type.

    -u | --unit
        Build and run only the unit suites.

    -i | --integration
        Build and run only the CLI and driven GUI integration tier.
        Cannot be combined with --unit.

    -B | --build-only
        Configure and build the selected test targets without running them.

    -N | --no-build
        Run tests from an already configured and built workspace without configuring or building it.
        Cannot be combined with --build-only.

    -W | --visible [delay-ms]
        Show the driven GUI integration window on screen, pausing after each action so you can follow it.
        The optional delay sets the pause in milliseconds,
        defaulting to and capped by the constants in tests/GuiDriver.h.
        Off by default, and meant for local runs, because the window takes the frontmost position.
        Selects the integration tier and cannot be combined with --unit.

    -v | --verbose
        Display commands being executed.
"

# Parses and validates test selection before any workspace changes.
parse_arguments() {
    BUILD_TYPE="Debug"
    BUILD_DIR=""
    TIER="all"
    NO_BUILD=false
    BUILD_ONLY=false
    VISIBLE=false
    VISIBLE_DELAY=""

    while [ $# -gt 0 ]; do
        case "$1" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -b | --build-type | -p | --build-dir)
                if [ $# -lt 2 ] || [ -z "$2" ] || [[ "$2" == -* ]]; then
                    print_error_and_exit "Missing value for $1."
                fi
                if [ "$1" = -b ] || [ "$1" = --build-type ]; then
                    BUILD_TYPE="$2"
                else
                    BUILD_DIR="$2"
                fi
                shift
                ;;
            -u | --unit | -i | --integration)
                case "$1" in
                    -u | --unit) SELECTED_TIER=unit ;;
                    -i | --integration) SELECTED_TIER=integration ;;
                esac
                if [ "$TIER" != all ] && [ "$TIER" != "$SELECTED_TIER" ]; then
                    print_error_and_exit "--unit and --integration cannot be used together."
                fi
                TIER="$SELECTED_TIER"
                ;;
            -B | --build-only)
                BUILD_ONLY=true
                ;;
            -N | --no-build)
                NO_BUILD=true
                ;;
            -W | --visible)
                VISIBLE=true
                if [ $# -ge 2 ] && [[ "$2" =~ ^[0-9]+$ ]]; then
                    VISIBLE_DELAY="$2"
                    shift
                fi
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

    if [ "$BUILD_ONLY" = true ] && [ "$NO_BUILD" = true ]; then
        print_error_and_exit "--build-only and --no-build cannot be used together."
    fi

    if [ "$VISIBLE" = true ]; then
        if [ "$TIER" = unit ]; then
            print_error_and_exit "--visible shows the GUI integration tier and cannot be used with --unit."
        fi
        TIER="integration"
        export AUDIOBATCH_GUI_TESTS_VISIBLE=1
        if [ -n "$VISIBLE_DELAY" ]; then
            export AUDIOBATCH_GUI_TESTS_STEP_DELAY_MS="$VISIBLE_DELAY"
        fi
    fi

    case "$BUILD_TYPE" in
        "" | *[!a-zA-Z0-9_-]*)
            print_error_and_exit "Build type must contain only letters, numbers, underscores, or hyphens."
            ;;
    esac
}

# Resolves the platform preset, absolute workspace, cache, and selected targets.
select_test_workspace() {
    case "$BASH_PLATFORM" in
        mac)
            PRESET="macos-debug"
            BUILD_PREFIX="cmake-build-mac"
            ;;
        windows)
            PRESET="windows-debug"
            BUILD_PREFIX="cmake-build-windows"
            ;;
        *)
            print_error_and_exit "The test build is only supported on macOS and Windows."
            ;;
    esac

    cd "$REPO"
    if [ -z "$BUILD_DIR" ]; then
        BUILD_TYPE_LOWER=$(printf '%s' "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')
        BUILD_DIR="$REPO/$BUILD_PREFIX-$BUILD_TYPE_LOWER"
    fi
    if [ "$BASH_PLATFORM" = windows ]; then
        BUILD_DIR=$(cygpath -u "$BUILD_DIR")
    fi
    if [[ "$BUILD_DIR" != /* ]]; then
        BUILD_DIR="$REPO/$BUILD_DIR"
    fi
    export CCACHE_DIR="$BUILD_DIR/ccache"

    TARGETS=(AudioBatchTests AudioBatchIntegrationTests AudioBatchCli)
    CTEST_ARGS=()
    case "$TIER" in
        unit)
            TARGETS=(AudioBatchTests)
            # Positive labels also exclude CTest's unlabelled placeholders for integration targets not built yet.
            CTEST_ARGS=(-L unit)
            ;;
        integration)
            TARGETS=(AudioBatchIntegrationTests AudioBatchCli)
            CTEST_ARGS=(-L integration)
            ;;
    esac
}

# Validates a reused workspace or configures and builds the requested test tier.
prepare_test_targets() {
    if [ "$NO_BUILD" = true ]; then
        if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] ||
            ! grep -Eiq '^BUILD_TESTING:BOOL=(ON|TRUE|1)$' "$BUILD_DIR/CMakeCache.txt"; then
            print_error_and_exit "No test-enabled workspace at $BUILD_DIR. Run without --no-build first."
        fi
        CACHED_GENERATOR=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" | tr -d '\r')
        CACHED_BUILD_TYPE=$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" | tr -d '\r')
        if [ "$CACHED_GENERATOR" = Ninja ] && [ "$CACHED_BUILD_TYPE" != "$BUILD_TYPE" ]; then
            print_error_and_exit "Workspace has a different build type. Run without --no-build to configure $BUILD_TYPE."
        fi
    else
        require_command uv "uv not found in path, it runs the shared CLI integration suite."
        print_magenta "Configuring $BUILD_TYPE tests at $BUILD_DIR..."
        if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
            if grep -Eq '^CMAKE_GENERATOR:INTERNAL=Ninja' "$BUILD_DIR/CMakeCache.txt"; then
                load_msvc_environment
            fi
            cmake -S "$REPO" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_TESTING=ON
        else
            cmake --preset "$PRESET" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_TESTING=ON
        fi
        print_magenta "Building $TIER test targets..."
        cmake --build "$BUILD_DIR" --target "${TARGETS[@]}" --config "$BUILD_TYPE"
    fi
}

run_tests() {
    parse_arguments "$@"
    select_test_workspace
    prepare_test_targets
    if [ "$BUILD_ONLY" = true ]; then
        return 0
    fi
    print_magenta "Running $TIER tests ($BUILD_TYPE)..."
    ctest --test-dir "$BUILD_DIR" --build-config "$BUILD_TYPE" "${CTEST_ARGS[@]}" \
        --output-on-failure --no-tests=error
}

run_tests "$@"
