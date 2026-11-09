/// Implementation of the shared test helpers:
/// the self-deleting temporary directory, the lookup of the committed audio fixtures and the test plugin,
/// the null test that compares a processed file against its expected output,
/// and the helper modes that run the test executable again as a child process.

#include "TestHelpers.h"

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/SpectrogramData.h"
#include "normalization/AudioNormalization.h"
#include "plugins/PluginScanner.h"
#include "serato/SeratoTagTypes.h"
#include "utils/AudioFormatUtils.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"
#include <fmt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Samples of a file as 16-bit values, one vector per channel.
using IntegerChannels = std::vector<std::vector<int>>;

/// The samples of a file read for a null test, with the format its reader reports.
struct NullTestSamples {
    NullTestFormat format;
    IntegerChannels channels;
};

/// Simulates a driver that owns the device from open until close and renders only when a test requests it.
class TestAudioDevice final : public juce::AudioIODevice
{
public:
    /// Retains the test-owned counters and callback state.
    TestAudioDevice(TestAudioDeviceState& deviceState, const juce::String& deviceName) :
        juce::AudioIODevice(deviceName, deviceState.typeName),
        state(deviceState)
    {
        state.attemptedDevices.add(deviceName);
    }

    /// Mirrors native drivers that release their resources on destruction.
    ~TestAudioDevice() override
    {
        close();
    }

    TestAudioDevice(const TestAudioDevice&) = delete;
    TestAudioDevice(TestAudioDevice&&) = delete;
    TestAudioDevice& operator=(const TestAudioDevice&) = delete;
    TestAudioDevice& operator=(TestAudioDevice&&) = delete;

    /// Keeps preparation independent of physical device capabilities.
    juce::Array<double> getAvailableSampleRates() override
    {
        return {48'000.0};
    }

    /// Uses a small fixed render quantum.
    juce::Array<int> getAvailableBufferSizes() override
    {
        return {512};
    }

    /// Returns the recording channels actually requested by the application.
    [[nodiscard]] juce::BigInteger getActiveInputChannels() const override
    {
        return inputs;
    }

    /// Returns the channels actually requested by the application.
    [[nodiscard]] juce::BigInteger getActiveOutputChannels() const override
    {
        return outputs;
    }

    /// Exposes explicit failures injected by the test.
    juce::String getLastError() override
    {
        return state.unavailableDeviceNames.contains(getName()) ? juce::String("No device is connected to the PC.")
                                                                : state.openError;
    }

    /// Captures the requested channels or returns the injected device-open error.
    juce::String open(
        const juce::BigInteger& inputChannels,
        const juce::BigInteger& outputChannels,
        double /*sampleRate*/,
        int /*bufferSizeSamples*/
    ) override
    {
        if (const auto error = getLastError(); error.isNotEmpty()) {
            return error;
        }
        inputs = inputChannels;
        outputs = outputChannels;
        state.inputChannels = inputs.countNumberOfSetBits();
        state.outputChannels = outputs.countNumberOfSetBits();
        opened = true;
        ++state.opens;
        return {};
    }

    /// Exposes inputs so tests can detect accidentally requesting recording channels.
    juce::StringArray getInputChannelNames() override
    {
        return {"Left", "Right"};
    }

    /// Exposes a stereo playback interface.
    juce::StringArray getOutputChannelNames() override
    {
        return {"Left", "Right"};
    }

    /// Reports simulated ownership of the interface.
    bool isOpen() override
    {
        return opened;
    }
    /// Reports whether a render callback is registered.
    bool isPlaying() override
    {
        return state.callback != nullptr;
    }

    /// Reports the prepared playback rate.
    double getCurrentSampleRate() override
    {
        return 48'000.0;
    }

    /// Reports the simulated interface resolution.
    int getCurrentBitDepth() override
    {
        return 24;
    }

    /// Reports the prepared callback size.
    int getCurrentBufferSizeSamples() override
    {
        return 512;
    }

    /// Matches the simulated callback size.
    int getDefaultBufferSize() override
    {
        return 512;
    }

    /// Adds no hardware input delay.
    int getInputLatencyInSamples() override
    {
        return 0;
    }

    /// Adds no hardware output delay.
    int getOutputLatencyInSamples() override
    {
        return 0;
    }

    /// Balances each successful device open exactly once.
    void close() override
    {
        stop();
        if (opened) {
            opened = false;
            ++state.closes;
        }
    }

    /// Sends JUCE's normal synchronous device-start notification.
    void start(juce::AudioIODeviceCallback* callback) override
    {
        state.callback = callback;
        if (callback != nullptr) {
            callback->audioDeviceAboutToStart(this);
        }
    }

    /// Detaches the callback before reporting that the driver stopped.
    void stop() override
    {
        if (auto* callback = std::exchange(state.callback, nullptr)) {
            callback->audioDeviceStopped();
        }
    }

private:
    TestAudioDeviceState& state;
    juce::BigInteger inputs;
    juce::BigInteger outputs;
    bool opened {false};
};

/// Supplies only simulated interfaces to a test's real JUCE device manager.
class TestAudioDeviceType final : public juce::AudioIODeviceType
{
public:
    /// Retains test state for every reopened driver instance.
    explicit TestAudioDeviceType(TestAudioDeviceState& deviceState) :
        juce::AudioIODeviceType(deviceState.typeName),
        state(deviceState)
    {}

