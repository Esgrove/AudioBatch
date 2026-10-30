/// In-process test plugins shared by the plugin processing and coordinator tests.
/// Declares TestPlugin, a small effect whose behaviour TestPluginSettings chooses:
/// its gain, channel handling, latency, tail, and failure modes.
/// It also declares the variants that record their lifecycle or fail their process calls,
/// and the helpers that build processing options for a chain of them.

#pragma once

#include "plugins/PluginProcessing.h"

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace audiobatch::tests
{
/// Behaviour of a TestPlugin.
struct TestPluginSettings {
    /// Linear gain applied to every main channel.
    float gain {1.0f};
    /// Extra linear gain applied to the right channel only, after the channel swap.
    float rightGain {1.0f};
    /// Swaps the left and right channels before the gain.
    bool swapChannels {false};
    /// Accepts a mono main bus in addition to stereo.
    bool acceptsMono {false};
    /// Refuses a stereo main bus, which together with acceptsMono makes a mono-only plugin.
    bool refusesStereo {false};
    /// Replaces the first sample of every block with infinity, like an unstable filter.
    bool producesInfinity {false};
    /// Multiplies the gain by this factor after every block, like a plugin that adapts to its input.
    /// The current gain is the plugin state, so restoring the state undoes the adaptation.
    float adaptiveGainStep {1.0f};
    /// Delays the output by this many samples and reports the delay as the plugin latency.
    int latencySamples {0};
    /// Adds an echo at half level this many samples after the input, and reports the echo as the tail.
    int echoSamples {0};
    /// Tail length reported without producing any tail, in seconds, for a plugin without an echo.
    double reportedTailSeconds {0.0};
    /// Emits a repeated pattern spanning 16-bit quantization boundaries and the PCM clipping limits.
    bool producesQuantizationPattern {false};
    /// Emits half a 16-bit step below full scale, which rounds down to the largest positive sample.
    bool producesHalfStepBelowFullScale {false};
};

/// A test plugin that applies a fixed gain and can swap the left and right channels.
/// It is stereo-only by default, like many effect plugins, and can also accept a mono layout.
/// Swapping exposes whether a mono source reaches the right input of a stereo-only plugin.
class TestPlugin : public juce::AudioPluginInstance
{
public:
    /// Creates the plugin with the given behaviour.
    explicit TestPlugin(const TestPluginSettings& pluginSettings) :
        juce::AudioPluginInstance(
            BusesProperties()
                .withInput("Input", juce::AudioChannelSet::stereo())
                .withOutput("Output", juce::AudioChannelSet::stereo())
        ),
        settings(pluginSettings),
        currentGain(pluginSettings.gain)
    {}

    /// Applies the gain and the optional channel swap to the main bus channels only,
    /// like a hosted plugin, which never touches buffer channels outside its own buses.
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/) override
    {
        widestBuffer = juce::jmax(widestBuffer, buffer.getNumChannels());
        const auto channels = juce::jmin(buffer.getNumChannels(), getMainBusNumOutputChannels());
        applyDelays(buffer, channels);

        if (settings.swapChannels && channels >= 2) {
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
                const auto left = buffer.getSample(0, sample);
                buffer.setSample(0, sample, buffer.getSample(1, sample));
                buffer.setSample(1, sample, left);
            }
        }

        for (int channel = 0; channel < channels; ++channel) {
            buffer.applyGain(channel, 0, buffer.getNumSamples(), currentGain);
        }

        currentGain *= settings.adaptiveGainStep;

        if (channels >= 2) {
            buffer.applyGain(1, 0, buffer.getNumSamples(), settings.rightGain);
        }

        if (settings.producesInfinity && channels > 0) {
            buffer.setSample(0, 0, std::numeric_limits<float>::infinity());
        }

        if (settings.producesQuantizationPattern) {
            constexpr float leastSignificantBit {1.0f / 32'768.0f};
            constexpr std::array pattern {
                0.0f,
                0.25f * leastSignificantBit,
                -0.25f * leastSignificantBit,
                1.25f * leastSignificantBit,
                -1.25f * leastSignificantBit,
                1.75f * leastSignificantBit,
                -1.75f * leastSignificantBit,
                0.25f,
                -0.25f,
                1.25f,
                -1.25f,
                1.0f - leastSignificantBit,
            };
            for (int channel = 0; channel < channels; ++channel) {
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
                    buffer.setSample(channel, sample, pattern[static_cast<std::size_t>(sample) % pattern.size()]);
                }
            }
        }

        if (settings.producesHalfStepBelowFullScale) {
            for (int channel = 0; channel < channels; ++channel) {
                juce::FloatVectorOperations::fill(
                    buffer.getWritePointer(channel), 32'767.5f / 32'768.0f, buffer.getNumSamples()
                );
            }
        }
    }

    /// Returns the largest channel count of any buffer passed to processBlock.
    [[nodiscard]] int getWidestBufferChannels() const
    {
        return widestBuffer;
    }

    /// Describes the plugin for the host.
    void fillInPluginDescription(juce::PluginDescription& description) const override
    {
        description.name = getName();
        description.pluginFormatName = "Test";
        description.numInputChannels = 2;
        description.numOutputChannels = 2;
    }

    /// Returns the plugin name.
    const juce::String getName() const override
    {
        return "Test Plugin";
    }

    /// Reports the latency and clears the delay history.
    void prepareToPlay(const double sampleRate, int /*maximumExpectedSamplesPerBlock*/) override
    {
        preparedSampleRate = sampleRate;
        setLatencySamples(settings.latencySamples);
        const auto historyLength
            = static_cast<std::size_t>(juce::jmax(settings.latencySamples, settings.echoSamples)) + 1;
        history.assign(2, std::vector<float>(historyLength, 0.0f));
        historyPosition = 0;
    }

    /// Clears the delay history, so no signal from an earlier file is left in it.
    void reset() override
    {
        for (auto& channelHistory : history) {
            std::ranges::fill(channelHistory, 0.0f);
        }

        historyPosition = 0;
    }

    /// Holds no resources.
    void releaseResources() override {}

    /// Returns the echo length, or the reported tail when there is no echo.
    double getTailLengthSeconds() const override
    {
        return settings.echoSamples > 0 ? settings.echoSamples / preparedSampleRate : settings.reportedTailSeconds;
    }

    /// Takes no MIDI input.
    bool acceptsMidi() const override
    {
        return false;
    }

    /// Produces no MIDI output.
    bool producesMidi() const override
    {
        return false;
    }

    /// Has no editor.
    bool hasEditor() const override
    {
        return false;
    }

    /// Has a single program.
    int getNumPrograms() override
    {
        return 1;
    }

    /// Always uses the single program.
    int getCurrentProgram() override
    {
        return 0;
    }

    /// Ignores program changes.
    void setCurrentProgram(int /*index*/) override {}

    /// Returns no program name.
    const juce::String getProgramName(int /*index*/) override
    {
        return {};
    }

    /// Ignores program renames.
    void changeProgramName(int /*index*/, const juce::String& /*newName*/) override {}

    /// Saves the current gain as the plugin state.
    void getStateInformation(juce::MemoryBlock& destData) override
    {
        destData.replaceAll(&currentGain, sizeof(currentGain));
    }

    /// Restores the current gain from a state saved by getStateInformation.
    void setStateInformation(const void* data, const int sizeInBytes) override
    {
        if (std::cmp_equal(sizeInBytes, sizeof(currentGain))) {
            std::memcpy(&currentGain, data, sizeof(currentGain));
        }
    }

