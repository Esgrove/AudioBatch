/// Source playback-clock formatting and status-bar composition without physical audio or a desktop peer.
/// The transport clock remains a separate label so foreground and optional preview progress stay readable.

#include "gui/components/StatusBar.h"
#include "TestSuites.h"

#include <JuceHeader.h>

#include <limits>
#include <memory>

namespace audiobatch::tests
{
/// Checks elapsed-time presentation independently of transport timing and device availability.
class PlaybackStatusTests final : public juce::UnitTest
{
public:
    /// Registers the status formatting suite without static initialization.
    PlaybackStatusTests() : juce::UnitTest("Playback status", "AudioBatch") {}

    /// Verifies clock boundaries, malformed timing, and independent status text ownership.
    void runTest() override
    {
        beginTest("Playback time uses whole seconds and switches both clocks to hours for long tracks");
        expectEquals(formatPlaybackTime(0.0, 245.0), juce::String("00:00 / 04:05"));
        expectEquals(formatPlaybackTime(61.9, 245.9), juce::String("01:01 / 04:05"));
        expectEquals(formatPlaybackTime(3601.0, 7200.0), juce::String("1:00:01 / 2:00:00"));
        expectEquals(formatPlaybackTime(312.0, 4860.0), juce::String("0:05:12 / 1:21:00"));
        expectEquals(formatPlaybackTime(0.0, 3599.999), juce::String("00:00 / 59:59"));
        expectEquals(formatPlaybackTime(0.0, 3600.0), juce::String("0:00:00 / 1:00:00"));
        expectEquals(formatPlaybackTime(3599.0, 3600.0), juce::String("0:59:59 / 1:00:00"));
        expectEquals(formatPlaybackTime(4860.0, 4860.0), juce::String("1:21:00 / 1:21:00"));
        expectEquals(formatPlaybackTime(100.0, 2.0), juce::String("00:02 / 00:02"));
        expectEquals(formatPlaybackTime(-2.0, 65.0), juce::String("00:00 / 01:05"));
        expectEquals(formatPlaybackTime(std::numeric_limits<double>::quiet_NaN(), 65.0), juce::String("00:00 / 01:05"));
        expectEquals(formatPlaybackTime(1.0, std::numeric_limits<double>::infinity()), juce::String("00:00 / 00:00"));

        beginTest("Playback clock remains visible beside background status and clears independently");
        StatusBar bar;
        bar.setSize(800, 24);
        bar.setStatusText("Background previews: 2/5 files");
        bar.setPlaybackTime(61.0, 245.0, true);
        bool clockFound = false;
        bool progressFound = false;
        for (const auto* child : bar.getChildren()) {
            if (const auto* label = dynamic_cast<const juce::Label*>(child)) {
                clockFound = clockFound || (label->isVisible() && label->getText() == "01:01 / 04:05");
                progressFound = progressFound || label->getText() == "Background previews: 2/5 files";
            }
        }
        expect(clockFound && progressFound);
        bar.setPlaybackTime(0.0, 0.0, false);
        for (const auto* child : bar.getChildren()) {
            if (const auto* label = dynamic_cast<const juce::Label*>(child);
                label != nullptr && label->getName() == "Playback time")
            {
                expect(!label->isVisible());
            }
        }
    }
};

/// Creates the device-free playback status suite.
std::unique_ptr<juce::UnitTest> createPlaybackStatusTests()
{
    return std::make_unique<PlaybackStatusTests>();
}
}  // namespace audiobatch::tests