    ~TestAudioDeviceType() override = default;

    TestAudioDeviceType(const TestAudioDeviceType&) = delete;
    TestAudioDeviceType(TestAudioDeviceType&&) = delete;
    TestAudioDeviceType& operator=(const TestAudioDeviceType&) = delete;
    TestAudioDeviceType& operator=(TestAudioDeviceType&&) = delete;

    /// Creates a fresh driver whose lifetime is controlled by the real device manager.
    juce::AudioIODevice* createDevice(
        const juce::String& outputDeviceName,
        const juce::String& /*inputDeviceName*/
    ) override
    {
        return new TestAudioDevice(state, outputDeviceName);
    }

    /// Offers the same interface for input and output.
    juce::StringArray getDeviceNames(bool /*wantInputNames*/) const override
    {
        return state.outputDeviceNames;
    }
    /// Allows output-only operation without an input device name.
    bool hasSeparateInputsAndOutputs() const override
    {
        return true;
    }

    /// Always selects the only interface.
    int getDefaultDeviceIndex(bool /*forInput*/) const override
    {
        return 0;
    }

    /// All created devices belong to the single advertised interface.
    int getIndexOfDevice(juce::AudioIODevice* device, bool /*asInput*/) const override
    {
        return device != nullptr ? state.outputDeviceNames.indexOf(device->getName()) : -1;
    }

    /// Needs no native device enumeration.
    void scanForDevices() override {}

private:
    TestAudioDeviceState& state;
};

/// Largest lag in either direction that the null test checks for the best correlation, in samples.
constexpr int maximumNullTestLag {256};

/// Scale from a float sample to a 16-bit sample value.
constexpr float pcm16Scale {32'768.0f};

/// Reads a file as 16-bit sample values, or returns nothing when it cannot be read.
/// The float samples of a 16-bit file are exact multiples of one step, so rounding loses nothing.
std::optional<NullTestSamples> readIntegerSamples(const juce::File& file)
{
    const std::unique_ptr<juce::AudioFormatReader> reader(utils::threadLocalFormatManager().createReaderFor(file));
    if (reader == nullptr) {
        return std::nullopt;
    }

    const auto length = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), length);
    if (!reader->read(&buffer, 0, length, 0, true, true)) {
        return std::nullopt;
    }

    NullTestSamples samples {
        .format = {
            .sampleRate = reader->sampleRate,
            .bitsPerSample = static_cast<int>(reader->bitsPerSample),
            .floatingPoint = reader->usesFloatingPointData,
        },
        .channels = IntegerChannels(static_cast<std::size_t>(buffer.getNumChannels())),
    };
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        auto& values = samples.channels[static_cast<std::size_t>(channel)];
        values.reserve(static_cast<std::size_t>(length));
        for (int sample = 0; sample < length; ++sample) {
            values.push_back(static_cast<int>(std::lround(buffer.getSample(channel, sample) * pcm16Scale)));
        }
    }
    return samples;
}

