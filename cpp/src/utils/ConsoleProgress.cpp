/// Implementation of console::ProgressLine:
/// the throttled redraw of the elapsed time, the bar, the counts, and the message on one stderr line.

#include "utils/ConsoleProgress.h"

#include "utils/ConsolePrint.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <cmath>
#include <string>

namespace audiobatch::console
{
namespace
{
/// Width of the bar in characters.
constexpr int barWidth {40};
/// Shortest time between two redraws, so a fast run does not flood the terminal.
constexpr double redrawIntervalMilliseconds {100.0};
constexpr int secondsPerMinute {60};
constexpr int secondsPerHour {3600};
}  // namespace

/// Creates the progress line, which draws only when stderr is a colour terminal.
ProgressLine::ProgressLine() :
    startedAtMilliseconds(juce::Time::getMillisecondCounterHiRes()),
    enabled(isColorEnabled(stderr))
{}

/// Erases the line, so later output starts on a clean line.
ProgressLine::~ProgressLine()
{
    finish();
}

/// Erases the line. Later calls draw it again.
void ProgressLine::finish()
{
    const std::scoped_lock lock(mutex);
    if (drawn) {
        fmt::print(stderr, "\r\033[K");
        std::fflush(stderr);
        drawn = false;
    }
}

/// Counts one more finished item and redraws the line.
void ProgressLine::increment()
{
    const std::scoped_lock lock(mutex);
    ++done;
    drawUnlocked(done == total);
}

/// Shows the message after the counts, such as the name of the file being worked on.
void ProgressLine::setMessage(const juce::String& message)
{
    const std::scoped_lock lock(mutex);
    currentMessage = message;
    drawUnlocked(false);
}

/// Sets the number of items the bar counts to, and redraws the line.
void ProgressLine::setTotal(const int newTotal)
{
    const std::scoped_lock lock(mutex);
    total = juce::jmax(0, newTotal);
    drawUnlocked(true);
}

/// Redraws the line when it is enabled and the last redraw was long enough ago, or always when forced.
/// Nothing is drawn while there is nothing to count, such as a run that only reads cached results.
/// The caller holds the mutex.
void ProgressLine::drawUnlocked(const bool force)
{
    if (!enabled || total == 0) {
        return;
    }

    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (!force && now - lastDrawnAtMilliseconds < redrawIntervalMilliseconds) {
        return;
    }
    lastDrawnAtMilliseconds = now;

    const auto elapsedSeconds = static_cast<int>((now - startedAtMilliseconds) / utils::millisecondsPerSecond);
    const auto filled = total > 0 ? juce::jlimit(0, barWidth, done * barWidth / total) : 0;
    const auto bar = std::string(static_cast<std::size_t>(filled), '#')
        + std::string(static_cast<std::size_t>(barWidth - filled), '-');
    fmt::print(
        stderr,
        "\r\033[K[{:02}:{:02}:{:02}] {} {}/{} {}",
        elapsedSeconds / secondsPerHour,
        (elapsedSeconds / secondsPerMinute) % secondsPerMinute,
        elapsedSeconds % secondsPerMinute,
        bar,
        done,
        total,
        currentMessage
    );
    std::fflush(stderr);
    drawn = true;
}
}  // namespace audiobatch::console
