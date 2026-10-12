/// Styled console output helpers for the CLI and the release-build log echo.
/// The console namespace wraps fmt::print with colour and emphasis styles from fmt/color.h,
/// plus Error and Warning message helpers that write to stderr.
/// JUCE strings and files format directly through the formatters in StringFormat.h.
/// Styles are only applied when the target stream is a terminal and NO_COLOR is not set,
/// so redirected output stays free of escape codes.

#pragma once

#include "StringFormat.h"
#include <fmt/color.h>
#include <string_view>

#include <JuceHeader.h>

#include <cstdio>
#include <string>
#include <utility>

namespace console
{
/// Returns how many terminal columns the text occupies, for example two for each CJK character.
/// Measures the width through fmt's own padding, so computed column widths match how fmt pads the cells,
/// without relying on fmt's internal width function.
[[nodiscard]] int displayWidth(const juce::String& text);

/// Prints one line of text to stdout and flushes it.
/// On Windows a GUI subsystem executable has no console of its own,
/// so when stdout is not already redirected to a pipe or file,
/// this attaches to the console of the parent process first, so the text shows up in cmd or PowerShell.
void printLineToConsole(const juce::String& text);

/// Returns true when styled output should be written to the given stream.
/// That is the case for an interactive terminal when the NO_COLOR environment variable is not set.
/// On Windows this also enables virtual terminal processing for the console the first time,
/// and treats a pipe as a terminal when TERM is set, which is how Git Bash (mintty) connects programs.
/// The result is cached for stdout and stderr.
[[nodiscard]] bool isColorEnabled(std::FILE* stream);

/// Prints already formatted text to the given stream with a text style.
/// Trailing newlines are written after the style reset,
/// so the reset escape code does not leak onto the next line.
void printStyledText(std::FILE* stream, fmt::text_style style, std::string_view text);

/// Formats and prints to the given stream with a text style.
/// The style is dropped when the stream does not support colour.
template<typename... Args>
void printStyled(std::FILE* stream, const fmt::text_style style, fmt::format_string<Args...> format, Args&&... args)
{
    if (isColorEnabled(stream)) {
        printStyledText(stream, style, fmt::format(format, std::forward<Args>(args)...));
    } else {
        fmt::print(stream, format, std::forward<Args>(args)...);
    }
}

/// Prints to stdout in the given colour.
template<typename... Args>
void printColor(const fmt::terminal_color color, fmt::format_string<Args...> format, Args&&... args)
{
    printStyled(stdout, fmt::fg(color), format, std::forward<Args>(args)...);
}

/// Prints bold text to stdout.
template<typename... Args>
void printBold(fmt::format_string<Args...> format, Args&&... args)
{
    printStyled(stdout, fmt::emphasis::bold, format, std::forward<Args>(args)...);
}

/// Prints bold text to stdout in the given colour.
template<typename... Args>
void printColorBold(const fmt::terminal_color color, fmt::format_string<Args...> format, Args&&... args)
{
    printStyled(stdout, fmt::emphasis::bold | fmt::fg(color), format, std::forward<Args>(args)...);
}

/// Prints green text to stdout.
template<typename... Args>
void printGreen(fmt::format_string<Args...> format, Args&&... args)
{
    printColor(fmt::terminal_color::green, format, std::forward<Args>(args)...);
}

/// Prints bold green text to stdout.
template<typename... Args>
void printGreenBold(fmt::format_string<Args...> format, Args&&... args)
{
    printColorBold(fmt::terminal_color::green, format, std::forward<Args>(args)...);
}

/// Prints yellow text to stdout.
template<typename... Args>
void printYellow(fmt::format_string<Args...> format, Args&&... args)
{
    printColor(fmt::terminal_color::yellow, format, std::forward<Args>(args)...);
}

/// Prints bold yellow text to stdout.
template<typename... Args>
void printYellowBold(fmt::format_string<Args...> format, Args&&... args)
{
    printColorBold(fmt::terminal_color::yellow, format, std::forward<Args>(args)...);
}

/// Prints red text to stdout.
template<typename... Args>
void printRed(fmt::format_string<Args...> format, Args&&... args)
{
    printColor(fmt::terminal_color::red, format, std::forward<Args>(args)...);
}

/// Prints bold red text to stdout.
template<typename... Args>
void printRedBold(fmt::format_string<Args...> format, Args&&... args)
{
    printColorBold(fmt::terminal_color::red, format, std::forward<Args>(args)...);
}

/// Prints bold magenta text to stdout.
template<typename... Args>
void printMagentaBold(fmt::format_string<Args...> format, Args&&... args)
{
    printColorBold(fmt::terminal_color::magenta, format, std::forward<Args>(args)...);
}

/// Formats a value with a colour for building composite lines,
/// for example a coloured column inside an otherwise plain row printed to stdout.
/// Returns the plain text when stdout does not support colour.
template<typename T>
[[nodiscard]] std::string colored(const T& value, const fmt::terminal_color color)
{
    if (isColorEnabled(stdout)) {
        return fmt::format(fmt::fg(color), "{}", value);
    }

    return fmt::format("{}", value);
}

/// Prints "Error: <message>" in red to stderr, followed by a newline.
/// Leading whitespace in the message is kept in front of the prefix,
/// so indented messages stay indented.
void printError(std::string_view message);

/// Prints "Error: <message>" in red to stderr, followed by a newline.
void printError(const juce::String& message);

/// Prints "Error: <message>" in red to stderr, followed by a newline.
/// Resolves string literals, which would otherwise be ambiguous between the other overloads.
inline void printError(const char* message)
{
    printError(std::string_view {message});
}

/// Prints "Warning: <message>" in yellow to stderr, followed by a newline.
/// Leading whitespace in the message is kept in front of the prefix.
void printWarn(std::string_view message);

/// Prints "Warning: <message>" in yellow to stderr, followed by a newline.
void printWarn(const juce::String& message);

/// Prints "Warning: <message>" in yellow to stderr, followed by a newline.
/// Resolves string literals, which would otherwise be ambiguous between the other overloads.
inline void printWarn(const char* message)
{
    printWarn(std::string_view {message});
}
}  // namespace console
