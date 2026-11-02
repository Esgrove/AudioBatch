/// Implements the configurable test effect, including delay history, saved gain, and synthetic output patterns.

#include "TestPlugin.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace audiobatch::tests
{
/// Creates the plugin with the given behaviour.
TestPlugin::TestPlugin(const TestPluginSettings& pluginSettings) :
    juce::AudioPluginInstance(
        BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo())
            .withOutput("Output", juce::AudioChannelSet::stereo())
    ),
    settings(pluginSettings),
    currentGain(pluginSettings.gain)
{}

/// Returns the largest channel count of any buffer passed to processBlock.
int TestPlugin::getWidestBufferChannels() const
{
    return widestBuffer;
}

/// Returns the plugin name.
const juce::String TestPlugin::getName() const
{
    return "Test Plugin";
}

/// Returns no program name.
const juce::String TestPlugin::getProgramName(int /*index*/)
{
    return {};
}

/// Takes no MIDI input.
bool TestPlugin::acceptsMidi() const
{
    return false;
}

/// Has no editor.
bool TestPlugin::hasEditor() const
{
    return false;
}

/// Produces no MIDI output.
bool TestPlugin::producesMidi() const
{
    return false;
}

/// Returns the echo length, or the reported tail when there is no echo.
double TestPlugin::getTailLengthSeconds() const
{
    return settings.echoSamples > 0 ? settings.echoSamples / preparedSampleRate : settings.reportedTailSeconds;
}

/// Always uses the single program.
int TestPlugin::getCurrentProgram()
{
    return 0;
}

/// Has a single program.
int TestPlugin::getNumPrograms()
{
    return 1;
}

/// Ignores program renames.
void TestPlugin::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

/// Describes the plugin for the host.
void TestPlugin::fillInPluginDescription(juce::PluginDescription& description) const
{
    description.name = getName();
    description.pluginFormatName = "Test";
    description.numInputChannels = 2;
    description.numOutputChannels = 2;
}

/// Saves the current gain as the plugin state.
void TestPlugin::getStateInformation(juce::MemoryBlock& destData)
{
    destData.replaceAll(&currentGain, sizeof(currentGain));
}

/// Reports the latency and clears the delay history.
void TestPlugin::prepareToPlay(const double sampleRate, int /*maximumExpectedSamplesPerBlock*/)
{
    preparedSampleRate = sampleRate;
    setLatencySamples(settings.latencySamples);
    const auto historyLength = static_cast<std::size_t>(juce::jmax(settings.latencySamples, settings.echoSamples)) + 1;
    history.assign(2, std::vector<float>(historyLength, 0.0f));
    historyPosition = 0;
}

/// Applies the gain and the optional channel swap to the main bus channels only,
/// like a hosted plugin, which never touches buffer channels outside its own buses.
void TestPlugin::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
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

/// Holds no resources.
void TestPlugin::releaseResources() {}

/// Clears the delay history, so no signal from an earlier file is left in it.
void TestPlugin::reset()
{
    for (auto& channelHistory : history) {
        std::ranges::fill(channelHistory, 0.0f);
    }

    historyPosition = 0;
}

/// Ignores program changes.
void TestPlugin::setCurrentProgram(int /*index*/) {}

/// Restores the current gain from a state saved by getStateInformation.
void TestPlugin::setStateInformation(const void* data, const int sizeInBytes)
{
    if (std::cmp_equal(sizeInBytes, sizeof(currentGain))) {
        std::memcpy(&currentGain, data, sizeof(currentGain));
    }
}

/// Accepts only a stereo main bus on both sides, like many effect plugins.
bool TestPlugin::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();

    if (settings.acceptsMono && input == juce::AudioChannelSet::mono() && output == juce::AudioChannelSet::mono()) {
        return true;
    }

    return !settings.refusesStereo && input == juce::AudioChannelSet::stereo()
        && output == juce::AudioChannelSet::stereo();
}

/// Delays the main channels by the latency and adds the echo, through a per-channel history of recent input.
void TestPlugin::applyDelays(juce::AudioBuffer<float>& buffer, const int channels)
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

/// Has no editor.
juce::AudioProcessorEditor* TestPlugin::createEditor()
{
    return nullptr;
}
}  // namespace audiobatch::tests
