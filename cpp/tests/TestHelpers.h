/// Shared helpers for the AudioBatch tests.
/// Declares TemporaryDirectory, which owns a scratch folder that is deleted with it,
/// the functions that locate the committed audio fixtures in the root fixtures directory, and level helpers.
/// Also declares the counting cancellation check,
/// and the helper modes that run the test executable again as a child process.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "normalization/AudioNormalization.h"
#include "serato/SeratoTagTypes.h"
#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <atomic>
#include <memory>
#include <optional>
#include <variant>

namespace audiobatch::tests
{
/// Peak level of the fixture tones, which have a peak amplitude of 0.5, in dBFS.
constexpr double FIXTURE_PEAK_DECIBELS {-6.0206};
/// Integrated loudness of a fixture tone, in LUFS.
/// A 1 kHz sine on both channels measures the same as its peak level, as in EBU Tech 3341.
/// Mono fixtures are measured as dual mono, so they read the same as the stereo ones.
constexpr double FIXTURE_LOUDNESS {FIXTURE_PEAK_DECIBELS};
/// Sample peak of the intersample fixture, a quarter sample rate sine whose samples fall between its peaks, in dBFS.
/// Its sample peak, true peak, and loudness all differ, so a measurement reported in the wrong field fails.
constexpr double INTERSAMPLE_FIXTURE_PEAK_DECIBELS {-9.03};
/// True peak of the intersample fixture, in dBTP.
constexpr double INTERSAMPLE_FIXTURE_TRUE_PEAK_DECIBELS {-5.92};
/// Integrated loudness of the intersample fixture, in LUFS, raised by the K-weighting at 11 kHz.
constexpr double INTERSAMPLE_FIXTURE_LOUDNESS {-2.67};
/// Position of the single click in the click fixtures, in samples.
constexpr juce::int64 CLICK_FIXTURE_POSITION {11'025};
/// Length of the click fixtures in samples, which is half a second at 44.1 kHz.
constexpr juce::int64 CLICK_FIXTURE_LENGTH {22'050};
/// Decoded length of the MP3 tone without the Xing frame:
/// its 40 frames of 1152 samples without the 529 samples of decoder delay.
constexpr juce::int64 NO_XING_FIXTURE_LENGTH {(40 * 1152) - 529};

/// First argument that makes the test executable run the message loop check of runHelperProcess and exit.
constexpr auto MESSAGE_LOOP_HELPER_ARGUMENT {"--message-loop-helper"};
/// First argument that makes the test executable hold a plugin scan lock for runHelperProcess.
/// It is followed by the lock name and the file whose creation releases the lock.
constexpr auto SCAN_LOCK_HELPER_ARGUMENT {"--scan-lock-helper"};
/// Line a scan lock helper prints once it holds the lock.
constexpr auto SCAN_LOCK_HELD_LINE {"held"};

/// Sample format of one file in a null test, as its reader reports it.
struct NullTestFormat {
    /// Returns true when both formats have the same sample rate, bit depth, and sample type.
    [[nodiscard]] bool matches(const NullTestFormat& other) const noexcept
    {
        return juce::approximatelyEqual(sampleRate, other.sampleRate) && bitsPerSample == other.bitsPerSample
            && floatingPoint == other.floatingPoint;
    }

    /// Sample rate in Hz. Keep the reader's positional initialization order.
    double sampleRate {0.0};
    /// Bits per sample of the stored data.
    int bitsPerSample {0};
    /// True for a file that stores floating point samples instead of integers.
    bool floatingPoint {false};
};

/// Result of a null test that subtracts an expected file from an actual file, sample by sample in 16-bit steps.
/// Both files are read as integer sample values, so any difference of a single step counts.
/// The files must also have the same sample rate, bit depth, and integer or floating point samples to cancel.
struct NullTestResult {
    [[nodiscard]] juce::String describe() const;

    /// Returns true when both files have the same format, layout, and length, and every sample cancels out.
    [[nodiscard]] bool cancels() const noexcept
    {
        return readable && expectedFormat.matches(actualFormat) && expectedChannels == actualChannels
            && expectedLength == actualLength && differingSamples == 0;
    }

    /// Sample format of the actual file.
    NullTestFormat actualFormat;
    /// Sample format of the expected file.
    NullTestFormat expectedFormat;

    /// False when either file could not be read, which leaves every other field empty.
    bool readable {false};

    /// Channel count of the actual file.
    int actualChannels {0};
    /// Offset of the actual first channel against the expected one, in samples, where the two correlate best.
    /// It is zero for aligned audio, and tells by how much a delayed or early output is shifted.
    /// Only computed when the files do not cancel.
    int bestLag {0};
    /// Channel count of the expected file.
    int expectedChannels {0};
    /// Largest absolute difference within the common length, in 16-bit steps.
    int peakDifference {0};

    /// Length of the actual file in samples per channel.
    juce::int64 actualLength {0};
    /// Number of samples over all channels that differ within the common length.
    juce::int64 differingSamples {0};
    /// Length of the expected file in samples per channel.
    juce::int64 expectedLength {0};
};

/// Observes simulated driver ownership without touching hardware. Must outlive the installed device manager.
struct TestAudioDeviceState {
    /// Non-owning callback installed only while the simulated driver is playing.
    juce::AudioIODeviceCallback* callback {nullptr};

    /// Error that every device open returns, empty for a successful open.
    juce::String openError;
    /// Name of the simulated device type.
    juce::String typeName {"Test audio"};

    /// Names of the devices created, in creation order.
    juce::StringArray attemptedDevices;
    /// Output devices the simulated type offers.
    juce::StringArray outputDeviceNames {"Test output"};
    /// Devices that fail to open as if disconnected.
    juce::StringArray unavailableDeviceNames;

    /// Sample rate the simulated device offers and reports.
    double sampleRate {48'000.0};
    /// Callback size the simulated device offers and reports.
    int blockSize {512};
    /// Number of balanced closes of a successfully opened device.
    int closes {0};
    /// Number of input channels requested by the latest open.
    int inputChannels {0};
    /// Number of successful device opens.
    int opens {0};
    /// Number of output channels requested by the latest open.
    int outputChannels {0};
};

/// Captures log messages from any worker and restores the previous logger when the scope ends.
/// All workers using the logger must finish before this object is destroyed.
class ScopedLogCapture final : public juce::Logger
{
public:
    ScopedLogCapture();

    ~ScopedLogCapture() override;

    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture(ScopedLogCapture&&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(ScopedLogCapture&&) = delete;

    [[nodiscard]] juce::StringArray getMessages() const;

protected:
    void logMessage(const juce::String& message) override;

private:
    juce::CriticalSection lock;
    juce::Logger* previousLogger;
    juce::StringArray messages;
};

/// A uniquely named folder in the system temp directory that is deleted recursively on destruction.
class TemporaryDirectory
{
public:
    TemporaryDirectory();

    ~TemporaryDirectory();

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    [[nodiscard]] juce::File copyFixture(const juce::String& fixtureName, const juce::String& copyName = {}) const;
    [[nodiscard]] juce::File getChildFile(const juce::String& name) const;

private:
    juce::File directory;
};

AudioAnalysisRecord outcomeRecord(const OperationOutcome& outcome);

AudioNormalizationOptions testNormalizationOptions();

NullTestResult nullTestFiles(const juce::File& expected, const juce::File& actual);

serato::TrackInfo makeTrackInfo(double bpm, const juce::String& key);

utils::CancellationCheck cancelAfterChecks(int allowedChecks, std::shared_ptr<std::atomic<int>> calls);

juce::File fixtureFile(const juce::String& name);
juce::File testPluginBundle();

juce::MemoryBlock validWaveform();

juce::String outcomeError(const OperationOutcome& outcome);
juce::String outcomeNotice(const OperationOutcome& outcome);
juce::String readHelperLine(juce::ChildProcess& process);

std::optional<int> runHelperProcess(const juce::StringArray& arguments);

/// Returns a copy of the value of a present Serato tag result as the given tag type,
/// or nothing when the result has no value of that type.
template<typename Value>
std::optional<Value> tagValue(const serato::TagReadResult& result)
{
    const auto* value = result.value.has_value() ? std::get_if<Value>(&*result.value) : nullptr;
    return value != nullptr ? std::optional<Value>(*value) : std::nullopt;
}

bool writeFloatWav(const juce::File& file, const juce::AudioBuffer<float>& samples);
bool deleteInsteadOfTrash(const juce::File& file);
bool executeSql(juce::UnitTest& test, const juce::File& databaseFile, const char* sql);
bool startHelperProcess(juce::ChildProcess& process, const juce::StringArray& helperArguments);

double peakDecibels(const AudioAnalysisRecord& record);

juce::int64 peakSamplePosition(const juce::File& file);

void expectAnalysisMeasurementsEqual(
    juce::UnitTest& test,
    const AudioAnalysisRecord& actual,
    const AudioAnalysisRecord& expected
);
void expectSqlScalar(juce::UnitTest& test, const juce::File& databaseFile, const char* sql, int expected);
void installTestAudioDevice(juce::AudioDeviceManager& manager, TestAudioDeviceState& state);
void renderTestAudioDevice(const TestAudioDeviceState& state, int sampleCount = 512);
}  // namespace audiobatch::tests