/// Longest time a helper process waits for its parent before giving up, in milliseconds.
constexpr juce::uint32 helperTimeoutMilliseconds {30'000};

/// Posts a message that stops the message loop, runs the loop, and returns 0 once the message was dispatched.
/// Stopping the loop ends it for the rest of the process, so this only runs in a helper process.
/// The CLI plugin scan worker relies on this loop to receive its scan requests.
int runMessageLoopHelper()
{
    // Shared, so a callback that is never dispatched does not refer to this stack frame.
    const auto dispatched = std::make_shared<std::atomic<bool>>(false);
    juce::MessageManager::callAsync([dispatched] {
        *dispatched = true;
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });

    juce::MessageManager::getInstance()->runDispatchLoop();
    return dispatched->load() ? 0 : 1;
}

/// Takes the plugin scan lock of the given name, prints scanLockHeldLine, and holds the lock
/// until the release file exists, so the parent process can check that it cannot take the same lock meanwhile.
/// Returns 1 without printing the line when the lock is already taken.
int runScanLockHelper(const juce::String& lockName, const juce::File& releaseFile)
{
    PluginScanLock lock(lockName);
    if (!lock.tryAcquire()) {
        return 1;
    }

    fmt::print("{}\n", scanLockHeldLine);
    std::fflush(stdout);

    const auto deadline = juce::Time::getMillisecondCounter() + helperTimeoutMilliseconds;
    while (!releaseFile.exists() && juce::Time::getMillisecondCounter() < deadline) {
        juce::Thread::sleep(5);
    }
    return 0;
}

/// Describes a null test format as its sample rate, bit depth, and sample type.
juce::String describeFormat(const NullTestFormat& format)
{
    return utils::format(
        "{} Hz {}-bit {}", format.sampleRate, format.bitsPerSample, format.floatingPoint ? "float" : "integer"
    );
}

/// Returns the lag of the actual signal against the expected one, within the checked range,
/// at which their cross-correlation is largest.
int findBestLag(const std::vector<int>& expected, const std::vector<int>& actual)
{
    const auto expectedLength = static_cast<int>(expected.size());
    const auto actualLength = static_cast<int>(actual.size());
    int bestLag = 0;
    double bestCorrelation = -std::numeric_limits<double>::infinity();

    for (int lag = -maximumNullTestLag; lag <= maximumNullTestLag; ++lag) {
        double correlation = 0.0;
        for (int sample = juce::jmax(0, -lag); sample < expectedLength && sample + lag < actualLength; ++sample) {
            const int actualSample = sample + lag;
            correlation += static_cast<double>(expected[static_cast<std::size_t>(sample)])
                * static_cast<double>(actual[static_cast<std::size_t>(actualSample)]);
        }
        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            bestLag = lag;
        }
    }
    return bestLag;
}
}  // namespace

