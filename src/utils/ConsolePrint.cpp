/// Implementation of the console output helpers.
/// Covers the terminal and NO_COLOR detection that decides whether styles are applied,
/// including enabling virtual terminal processing on Windows consoles,
/// and the Error and Warning message helpers that keep leading indentation.

#include "utils/ConsolePrint.h"

#include "utils/StringFormat.h"

#include <algorithm>

#if JUCE_WINDOWS
#include <juce_core/native/juce_BasicNativeHeaders.h>
#else
#include <unistd.h>
#endif

namespace audiobatch
{
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
/// Returns true when styled output should be written to the given stream.
/// That is the case for an interactive terminal when the NO_COLOR environment variable is not set.
/// On Windows this also enables virtual terminal processing for the console the first time,
/// and treats a pipe as a terminal when TERM is set, which is how Git Bash (mintty) connects programs.
/// The result is cached for stdout and stderr.
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

/// Prints already formatted text to the given stream with a text style.
/// Trailing newlines are written after the style reset, so the reset escape code does not leak onto the next line.
void printStyledText(std::FILE* stream, const fmt::text_style style, const std::string_view text)
{
    const auto bodyLength = text.find_last_not_of("\r\n") + 1;
    const auto body = text.substr(0, bodyLength);
    const auto trailingNewlines = text.substr(body.size());

    if (body.empty()) {
        fmt::print(stream, "{}", trailingNewlines);
        return;
    }

    // Write the styled body and the newlines in a single call, so output from other threads cannot land between them.
    fmt::print(stream, "{}{}", fmt::styled(body, style), trailingNewlines);
}

/// Prints "Error: <message>" in red to stderr, followed by a newline.
/// Leading whitespace in the message is kept in front of the prefix, so indented messages stay indented.
void printError(const std::string_view message)
{
    detail::printPrefixedMessage("Error: ", fmt::terminal_color::red, message);
}

/// Prints "Error: <message>" in red to stderr, followed by a newline.
void printError(const juce::String& message)
{
    printError(std::string_view {message.toRawUTF8(), message.getNumBytesAsUTF8()});
}

/// Prints "Warning: <message>" in yellow to stderr, followed by a newline.
/// Leading whitespace in the message is kept in front of the prefix.
void printWarn(const std::string_view message)
{
    detail::printPrefixedMessage("Warning: ", fmt::terminal_color::yellow, message);
}

/// Prints "Warning: <message>" in yellow to stderr, followed by a newline.
void printWarn(const juce::String& message)
{
    printWarn(std::string_view {message.toRawUTF8(), message.getNumBytesAsUTF8()});
}

/// Prints one line of text to stdout and flushes it.
/// On Windows a GUI subsystem executable has no console of its own,
/// so when stdout is not already redirected to a pipe or file,
/// this attaches to the console of the parent process first, so the text shows up in cmd or PowerShell.
void printLineToConsole(const juce::String& text)
{
#if JUCE_WINDOWS
    if (const auto* outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
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

/// Returns how many terminal columns the text occupies, for example two for each CJK character.
/// Measures the width through fmt's own padding, so computed column widths match how fmt pads the cells,
/// without relying on fmt's internal width function.
int displayWidth(const juce::String& text)
{
    const std::string_view utf8 {text.toRawUTF8(), text.getNumBytesAsUTF8()};

    // fmt has no public width query, so the width is measured by the padding fmt adds to reach a known width.
    // No character is wider than two columns, so the padded width is always larger than the text.
    const auto paddedWidth = utf8.size() * 2 + 1;
    const auto padded = fmt::format("{:<{}}", utf8, paddedWidth);
    return static_cast<int>(paddedWidth - (padded.size() - utf8.size()));
}
}  // namespace console
}  // namespace audiobatch
