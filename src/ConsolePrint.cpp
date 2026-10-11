/// Implementation of the console output helpers.
/// Covers the terminal and NO_COLOR detection that decides whether styles are applied,
/// including enabling virtual terminal processing on Windows consoles,
/// and the Error and Warning message helpers that keep leading indentation.

#include "ConsolePrint.h"

#include <algorithm>
#include <cstdint>

#if JUCE_WINDOWS
#include <juce_core/native/juce_BasicNativeHeaders.h>
#else
#include <unistd.h>
#endif

namespace console::detail
{
/// Returns true when the NO_COLOR convention (https://no-color.org) asks for plain output.
static bool isNoColorRequested()
{
    return juce::SystemStats::getEnvironmentVariable("NO_COLOR", {}).isNotEmpty();
}

/// Returns true when the stream is connected to something that renders ANSI escape codes.
static bool streamSupportsColor(std::FILE* stream)
{
#if JUCE_WINDOWS
    if (stream != stdout && stream != stderr) {
        return false;
    }

    auto* const handle = GetStdHandle(stream == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    switch (GetFileType(handle)) {
        case FILE_TYPE_CHAR: {
            // A real console. Enable escape code processing, which older consoles have off by default.
            DWORD mode = 0;
            if (GetConsoleMode(handle, &mode) == 0) {
                return false;
            }

            return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0
                || SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        }
        case FILE_TYPE_PIPE:
            // Git Bash (mintty) connects programs through pipes and sets TERM.
            return juce::SystemStats::getEnvironmentVariable("TERM", {}).isNotEmpty();
        default:
            return false;
    }
#else
    return isatty(fileno(stream)) != 0;
#endif
}

/// Checks colour support for a stream, ignoring any cached result.
static bool computeColorEnabled(std::FILE* stream)
{
    return !isNoColorRequested() && streamSupportsColor(stream);
}

/// Splits a message into its leading whitespace and the remaining text.
static std::pair<std::string_view, std::string_view> splitLeadingWhitespace(const std::string_view message)
{
    const auto indentSize = std::min(message.find_first_not_of(" \t\r\n"), message.size());
    return {message.substr(0, indentSize), message.substr(indentSize)};
}

/// Prints a prefixed, coloured message line to stderr, keeping the message indentation in front of the prefix.
static void printPrefixedMessage(
    const std::string_view prefix,
    const fmt::terminal_color color,
    const std::string_view message
)
{
    const auto [indent, text] = splitLeadingWhitespace(message);
    printStyled(stderr, fmt::fg(color), "{}{}{}\n", indent, prefix, text);
}
}  // namespace console::detail

namespace console
{
bool isColorEnabled(std::FILE* stream)
{
    if (stream == stdout) {
        static const bool stdoutColorEnabled = detail::computeColorEnabled(stdout);
        return stdoutColorEnabled;
    }

    if (stream == stderr) {
        static const bool stderrColorEnabled = detail::computeColorEnabled(stderr);
        return stderrColorEnabled;
    }

    return detail::computeColorEnabled(stream);
}

void printStyledText(std::FILE* stream, const fmt::text_style style, const std::string_view text)
{
    const auto bodyLength = text.find_last_not_of("\r\n") + 1;
    const auto body = text.substr(0, bodyLength);
    const auto trailingNewlines = text.substr(body.size());

    if (body.empty()) {
        fmt::print(stream, "{}", trailingNewlines);
        return;
    }

    // Write the styled body and the newlines in a single call,
    // so output from other threads cannot land between them.
    fmt::print(stream, "{}{}", fmt::styled(body, style), trailingNewlines);
}

void printError(const std::string_view message)
{
    detail::printPrefixedMessage("Error: ", fmt::terminal_color::red, message);
}

void printError(const juce::String& message)
{
    printError(std::string_view {message.toRawUTF8(), message.getNumBytesAsUTF8()});
}

void printWarn(const std::string_view message)
{
    detail::printPrefixedMessage("Warning: ", fmt::terminal_color::yellow, message);
}

void printWarn(const juce::String& message)
{
    printWarn(std::string_view {message.toRawUTF8(), message.getNumBytesAsUTF8()});
}

void printLineToConsole(const juce::String& text)
{
#if JUCE_WINDOWS
    if (auto* const outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        outputHandle == nullptr || outputHandle == INVALID_HANDLE_VALUE)
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS) != 0) {
            FILE* stream = nullptr;
            freopen_s(&stream, "CONOUT$", "w", stdout);
        }
    }
#endif
    fmt::print("{}\n", text);
    std::fflush(stdout);
}

int displayWidth(const juce::String& text)
{
    std::size_t width = 0;
    for (auto character = text.getCharPointer(); !character.isEmpty();) {
        width += fmt::detail::display_width_of(static_cast<std::uint32_t>(character.getAndAdvance()));
    }

    return static_cast<int>(width);
}
}  // namespace console