/// Checks every measurement field and both complete histories against an independent readback.
void expectAnalysisMeasurementsEqual(
    juce::UnitTest& test,
    const AudioAnalysisRecord& actual,
    const AudioAnalysisRecord& expected
)
{
    test.expect(actual.isReady() && expected.isReady());
    test.expect(actual.nativeWaveform.has_value() == expected.nativeWaveform.has_value());
    if (actual.nativeWaveform && expected.nativeWaveform) {
        test.expect(
            *actual.nativeWaveform == *expected.nativeWaveform,
            "Native waveform must describe the same measured PCM independent of chunk boundaries"
        );
    }
    test.expect((actual.spectrogram != nullptr) == (expected.spectrogram != nullptr));
    if (actual.spectrogram != nullptr && expected.spectrogram != nullptr) {
        test.expect(
            serializeSpectrogram(*actual.spectrogram) == serializeSpectrogram(*expected.spectrogram),
            "The spectral product must describe the exact measured PCM, including final output quantization"
        );
    }
    test.expectWithinAbsoluteError(actual.peakLeft, expected.peakLeft, 0.0f);
    test.expectWithinAbsoluteError(actual.peakRight, expected.peakRight, 0.0f);
    test.expectWithinAbsoluteError(actual.overallPeak, expected.overallPeak, 0.0f);
    test.expectWithinAbsoluteError(actual.truePeakLeft, expected.truePeakLeft, 0.0);
    test.expectWithinAbsoluteError(actual.truePeakRight, expected.truePeakRight, 0.0);
    test.expectWithinAbsoluteError(actual.overallTruePeak, expected.overallTruePeak, 0.0);
    test.expectWithinAbsoluteError(actual.integratedLufs, expected.integratedLufs, 0.0);
    test.expectWithinAbsoluteError(actual.maxShortTermLufs, expected.maxShortTermLufs, 0.0);
    test.expectWithinAbsoluteError(actual.durationSeconds, expected.durationSeconds, 0.0);
    test.expect(actual.signalMetrics.plrDb == expected.signalMetrics.plrDb);
    test.expect(actual.signalMetrics.psrDb == expected.signalMetrics.psrDb);
    test.expect(actual.signalMetrics.crestFactorDb == expected.signalMetrics.crestFactorDb);
    test.expect(actual.signalMetrics.dcOffsets == expected.signalMetrics.dcOffsets);
    test.expect(actual.signalMetrics.clipCounts == expected.signalMetrics.clipCounts);
    test.expect(actual.signalMetrics.stereoCorrelation == expected.signalMetrics.stereoCorrelation);
    test.expectEquals(actual.lengthInSamples, expected.lengthInSamples);
    test.expectEquals(actual.sampleRate, expected.sampleRate);
    test.expectEquals(actual.channels, expected.channels);
    test.expect(actual.shortTermLoudness != nullptr && expected.shortTermLoudness != nullptr);
    test.expect(actual.overloads != nullptr && expected.overloads != nullptr);
    if (actual.shortTermLoudness != nullptr && expected.shortTermLoudness != nullptr) {
        test.expect(actual.shortTermLoudness->values == expected.shortTermLoudness->values);
        test.expectEquals(actual.shortTermLoudness->samplesPerPoint, expected.shortTermLoudness->samplesPerPoint);
        test.expectEquals(actual.shortTermLoudness->lengthInSamples, expected.shortTermLoudness->lengthInSamples);
        test.expectWithinAbsoluteError(
            actual.shortTermLoudness->sampleRate, expected.shortTermLoudness->sampleRate, 0.0
        );
    }
    if (actual.overloads != nullptr && expected.overloads != nullptr) {
        test.expect(actual.overloads->flags == expected.overloads->flags);
        test.expectEquals(actual.overloads->samplesPerBucket, expected.overloads->samplesPerBucket);
        test.expectEquals(actual.overloads->lengthInSamples, expected.overloads->lengthInSamples);
        test.expectEquals(actual.overloads->channelCount, expected.overloads->channelCount);
        test.expectWithinAbsoluteError(actual.overloads->sampleRate, expected.overloads->sampleRate, 0.0);
    }
}

/// Describes the result for a failed expectation:
/// the formats, layouts and lengths, the differing sample count, the residual peak in dBFS, and the best lag.
juce::String NullTestResult::describe() const
{
    if (!readable) {
        return "one of the files could not be read";
    }

    const auto residual = peakDifference > 0
        ? utils::format("{:.1f} dBFS", juce::Decibels::gainToDecibels(static_cast<float>(peakDifference) / pcm16Scale))
        : juce::String("silent");
    return utils::format(
        "{} with {} channels and {} samples expected, {} with {} channels and {} samples written, "
        "{} samples differ, residual peak {} ({} steps), best correlation at a lag of {} samples",
        describeFormat(expectedFormat),
        expectedChannels,
        expectedLength,
        describeFormat(actualFormat),
        actualChannels,
        actualLength,
        differingSamples,
        residual,
        peakDifference,
        bestLag
    );
}

