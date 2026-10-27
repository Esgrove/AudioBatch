/// Shared helpers for the AudioBatch tests.
/// Declares TemporaryDirectory, which owns a scratch folder that is deleted with it,
/// the functions that locate the committed audio fixtures in the root fixtures directory, and level helpers.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "normalization/AudioNormalization.h"
#include "serato/SeratoTagTypes.h"

#include <JuceHeader.h>

#include <optional>
#include <variant>

namespace audiobatch::tests
{
/// Peak level of the fixture tones, which have a peak amplitude of 0.5, in dBFS.
constexpr double fixturePeakDecibels = -6.0206;
/// Integrated loudness of a fixture tone, in LUFS.
/// A 1 kHz sine on both channels measures the same as its peak level, as in EBU Tech 3341.
/// Mono fixtures are measured as dual mono, so they read the same as the stereo ones.
constexpr double fixtureLoudness = fixturePeakDecibels;
/// Sample peak of the intersample fixture, a quarter sample rate sine whose samples fall between its peaks, in dBFS.
/// Its sample peak, true peak, and loudness all differ, so a measurement reported in the wrong field fails.
constexpr double intersampleFixturePeakDecibels {-9.03};
/// True peak of the intersample fixture, in dBTP.
constexpr double intersampleFixtureTruePeakDecibels {-5.92};
/// Integrated loudness of the intersample fixture, in LUFS, raised by the K-weighting at 11 kHz.
constexpr double intersampleFixtureLoudness {-2.67};
/// Position of the single click in the click fixtures, in samples.
constexpr juce::int64 clickFixturePosition {11'025};
/// Length of the click fixtures in samples, which is half a second at 44.1 kHz.
constexpr juce::int64 clickFixtureLength {22'050};
/// Decoded length of the MP3 tone without the Xing frame:
/// its 40 frames of 1152 samples without the 529 samples of decoder delay.
constexpr juce::int64 noXingFixtureLength {(40 * 1152) - 529};

/// Observes simulated driver ownership without touching hardware. Must outlive the installed device manager.
struct TestAudioDeviceState {
    int opens {0};
    int closes {0};
    int inputChannels {0};
    int outputChannels {0};
    juce::String openError;
    juce::AudioIODeviceCallback* callback {nullptr};
};

/// Captures log messages from any worker and restores the previous logger when the scope ends.
/// All workers using the logger must finish before this object is destroyed.
class ScopedLogCapture final : public juce::Logger
{
public:
    ScopedLogCapture();

    ~ScopedLogCapture() override;

    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;
    ScopedLogCapture(ScopedLogCapture&&) = delete;
    ScopedLogCapture& operator=(ScopedLogCapture&&) = delete;

    [[nodiscard]] juce::StringArray getMessages() const;

protected:
    void logMessage(const juce::String& message) override;

private:
    juce::Logger* previousLogger;
    juce::CriticalSection lock;
    juce::StringArray messages;
};

/// A uniquely named folder in the system temp directory that is deleted recursively on destruction.
class TemporaryDirectory
{
public:
    /// Creates the folder.
    TemporaryDirectory();

    /// Deletes the folder and everything in it.
    ~TemporaryDirectory();

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    /// Returns a file inside the folder, which does not need to exist yet.
    [[nodiscard]] juce::File getChildFile(const juce::String& name) const;

    /// Copies a fixture into the folder, optionally under a new name, and returns the copy.
    /// Tests that modify files work on copies so the committed fixtures never change.
    [[nodiscard]] juce::File copyFixture(const juce::String& fixtureName, const juce::String& copyName = {}) const;

private:
    juce::File directory;
};

/// Result of a null test that subtracts an expected file from an actual file, sample by sample in 16-bit steps.
/// Both files are read as integer sample values, so any difference of a single step counts.
struct NullTestResult {
    /// False when either file could not be read, which leaves every other field empty.
    bool readable {false};
    int expectedChannels {0};
    int actualChannels {0};
    juce::int64 expectedLength {0};
    juce::int64 actualLength {0};
    /// Number of samples over all channels that differ within the common length.
    juce::int64 differingSamples {0};
    /// Largest absolute difference within the common length, in 16-bit steps.
    int peakDifference {0};
    /// Offset of the actual first channel against the expected one, in samples, where the two correlate best.
    /// It is zero for aligned audio, and tells by how much a delayed or early output is shifted.
    /// Only computed when the files do not cancel.
    int bestLag {0};

    /// Returns true when both files have the same layout and length and every sample cancels out.
    [[nodiscard]] bool cancels() const noexcept
    {
        return readable && expectedChannels == actualChannels && expectedLength == actualLength
            && differingSamples == 0;
    }

    /// Describes the result for a failed expectation:
    /// the layouts and lengths, the differing sample count, the residual peak in dBFS, and the best lag.
    [[nodiscard]] juce::String describe() const;
};

/// Subtracts the expected file from the actual file sample by sample, and finds the lag at which they correlate best.
NullTestResult nullTestFiles(const juce::File& expected, const juce::File& actual);

/// Returns the committed fixture with the given file name from the root fixtures directory.
juce::File fixtureFile(const juce::String& name);

/// Returns the AudioBatch Test MS VST3 bundle that CMake builds from test-plugin/ next to the tests.
juce::File testPluginBundle();

/// Returns the sample peak of an analysis record in dBFS.
double peakDecibels(const AudioAnalysisRecord& record);
juce::int64 peakSamplePosition(const juce::File& file);
juce::String outcomeError(const OperationOutcome& outcome);
AudioAnalysisRecord outcomeRecord(const OperationOutcome& outcome);
juce::String outcomeNotice(const OperationOutcome& outcome);

bool deleteInsteadOfTrash(const juce::File& file);
AudioNormalizationOptions testNormalizationOptions();
serato::TrackInfo makeTrackInfo(double bpm, const juce::String& key);

/// Returns a copy of the value of a present Serato tag result as the given tag type,
/// or nothing when the result has no value of that type.
template<typename Value>
std::optional<Value> tagValue(const serato::TagReadResult& result)
{
    const auto* value = result.value.has_value() ? std::get_if<Value>(&*result.value) : nullptr;
    return value != nullptr ? std::optional<Value>(*value) : std::nullopt;
}

void installTestAudioDevice(juce::AudioDeviceManager& manager, TestAudioDeviceState& state);
void renderTestAudioDevice(const TestAudioDeviceState& state, int sampleCount = 512);
}  // namespace audiobatch::tests
