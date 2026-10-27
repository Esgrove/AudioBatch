/// A progress bar on one line of stderr for the CLI.
/// Declares console::ProgressLine, which the CLI analysis and normalization advance from any thread,
/// and which draws only when stderr is a colour terminal, so piped and redirected output stays clean.

#pragma once

#include <JuceHeader.h>

#include <mutex>

namespace audiobatch::console
{
/// Draws "[elapsed] [bar] done/total name" on one stderr line, rewritten in place,
/// the same layout the Rust CLI draws.
/// Thread-safe, so worker callbacks can advance it, and it erases its line when it finishes or is destroyed.
class ProgressLine
{
public:
    ProgressLine();

    ~ProgressLine();

    ProgressLine(const ProgressLine&) = delete;
    ProgressLine& operator=(const ProgressLine&) = delete;
    ProgressLine(ProgressLine&&) = delete;
    ProgressLine& operator=(ProgressLine&&) = delete;

    void finish();
    void increment();
    void setMessage(const juce::String& message);
    void setTotal(int total);

private:
    void drawUnlocked(bool force);

    std::mutex mutex;
    /// The name shown after the counts, such as the file being worked on.
    juce::String currentMessage;
    /// When the progress line was created, for the elapsed time.
    double startedAtMilliseconds {0.0};
    /// When the line was last drawn, to limit redraws to a few per second.
    double lastDrawnAtMilliseconds {0.0};
    int done {0};
    int total {0};
    /// True when stderr is a colour terminal, so the line is drawn at all.
    bool enabled {false};
    /// True while a drawn line is on screen and needs erasing.
    bool drawn {false};
};
}  // namespace audiobatch::console