protected:
    /// Accepts only a stereo main bus on both sides, like many effect plugins.
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto input = layouts.getMainInputChannelSet();
        const auto output = layouts.getMainOutputChannelSet();

        if (settings.acceptsMono && input == juce::AudioChannelSet::mono() && output == juce::AudioChannelSet::mono()) {
            return true;
        }

        return !settings.refusesStereo && input == juce::AudioChannelSet::stereo()
            && output == juce::AudioChannelSet::stereo();
    }

private:
    /// Has no editor.
    juce::AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }

    /// Delays the main channels by the latency and adds the echo, through a per-channel history of recent input.
    void applyDelays(juce::AudioBuffer<float>& buffer, const int channels)
    {
        if ((settings.latencySamples == 0 && settings.echoSamples == 0) || history.empty()) {
            return;
        }

        const auto historyLength = history.front().size();
        const auto delayedPosition = [&](const int delay) {
            return (historyPosition + historyLength - static_cast<std::size_t>(delay)) % historyLength;
        };

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            for (int channel = 0; channel < juce::jmin(channels, 2); ++channel) {
                auto& channelHistory = history[static_cast<std::size_t>(channel)];
                channelHistory[historyPosition] = buffer.getSample(channel, sample);

                auto output = channelHistory[delayedPosition(settings.latencySamples)];
                if (settings.echoSamples > 0) {
                    output += 0.5f * channelHistory[delayedPosition(settings.echoSamples)];
                }

                buffer.setSample(channel, sample, output);
            }

            historyPosition = (historyPosition + 1) % historyLength;
        }
    }

    /// Behaviour selected by the test.
    TestPluginSettings settings;
    /// Linear gain restored from state and advanced after each processed block.
    float currentGain;
    /// Largest channel count presented to the processor.
    int widestBuffer {0};
    /// Rate used to express the echo length in seconds.
    double preparedSampleRate {44'100.0};
    /// Recent samples of each main channel for latency and echo processing.
    std::vector<std::vector<float>> history;
    /// Next sample position in the circular delay history.
    std::size_t historyPosition {0};
};

