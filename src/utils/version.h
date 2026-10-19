/// Build-time version information for the application.
/// The version namespace exposes the application name, version, branch, commit, and build date
/// as constants filled in from preprocessor definitions set by CMake.

#pragma once

/// Build metadata injected by CMake at compile time.
namespace audiobatch::version
{
constexpr auto APP_NAME {BUILDTIME_APP_NAME};
constexpr auto BRANCH {BUILDTIME_BRANCH};
constexpr auto BUILD_NAME {BUILDTIME_BUILD_NAME};
constexpr auto COMMIT {BUILDTIME_COMMIT};
constexpr auto DATE {BUILDTIME_DATE};
constexpr auto VERSION_INFO {BUILDTIME_VERSION_INFO};
constexpr auto VERSION_NUMBER {BUILDTIME_VERSION_NUMBER};

/// Version, build date, commit, and branch, as printed by the --version flag.
constexpr auto VERSION_STRING {BUILDTIME_VERSION_STRING};
}  // namespace audiobatch::version
