#!/bin/bash
set -eo pipefail

USAGE="Usage: $0 [OPTIONS]

Bump the C++ application, Rust workspace, and shared test plugin project versions together.
Default to a patch bump, and reset lower version components for a major or minor bump.
Run cargo update --workspace from rust/ to refresh the workspace packages in Cargo.lock
without upgrading external dependencies.

OPTIONS: All options are optional
    -h | --help
        Display these instructions.

    -m | --major
        Bump the major version.

    -n | --minor
        Bump the minor version.

    -c | --commit
        Stage the four version files and commit with the message \"Version X.X.X\".
        Create an annotated tag vX.X.X on that commit with the same message.
        Leave unrelated staged files out of the commit.
"

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./common.sh
source "$DIR/common.sh"

BUMP="patch"
BUMP_SELECTED=false
COMMIT=false
for argument in "$@"; do
    case "$argument" in
        -h | --help)
            print_usage_and_exit 0
            ;;
        -c | --commit)
            COMMIT=true
            continue
            ;;
        -m | --major)
            NEXT_BUMP=major
            ;;
        -n | --minor)
            NEXT_BUMP=minor
            ;;
        *)
            print_error_and_exit "Unknown argument: $argument. See --help."
            ;;
    esac
    if [ "$BUMP_SELECTED" = true ]; then
        print_error_and_exit "Choose only one version bump option."
    fi
    BUMP=$NEXT_BUMP
    BUMP_SELECTED=true
done

require_command perl
require_command cargo
if [ "$COMMIT" = true ]; then
    require_command git
fi

VERSION_FILES=(cpp/CMakeLists.txt rust/Cargo.toml rust/Cargo.lock test-plugin/CMakeLists.txt)

# Validate every declaration before changing any file.
# Match only project metadata, leaving dependency versions, examples, and fake plugin data untouched.
perl - "$BUMP" "$DIR/cpp/CMakeLists.txt" "$DIR/rust/Cargo.toml" "$DIR/test-plugin/CMakeLists.txt" << 'PERL'
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

cd "$DIR/rust"
if ! cargo update --workspace; then
    print_error_and_exit "Versions were changed, but Cargo.lock could not be refreshed. Run cargo update --workspace in rust/."
fi
print_green "Project versions and Rust lockfile updated."

if [ "$COMMIT" = true ]; then
    VERSION=$(perl -ne 'print $1 if /^version = "([0-9]+\.[0-9]+\.[0-9]+)"/' "$DIR/rust/Cargo.toml")
    cd "$DIR"
    git add -- "${VERSION_FILES[@]}"
    git commit --only -m "Version $VERSION" -- "${VERSION_FILES[@]}"
    git tag -a "v$VERSION" -m "Version $VERSION" HEAD
fi
