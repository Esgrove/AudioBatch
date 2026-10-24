/// Implementation of the shared test helpers:
/// the self-deleting temporary directory and the lookup of the committed audio fixtures.

#include "TestHelpers.h"

#include <cmath>
#include <memory>
#include <utility>

namespace audiobatch::tests
{
namespace
{
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
    juce::BigInteger getActiveOutputChannels() const override
    {
        return outputs;
    }
    /// Returns the recording channels actually requested by the application.
    juce::BigInteger getActiveInputChannels() const override
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
        double,
        int
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
    juce::StringArray getDeviceNames(bool) const override
    {
        return {"Test output"};
    }
    /// Always selects the only interface.
    int getDefaultDeviceIndex(bool) const override
    {
        return 0;
    }
    /// All created devices belong to the single advertised interface.
    int getIndexOfDevice(juce::AudioIODevice*, bool) const override
    {
        return 0;
    }
    /// Allows output-only operation without an input device name.
    bool hasSeparateInputsAndOutputs() const override
    {
        return true;
    }
    /// Creates a fresh driver whose lifetime is controlled by the real device manager.
    juce::AudioIODevice* createDevice(const juce::String&, const juce::String&) override
    {
        return new TestAudioDevice(state);
    }

private:
    TestAudioDeviceState& state;
};
}  // namespace

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
void renderTestAudioDevice(TestAudioDeviceState& state, const int sampleCount)
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
    const auto copy = directory.getChildFile(copyName.isEmpty() ? fixtureName : copyName);
    fixtureFile(fixtureName).copyFileTo(copy);
    return copy;
}

juce::File fixtureFile(const juce::String& name)
{
    // Set by CMake to the absolute path of tests/fixtures.
    return juce::File(AUDIOBATCH_TEST_FIXTURES_DIR).getChildFile(name);
}

double peakDecibels(const AudioAnalysisRecord& record)
{
    return static_cast<double>(juce::Decibels::gainToDecibels(std::abs(record.overallPeak)));
}
}  // namespace audiobatch::tests
