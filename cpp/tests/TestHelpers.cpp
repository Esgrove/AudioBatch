/// Implementation of the shared test helpers:
/// the self-deleting temporary directory, the lookup of the committed audio fixtures and the test plugin,
/// the null test that compares a processed file against its expected output,
/// and the helper modes that run the test executable again as a child process.

#include "TestHelpers.h"

#include "analysis/AudioAnalysisTypes.h"
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
    explicit TestAudioDevice(TestAudioDeviceState& deviceState) :
        juce::AudioIODevice("Test output", "Test audio"),
        state(deviceState)
    {}

    /// Mirrors native drivers that release their resources on destruction.
    ~TestAudioDevice() override
    {
        close();
    }

    TestAudioDevice(const TestAudioDevice&) = delete;
    TestAudioDevice& operator=(const TestAudioDevice&) = delete;
    TestAudioDevice(TestAudioDevice&&) = delete;
    TestAudioDevice& operator=(TestAudioDevice&&) = delete;

    /// Exposes a stereo playback interface.
    juce::StringArray getOutputChannelNames() override
    {
        return {"Left", "Right"};
    }
    /// Exposes inputs so tests can detect accidentally requesting recording channels.
    juce::StringArray getInputChannelNames() override
    {
        return {"Left", "Right"};
    }
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
    /// Matches the simulated callback size.
    int getDefaultBufferSize() override
    {
        return 512;
    }
    /// Reports the prepared callback size.
    int getCurrentBufferSizeSamples() override
    {
        return 512;
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
    /// Adds no hardware output delay.
    int getOutputLatencyInSamples() override
    {
        return 0;
    }
    /// Adds no hardware input delay.
    int getInputLatencyInSamples() override
    {
        return 0;
    }
    /// Returns the channels actually requested by the application.
    [[nodiscard]] juce::BigInteger getActiveOutputChannels() const override
    {
        return outputs;
    }
    /// Returns the recording channels actually requested by the application.
    [[nodiscard]] juce::BigInteger getActiveInputChannels() const override
    {
        return inputs;
    }
    /// Exposes explicit failures injected by the test.
    juce::String getLastError() override
    {
        return state.openError;
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

    /// Captures the requested channels or returns the injected device-open error.
    juce::String open(
        const juce::BigInteger& inputChannels,
        const juce::BigInteger& outputChannels,
        double /*sampleRate*/,
        int /*bufferSizeSamples*/
    ) override
    {
        if (state.openError.isNotEmpty()) {
            return state.openError;
        }
        inputs = inputChannels;
        outputs = outputChannels;
        state.inputChannels = inputs.countNumberOfSetBits();
        state.outputChannels = outputs.countNumberOfSetBits();
        opened = true;
        ++state.opens;
        return {};
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
        juce::AudioIODeviceType("Test audio"),
        state(deviceState)
    {}
    /// Needs no native device enumeration.
    void scanForDevices() override {}
    /// Offers the same interface for input and output.
    juce::StringArray getDeviceNames(bool /*wantInputNames*/) const override
    {
        return {"Test output"};
    }
    /// Always selects the only interface.
    int getDefaultDeviceIndex(bool /*forInput*/) const override
    {
        return 0;
    }
    /// All created devices belong to the single advertised interface.
    int getIndexOfDevice(juce::AudioIODevice* /*device*/, bool /*asInput*/) const override
    {
        return 0;
    }
    /// Allows output-only operation without an input device name.
    bool hasSeparateInputsAndOutputs() const override
    {
        return true;
    }
    /// Creates a fresh driver whose lifetime is controlled by the real device manager.
    juce::AudioIODevice* createDevice(
        const juce::String& /*outputDeviceName*/,
        const juce::String& /*inputDeviceName*/
    ) override
    {
        return new TestAudioDevice(state);
    }

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

TemporaryDirectory::TemporaryDirectory() :
    directory(
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("AudioBatchTests")
            .getNonexistentChildFile("run", "", false)
    )
{
    directory.createDirectory();
}

TemporaryDirectory::~TemporaryDirectory()
{
    directory.deleteRecursively();
}

juce::File TemporaryDirectory::getChildFile(const juce::String& name) const
{
    return directory.getChildFile(name);
}

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

juce::File fixtureFile(const juce::String& name)
{
    // Set by CMake to the absolute path of the fixtures directory at the repository root.
    return juce::File(AUDIOBATCH_TEST_FIXTURES_DIR).getChildFile(name);
}

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
