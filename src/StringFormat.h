/// fmt formatting support for JUCE types and a format helper that returns juce::String.
/// Provides fmt::formatter specializations for juce::String and juce::File,
/// and utils::format, a compile-time checked wrapper around fmt::format
/// that round-trips text through UTF-8 so results are safe for any juce::String content.
/// Prefer utils::format over chained juce::String concatenation when building display and log text.

#pragma once

#include <fmt/format.h>
#include <string_view>

#include <JuceHeader.h>

#include <string>
#include <utility>

/// Formats a juce::String as its UTF-8 text.
/// Inherits the std::string_view formatter so standard format specs
/// like fill, alignment, and width keep working, for example "{:>10}".
template<>
struct fmt::formatter<juce::String> : fmt::formatter<std::string_view> {
    /// Views the string's internal UTF-8 representation without copying
    /// and delegates to the string_view formatter.
    template<typename FormatContext>
    auto format(const juce::String& value, FormatContext& context) const
    {
        const std::string_view utf8Text {value.toRawUTF8(), value.getNumBytesAsUTF8()};
        return fmt::formatter<std::string_view>::format(utf8Text, context);
    }
};

/// Formats a juce::File as its full path,
/// so messages can pass files directly instead of calling getFullPathName() at every site.
template<>
struct fmt::formatter<juce::File> : fmt::formatter<juce::String> {
    /// Formats the file's absolute path using the juce::String formatter.
    template<typename FormatContext>
    auto format(const juce::File& value, FormatContext& context) const
    {
        return fmt::formatter<juce::String>::format(value.getFullPathName(), context);
    }
};

namespace utils
{
/// Formats the arguments with fmt using a compile-time checked format string
/// and returns the result as a juce::String decoded from UTF-8.
/// Prefer this over chained juce::String concatenation when building display or log text.
template<typename... Args>
[[nodiscard]] juce::String format(fmt::format_string<Args...> formatString, Args&&... args)
{
    const std::string result = fmt::format(formatString, std::forward<Args>(args)...);
    return juce::String::fromUTF8(result.c_str(), static_cast<int>(result.size()));
}
}  // namespace utils
