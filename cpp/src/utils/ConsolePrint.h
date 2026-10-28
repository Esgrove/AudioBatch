/// Styled console output helpers for the CLI and the release-build log echo.
/// The console namespace wraps fmt::print with colour and emphasis styles from fmt/color.h,
/// plus Error and Warning message helpers that write to stderr.
/// JUCE strings and files format directly through the formatters in StringFormat.h.
/// Styles are only applied when the target stream is a terminal and NO_COLOR is not set,
/// so redirected output stays free of escape codes.

#pragma once

#include "utils/StringFormat.h"
#include <fmt/color.h>
#include <string_view>

#include <JuceHeader.h>

#include <cstdio>
#include <string>
#include <utility>

namespace audiobatch::console
{
[[nodiscard]] int displayWidth(const juce::String& text);

[[nodiscard]] bool isColorEnabled(std::FILE* stream);

void printLineToConsole(const juce::String& text);

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

void printError(std::string_view message);

void printError(const juce::String& message);

/// Prints "Error: <message>" in red to stderr, followed by a newline.
/// Resolves string literals, which would otherwise be ambiguous between the other overloads.
inline void printError(const char* message)
{
    printError(std::string_view {message});
}

void printWarn(std::string_view message);

void printWarn(const juce::String& message);

/// Prints "Warning: <message>" in yellow to stderr, followed by a newline.
/// Resolves string literals, which would otherwise be ambiguous between the other overloads.
inline void printWarn(const char* message)
{
    printWarn(std::string_view {message});
}

/// Formats a value with a colour for building composite lines,
/// for example a coloured column inside an otherwise plain row printed to stdout.
/// Returns the plain text when stdout does not support colour.
template<utils::Formattable T>
[[nodiscard]] std::string colored(const T& value, const fmt::terminal_color color)
{
    if (isColorEnabled(stdout)) {
        return fmt::format(fmt::fg(color), "{}", value);
    }

    return fmt::format("{}", value);
}
}  // namespace audiobatch::console