/// Subtracts the expected file from the actual file sample by sample, and finds the lag at which they correlate best.
NullTestResult nullTestFiles(const juce::File& expected, const juce::File& actual)
{
    NullTestResult result;
    const auto expectedSamples = readIntegerSamples(expected);
    const auto actualSamples = readIntegerSamples(actual);
    if (!expectedSamples.has_value() || !actualSamples.has_value()) {
        return result;
    }

    const auto& expectedChannels = expectedSamples->channels;
    const auto& actualChannels = actualSamples->channels;
    result.readable = true;
    result.expectedFormat = expectedSamples->format;
    result.actualFormat = actualSamples->format;
    result.expectedChannels = static_cast<int>(expectedChannels.size());
    result.actualChannels = static_cast<int>(actualChannels.size());
    result.expectedLength = expectedChannels.empty() ? 0 : static_cast<juce::int64>(expectedChannels.front().size());
    result.actualLength = actualChannels.empty() ? 0 : static_cast<juce::int64>(actualChannels.front().size());

    const auto channels = juce::jmin(expectedChannels.size(), actualChannels.size());
    for (std::size_t channel = 0; channel < channels; ++channel) {
        const auto& expectedChannel = expectedChannels[channel];
        const auto& actualChannel = actualChannels[channel];
        const auto length = juce::jmin(expectedChannel.size(), actualChannel.size());
        for (std::size_t sample = 0; sample < length; ++sample) {
            const auto difference = std::abs(actualChannel[sample] - expectedChannel[sample]);
            if (difference != 0) {
                ++result.differingSamples;
                result.peakDifference = juce::jmax(result.peakDifference, difference);
            }
        }
    }

    // The lag only explains a difference, so matching files skip the correlation.
    if (channels > 0 && !result.cancels()) {
        result.bestLag = findBestLag(expectedChannels.front(), actualChannels.front());
    }
    return result;
}

/// Returns the AudioBatch Test MS VST3 bundle that CMake builds from test-plugin/ next to the tests.
juce::File testPluginBundle()
{
    // Set by CMake to the bundle that the test plugin target builds for the configuration of the tests.
    return {AUDIOBATCH_TEST_VST3_PATH};
}

/// Removes native backends before installing the simulated interface, preventing tests from opening hardware.
void installTestAudioDevice(juce::AudioDeviceManager& manager, TestAudioDeviceState& state)
{
    manager.closeAudioDevice();
    while (!manager.getAvailableDeviceTypes().isEmpty()) {
        manager.removeAudioDeviceType(manager.getAvailableDeviceTypes().getLast());
    }
    manager.addAudioDeviceType(std::make_unique<TestAudioDeviceType>(state));
}

/// Advances the real audio callback synchronously, without a device thread or timing-dependent waits.
void renderTestAudioDevice(const TestAudioDeviceState& state, const int sampleCount)
{
    if (state.callback != nullptr) {
        juce::AudioBuffer<float> output(2, sampleCount);
        state.callback->audioDeviceIOCallbackWithContext(
            nullptr, 0, output.getArrayOfWritePointers(), 2, sampleCount, {}
        );
    }
}

/// Installs the capture after saving the runner's logger.
ScopedLogCapture::ScopedLogCapture() : previousLogger(juce::Logger::getCurrentLogger())
{
    juce::Logger::setCurrentLogger(this);
}

/// Restores the runner's logger after the tested workers have finished.
ScopedLogCapture::~ScopedLogCapture()
{
    juce::Logger::setCurrentLogger(previousLogger);
}

/// Returns a snapshot without racing worker-thread log messages.
juce::StringArray ScopedLogCapture::getMessages() const
{
    const juce::ScopedLock scopedLock(lock);
    return messages;
}

/// Collects one complete message under the capture lock.
void ScopedLogCapture::logMessage(const juce::String& message)
{
    const juce::ScopedLock scopedLock(lock);
    messages.add(message);
}

/// Creates a uniquely owned folder, including across test binaries running in different build configurations.
/// A nonexistent-name lookup followed by creation can otherwise select the same path in two processes.
TemporaryDirectory::TemporaryDirectory() :
    directory(
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("AudioBatchTests")
            .getChildFile(utils::format("run-{}", juce::Uuid().toString()))
    )
{
    if (directory.createDirectory().failed()) {
        throw std::runtime_error("Could not create test temporary directory");
    }
}

/// Deletes the folder and everything in it.
TemporaryDirectory::~TemporaryDirectory()
{
    directory.deleteRecursively();
}

/// Returns a file inside the folder, which does not need to exist yet.
juce::File TemporaryDirectory::getChildFile(const juce::String& name) const
{
    return directory.getChildFile(name);
}