/// Records preparation order and emulates an Audio Unit that needs initialization before state restoration.
/// Its null native handle exercises the AU lifecycle without calling a real Audio Unit.
class PreparationTestPlugin final : public TestPlugin, public juce::AudioPluginExtensions::AudioUnitClient
{
public:
    /// Starts with a saved gain different from the unity gain reinstated by every AU preparation.
    explicit PreparationTestPlugin(const bool emulateAudioUnit) :
        TestPlugin({.gain = 0.25f}),
        audioUnit(emulateAudioUnit)
    {}

    /// Advertises AU extensions only when testing the Audio Unit lifecycle.
    juce::AudioPluginExtensions::AudioUnitClient* getAudioUnitClient() override
    {
        return audioUnit ? this : nullptr;
    }

    /// Exposes the same format identity to const host queries.
    const juce::AudioPluginExtensions::AudioUnitClient* getAudioUnitClient() const override
    {
        return audioUnit ? this : nullptr;
    }

    /// Prevents native offline-render and latency queries from reaching the operating system.
    AudioUnit getAudioUnitHandle() const noexcept override
    {
        return nullptr;
    }

    /// Emulates AU initialization resetting parameters, so a second preparation destroys restored state.
    void prepareToPlay(const double sampleRate, const int samplesPerBlock) override
    {
        lifecycle.add("prepare");
        preparedBlockSize = samplesPerBlock;
        lastSampleRate = sampleRate;
        TestPlugin::prepareToPlay(sampleRate, samplesPerBlock);
        initialized = true;
        if (audioUnit) {
            constexpr float defaultGain = 1.0f;
            TestPlugin::setStateInformation(&defaultGain, sizeof(defaultGain));
        }
    }

    /// Rejects premature AU restoration and records whether other formats were restored before activation.
    void setStateInformation(const void* data, const int sizeInBytes) override
    {
        lifecycle.add(initialized ? "restore-initialized" : "restore-inactive");
        if (!audioUnit || initialized) {
            TestPlugin::setStateInformation(data, sizeInBytes);
        }
    }

    /// Records the chain reset without changing the restored gain.
    void reset() override
    {
        lifecycle.add("reset");
        TestPlugin::reset();
    }

    /// Requires the next file to initialize the plugin again before AU state can be restored.
    void releaseResources() override
    {
        lifecycle.add("release");
        initialized = false;
        TestPlugin::releaseResources();
    }

    /// Returns the ordered host lifecycle calls without allowing tests to mutate the record.
    [[nodiscard]] const juce::StringArray& getLifecycle() const
    {
        return lifecycle;
    }

    /// Returns the block size supplied by the most recent preparation.
    [[nodiscard]] int getPreparedBlockSize() const
    {
        return preparedBlockSize;
    }

    /// Returns the sample rate supplied by the most recent preparation.
    [[nodiscard]] double getLastSampleRate() const
    {
        return lastSampleRate;
    }

