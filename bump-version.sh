#!/bin/bash
set -eo pipefail

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$DIR/common.sh"

USAGE="Usage: $0 [OPTIONS]

Bump the C++ application, Rust application workspace, and shared test plugin project versions together.
ImageControl keeps its independent versions.
Default to a patch bump, and reset lower version components for a major or minor bump.
Run cargo update --workspace from the repository root to refresh the workspace packages in Cargo.lock,
and refresh the application packages in rust/fuzz/Cargo.lock, without upgrading external dependencies.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -m | --major
        Bump the major version.

    -n | --minor
        Bump the minor version.

    -c | --commit
        Stage the five version files and commit with the message \"Version X.X.X\".
        Create an annotated tag vX.X.X on that commit with the same message.
        Leave unrelated staged files out of the commit.
"

# Application packages that the separate fuzz workspace pins in rust/fuzz/Cargo.lock.
FUZZ_LOCKED_PACKAGES=(audiobatch-cli audiobatch-core serato-tags)

# Handle version selection and help before checking tools or editing files.
parse_arguments() {
    local argument next_bump bump_selected=false
    BUMP="patch"
    CREATE_COMMIT=false
    for argument in "$@"; do
        case "$argument" in
            -h | --help)
                print_usage_and_exit 0
                ;;
            -c | --commit)
                CREATE_COMMIT=true
                continue
                ;;
            -m | --major)
                next_bump=major
                ;;
            -n | --minor)
                next_bump=minor
                ;;
            *)
                print_error_and_exit "Unknown argument: $argument. See --help."
                ;;
        esac
        if [ "$bump_selected" = true ]; then
            print_error_and_exit "Choose only one version bump option."
        fi
        BUMP=$next_bump
        bump_selected=true
    done
}

# Validate every declaration before changing any file.
# Match only project metadata, leaving dependency versions, examples, and fake plugin data untouched.
bump_project_versions() {
    local bump="$1"
    perl - "$bump" "$REPO_ROOT/cpp/CMakeLists.txt" "$REPO_ROOT/Cargo.toml" "$REPO_ROOT/test-plugin/CMakeLists.txt" << 'PERL'
use strict;
use warnings;

my ($bump, @paths) = @ARGV;
my @contents;
my @patterns;
my $current;
for my $path (@paths) {
    open my $input, '<', $path or die "Cannot read $path: $!\n";
    my $content = do { local $/; <$input> };
    close $input or die "Cannot close $path: $!\n";
    my $pattern = $path =~ /Cargo\.toml$/
        ? qr/(^\[workspace\.package\]\s*\r?\nversion = ")([0-9]+\.[0-9]+\.[0-9]+)(")/m
        : qr/(^project\(\s*"[^"]+"\s+VERSION )([0-9]+\.[0-9]+\.[0-9]+)(\s)/m;
    my @matches = $content =~ /$pattern/g;
    @matches == 3 or die "Expected exactly one project version in $path\n";
    my $version = $matches[1];
    $current //= $version;
    $version eq $current or die "Project versions disagree: $path has $version, expected $current\n";
    push @contents, $content;
    push @patterns, $pattern;
}

my ($major, $minor, $patch) = split /\./, $current;
if ($bump eq 'major') {
    ++$major;
    $minor = $patch = 0;
} elsif ($bump eq 'minor') {
    ++$minor;
    $patch = 0;
} else {
    ++$patch;
}
my $next = join '.', $major, $minor, $patch;
for my $index (0 .. $#paths) {
    $contents[$index] =~ s/$patterns[$index]/${1}${next}${3}/;
    open my $output, '>', $paths[$index] or die "Cannot write $paths[$index]: $!\n";
    print {$output} $contents[$index] or die "Cannot write $paths[$index]: $!\n";
    close $output or die "Cannot close $paths[$index]: $!\n";
}
print "Bumped project versions: $current -> $next\n";
PERL
}

# Commit only the coordinated version files, leaving unrelated staged work alone.
commit_version() {
    local version
    local version_files=(cpp/CMakeLists.txt Cargo.toml Cargo.lock rust/fuzz/Cargo.lock test-plugin/CMakeLists.txt)
    version=$(perl -ne 'print $1 if /^version = "([0-9]+\.[0-9]+\.[0-9]+)"/' "$REPO_ROOT/Cargo.toml")
    git add -- "${version_files[@]}"
    git commit --only -m "Version $version" -- "${version_files[@]}"
    git tag -a "v$version" -m "Version $version" HEAD
}

# Update coordinated declarations and the lockfile before optionally committing their explicit paths.
update_versions() {
    parse_arguments "$@"
    require_command perl
    require_command cargo
    if [ "$CREATE_COMMIT" = true ]; then
        require_command git
    fi
    bump_project_versions "$BUMP"
    cd "$REPO_ROOT"
    if ! cargo update --workspace; then
        print_error_and_exit "Versions were changed, but Cargo.lock could not be refreshed. Run cargo update --workspace in the repository root."
    fi
    # The fuzz crates form a separate workspace with their own lockfile that pins the application packages.
    if ! cargo update --manifest-path rust/fuzz/Cargo.toml "${FUZZ_LOCKED_PACKAGES[@]/#/-p=}"; then
        print_error_and_exit "Versions were changed, but rust/fuzz/Cargo.lock could not be refreshed. Run cargo update --manifest-path rust/fuzz/Cargo.toml -p audiobatch-cli -p audiobatch-core -p serato-tags in the repository root."
    fi
    print_green "Project versions and Rust lockfiles updated."
    if [ "$CREATE_COMMIT" = true ]; then
        commit_version
    fi
}

update_versions "$@"