/// Copies a fixture into the folder, optionally under a new name, and returns the copy.
/// Tests that modify files work on copies so the committed fixtures never change.
/// Logs which fixture could not be copied and throws when the copy fails,
/// since every later check on the missing copy would fail with a misleading message.
/// The JUCE runner reports the exception as a failure of the suite.
juce::File TemporaryDirectory::copyFixture(const juce::String& fixtureName, const juce::String& copyName) const
{
    const auto source = fixtureFile(fixtureName);
    const auto copy = directory.getChildFile(copyName.isEmpty() ? fixtureName : copyName);
    if (!source.copyFileTo(copy) || !copy.existsAsFile()) {
        const auto message = utils::format(
            "Could not copy the fixture {} to {}", source.getFullPathName().quoted(), copy.getFullPathName().quoted()
        );
        juce::Logger::writeToLog(message);
        throw std::runtime_error(message.toStdString());
    }
    return copy;
}

/// Returns the committed fixture with the given file name from the root fixtures directory.
juce::File fixtureFile(const juce::String& name)
{
    // Set by CMake to the absolute path of the fixtures directory at the repository root.
    return juce::File(AUDIOBATCH_TEST_FIXTURES_DIR).getChildFile(name);
}

/// Returns the sample peak of an analysis record in dBFS.
double peakDecibels(const AudioAnalysisRecord& record)
{
    return static_cast<double>(juce::Decibels::gainToDecibels(std::abs(record.overallPeak)));
}

/// Returns track information with only a tempo and a key, the fields most tests care about.
serato::TrackInfo makeTrackInfo(const double bpm, const juce::String& key)
{
    serato::TrackInfo info;
    info.bpm = bpm;
    info.key = key;
    return info;
}

/// Returns the error of a failed or unanalyzed outcome, or an empty string for a success.
juce::String outcomeError(const OperationOutcome& outcome)
{
    return std::visit(
        Overloaded {
            [](const OperationFailed& failed) { return failed.error; },
            [](const CommittedButUnanalyzed& unanalyzed) { return unanalyzed.error; },
            [](const OperationSucceeded& /*succeeded*/) { return juce::String(); },
        },
        outcome
    );
}

/// Writes exact floating-point PCM into an isolated WAV for invalid-source streaming contracts.
bool writeFloatWav(const juce::File& file, const juce::AudioBuffer<float>& samples)
{
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    juce::WavAudioFormat format;
    const auto writer = format.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions()
            .withSampleRate(44'100.0)
            .withNumChannels(samples.getNumChannels())
            .withBitsPerSample(32)
            .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)
    );
    return writer != nullptr && writer->writeFromAudioSampleBuffer(samples, 0, samples.getNumSamples());
}

/// Returns the analysis of the output of an operation that wrote it, or an empty record for one that failed before.
AudioAnalysisRecord outcomeRecord(const OperationOutcome& outcome)
{
    return std::visit(
        Overloaded {
            [](const OperationFailed& /*failed*/) { return AudioAnalysisRecord(); },
            [](const CommittedButUnanalyzed& unanalyzed) { return unanalyzed.output; },
            [](const OperationSucceeded& succeeded) { return succeeded.output; },
        },
        outcome
    );
}

/// Returns the notice of a successful outcome, or an empty string.
juce::String outcomeNotice(const OperationOutcome& outcome)
{
    const auto* succeeded = std::get_if<OperationSucceeded>(&outcome);
    return succeeded != nullptr ? succeeded->notice : juce::String();
}

/// Decodes the first channel of an audio file and returns the position of its sample with the largest magnitude,
/// which for a click fixture shows where its click ended up.
/// Returns -1 when the file cannot be read.
juce::int64 peakSamplePosition(const juce::File& file)
{
    const auto reader = utils::createAudioReader(file);

    if (reader == nullptr || reader->lengthInSamples <= 0) {
        return -1;
    }

    juce::AudioBuffer<float> buffer(1, static_cast<int>(reader->lengthInSamples));
    if (!reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, false)) {
        return -1;
    }

    const std::span samples(buffer.getReadPointer(0), static_cast<std::size_t>(buffer.getNumSamples()));
    const auto peak = std::ranges::max_element(samples, {}, [](const float sample) { return std::abs(sample); });
    return std::distance(samples.begin(), peak);
}