    /// Starts a fresh lifecycle record without changing the plugin's initialization state.
    void clearLifecycle()
    {
        lifecycle.clear();
    }

private:
    /// Ordered host lifecycle calls, excluding audio rendering.
    juce::StringArray lifecycle;
    /// Most recent preparation block size, including editor-specific sizes.
    int preparedBlockSize {0};
    /// Most recent preparation sample rate.
    double lastSampleRate {0.0};
    /// Advertises the Audio Unit lifecycle when enabled.
    bool audioUnit;
    /// True between preparation and resource release.
    bool initialized {false};
};

/// Records its resource release and its destruction in a log owned by the test, which outlives the plugin.
class TeardownRecordingPlugin final : public TestPlugin
{
public:
    /// Writes the teardown events to the given log.
    explicit TeardownRecordingPlugin(juce::StringArray& eventLog) : TestPlugin(TestPluginSettings {}), events(eventLog)
    {}

    /// Records the destruction after any release.
    ~TeardownRecordingPlugin() override
    {
        events.add("destroy");
    }

    TeardownRecordingPlugin(const TeardownRecordingPlugin&) = delete;
    TeardownRecordingPlugin(TeardownRecordingPlugin&&) = delete;
    TeardownRecordingPlugin& operator=(const TeardownRecordingPlugin&) = delete;
    TeardownRecordingPlugin& operator=(TeardownRecordingPlugin&&) = delete;

    /// Records the release of the prepared resources.
    void releaseResources() override
    {
        events.add("release");
        TestPlugin::releaseResources();
    }

private:
    /// Teardown log owned by the test, which outlives the plugin.
    juce::StringArray& events;
};

/// Counts the change notifications of the processors it is registered on.
class CountingProcessorListener final : public juce::AudioProcessorListener
{
public:
    /// Ignores parameter changes, which the tests do not trigger.
    void audioProcessorParameterChanged(juce::AudioProcessor* /*processor*/, int /*index*/, float /*value*/) override {}

    /// Counts a change notification.
    void audioProcessorChanged(juce::AudioProcessor* /*processor*/, const ChangeDetails& /*details*/) override
    {
        ++changeCount;
    }

    /// Number of processor change notifications received.
    int changeCount {0};
};

/// The VST3 client of FailingProcessPlugin, which reports a fixed result for the last process call.
class FailingVST3Client final : public juce::AudioPluginExtensions::VST3Client
{
public:
    /// Has no component, since the plugin is not a real VST3.
    [[nodiscard]] Steinberg::Vst::IComponent* getIComponentPtr() const noexcept override
    {
        return nullptr;
    }

    /// Has no preset.
    [[nodiscard]] juce::MemoryBlock getPreset() const override
    {
        return {};
    }

    /// Accepts no preset.
    bool setPreset(const juce::MemoryBlock& /*preset*/) override
    {
        return false;
    }

    /// Reports the result a VST3 plugin returns when its process call fails.
    [[nodiscard]] int getLastProcessResult() const noexcept override
    {
        return failedResult;
    }

    /// The result code reported, kResultFalse of the VST3 SDK.
    static constexpr int failedResult {1};
};

/// A test plugin that looks like a VST3 plugin whose process calls fail, while its audio passes through.
class FailingProcessPlugin final : public TestPlugin
{
public:
    /// Creates a gain-only plugin.
    FailingProcessPlugin() : TestPlugin({}) {}

    /// Returns the client that reports the failed process call.
    juce::AudioPluginExtensions::VST3Client* getVST3Client() override
    {
        return &client;
    }

    /// Returns the client that reports the failed process call.
    const juce::AudioPluginExtensions::VST3Client* getVST3Client() const override
    {
        return &client;
    }

private:
    /// Exposes the failed process result to the host.
    FailingVST3Client client;
};

std::vector<juce::AudioPluginInstance*> chainOf(juce::AudioPluginInstance& plugin);

PluginProcessingOptions makeTestPluginOptions(
    const std::vector<juce::AudioPluginInstance*>& chain,
    const std::vector<juce::MemoryBlock>& states = {}
);
}  // namespace audiobatch::tests
