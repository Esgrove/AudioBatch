/// Configurable in-process effect shared by plugin processing, preparation, and coordinator tests.
/// TestPluginSettings selects gain, bus layouts, latency, tails, and synthetic failure signals.

#pragma once

#include <JuceHeader.h>

#include <cstddef>
#include <vector>

namespace audiobatch::tests
{
/// Behaviour of a TestPlugin.
/// Declaration order is retained for the designated initializers used by the fixtures.
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
    explicit TestPlugin(const TestPluginSettings& pluginSettings);

    const juce::String getName() const override;
    const juce::String getProgramName(int index) override;

    bool acceptsMidi() const override;
    bool hasEditor() const override;
    bool producesMidi() const override;

    double getTailLengthSeconds() const override;

    int getCurrentProgram() override;
    int getNumPrograms() override;

    void changeProgramName(int index, const juce::String& newName) override;
    void fillInPluginDescription(juce::PluginDescription& description) const override;
    void getStateInformation(juce::MemoryBlock& destData) override;
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void releaseResources() override;
    void reset() override;
    void setCurrentProgram(int index) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    [[nodiscard]] int getWidestBufferChannels() const;

protected:
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

private:
    juce::AudioProcessorEditor* createEditor() override;

    void applyDelays(juce::AudioBuffer<float>& buffer, int channels);

    /// Behaviour selected by the test, initialized before the current gain derived from it.
    TestPluginSettings settings;

    /// Recent samples of each main channel for latency and echo processing.
    std::vector<std::vector<float>> history;

    /// Rate used to express the echo length in seconds.
    double preparedSampleRate {44'100.0};

    /// Linear gain restored from state and advanced after each processed block.
    float currentGain;

    /// Largest channel count presented to the processor.
    int widestBuffer {0};

    /// Next sample position in the circular delay history.
    std::size_t historyPosition {0};
};
}  // namespace audiobatch::tests