/// Builds a small complete waveform without decoding or rendering audio.
juce::MemoryBlock validWaveform()
{
    juce::MemoryBlock result;
    juce::MemoryOutputStream output(result, false);
    output.write("jatm", 4);
    output.writeInt(16);
    output.writeInt64(32);
    output.writeInt64(32);
    output.writeInt(3);
    output.writeInt(1);
    output.writeInt(44'100);
    output.writeInt64(0);
    output.writeInt64(0);
    for (int point = 0; point < 3; ++point) {
        output.writeShort(0);
    }
    return result;
}

/// Mutates an isolated test database to exercise rows that normal cache APIs cannot write.
/// Logs the SQLite error to the test and returns false when the statement fails.
bool executeSql(juce::UnitTest& test, const juce::File& databaseFile, const char* sql)
{
    sqlite3* database = nullptr;
    const auto opened = sqlite3_open(databaseFile.getFullPathName().toRawUTF8(), &database);
    const auto completed = opened == SQLITE_OK && sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!completed && database != nullptr) {
        test.logMessage(juce::String::fromUTF8(sqlite3_errmsg(database)));
    }
    sqlite3_close(database);
    return completed;
}

/// Checks persisted SQL state without relying on the cache's decoder or identity validation.
/// The statement must return one row whose first column is the expected integer.
void expectSqlScalar(juce::UnitTest& test, const juce::File& databaseFile, const char* sql, const int expected)
{
    sqlite3* database = nullptr;
    const auto opened = sqlite3_open(databaseFile.getFullPathName().toRawUTF8(), &database);
    test.expectEquals(opened, SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    if (opened == SQLITE_OK && sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK) {
        test.expectEquals(sqlite3_step(statement), SQLITE_ROW);
        test.expectEquals(sqlite3_column_int(statement, 0), expected);
    } else {
        test.expect(false, "Cannot read test database state");
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
}

/// Returns a cancellation check that counts its calls in `calls` and reports cancellation on every call
/// after the first allowedChecks ones.
/// Counting the checks of a complete run first, with a limit it never reaches,
/// lets a test cancel the same operation at any later check, such as the last one before its output is committed.
utils::CancellationCheck cancelAfterChecks(const int allowedChecks, std::shared_ptr<std::atomic<int>> calls)
{
    return [allowedChecks, counter = std::move(calls)] { return counter->fetch_add(1) + 1 > allowedChecks; };
}

/// Runs the helper mode that the first argument selects, and returns its exit code,
/// or nothing when the arguments select no helper mode and the tests should run.
/// The helper modes run in a child process started by startHelperProcess,
/// for checks that would change the state of the test process for good or that need a second process.
std::optional<int> runHelperProcess(const juce::StringArray& arguments)
{
    if (arguments.size() == 1 && arguments[0] == messageLoopHelperArgument) {
        return runMessageLoopHelper();
    }

    if (arguments.size() == 3 && arguments[0] == scanLockHelperArgument) {
        return runScanLockHelper(arguments[1], juce::File(arguments[2]));
    }

    return std::nullopt;
}

/// Starts the test executable again as a helper process with the given arguments, capturing its standard output.
bool startHelperProcess(juce::ChildProcess& process, const juce::StringArray& helperArguments)
{
    juce::StringArray command(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName());
    command.addArray(helperArguments);
    return process.start(command, juce::ChildProcess::wantStdOut);
}

/// Reads one line of a helper's standard output without its line break, blocking until the line is complete.
/// Returns what was read so far when the helper exits first.
juce::String readHelperLine(juce::ChildProcess& process)
{
    constexpr int maximumLineLength {256};
    std::string line;
    char character {};
    while (std::cmp_less(line.size(), maximumLineLength) && process.readProcessOutput(&character, 1) == 1
           && character != '\n')
    {
        line.push_back(character);
    }
    return juce::String(line).trim();
}

/// FileDisposer for tests, which deletes the file so the user's system trash never fills up with test copies.
/// Returns true when the file is gone afterwards.
bool deleteInsteadOfTrash(const juce::File& file)
{
    return !file.exists() || file.deleteFile();
}

/// Returns default normalization options that delete replaced files instead of moving them to the system trash.
AudioNormalizationOptions testNormalizationOptions()
{
    AudioNormalizationOptions options;
    options.disposeFile = deleteInsteadOfTrash;
    return options;
}
}  // namespace audiobatch::tests
