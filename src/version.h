/// Build-time version information for the application.
/// The version namespace exposes the application name, version, branch, commit, and build date
/// as constants filled in from preprocessor definitions set by CMake,
/// plus makeVersionLine, which builds the --version output line entirely at compile time.

//==========================================================
// Version header
// Version information is set in preprocessor definitions
// Akseli Lukkarila
//==========================================================

#pragma once

#include <string_view>

#include <JuceHeader.h>

#include <array>
#include <cstddef>

/// Build metadata injected by CMake at compile time.
namespace version
{
constexpr auto APP_NAME = BUILDTIME_APP_NAME;
constexpr auto BRANCH = BUILDTIME_BRANCH;
constexpr auto BUILD_NAME = BUILDTIME_BUILD_NAME;
constexpr auto COMMIT = BUILDTIME_COMMIT;
constexpr auto DATE = BUILDTIME_DATE;
constexpr auto VERSION_INFO = BUILDTIME_VERSION_INFO;
constexpr auto VERSION_NUMBER = BUILDTIME_VERSION_NUMBER;

/// Null-terminated text produced at compile time by makeVersionLine.
template<std::size_t Capacity>
struct VersionLine {
    std::array<char, Capacity> characters {};
    std::size_t length = 0;

    /// Returns the text without the terminating null character.
    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        return {characters.data(), length};
    }
};

/// Joins the given string literals with single spaces at compile time, skipping empty ones.
/// The combined literal sizes always leave room for the separators and the terminating null,
/// because every literal contributes one null character that is not copied.
template<std::size_t... Sizes>
consteval auto joinNonEmpty(const char (&... parts)[Sizes])
{
    VersionLine<(Sizes + ...)> line;

    const auto append = [&line](const char* part, const std::size_t size) {
        // Each literal size includes its null terminator, so a size of one is an empty string.
        if (size <= 1) {
            return;
        }

        if (line.length > 0) {
            line.characters[line.length++] = ' ';
        }

        for (std::size_t index = 0; index + 1 < size; ++index) {
            line.characters[line.length++] = part[index];
        }
    };

    (append(parts, Sizes), ...);
    return line;
}

static_assert(joinNonEmpty("audiobatch", "", "1.0", "").view() == "audiobatch 1.0");

/// Builds the --version output line for the given executable name at compile time:
/// name, version info, git branch, and JUCE version, separated by spaces.
/// An empty branch, for example from a detached HEAD build, is left out.
template<std::size_t NameSize>
consteval auto makeVersionLine(const char (&appName)[NameSize])
{
    // The JUCE part matches the "JUCE vX.Y.Z" form of juce::SystemStats::getJUCEVersion(),
    // built from the JUCE version macros so it is available at compile time.
    return joinNonEmpty(
        appName,
        BUILDTIME_VERSION_INFO,
        BUILDTIME_BRANCH,
        "JUCE v" JUCE_STRINGIFY(JUCE_MAJOR_VERSION) "." JUCE_STRINGIFY(JUCE_MINOR_VERSION) "." JUCE_STRINGIFY(
            JUCE_BUILDNUMBER
        )
    );
}
}  // namespace version
