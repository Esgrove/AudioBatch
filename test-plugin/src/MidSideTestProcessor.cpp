/// Implements the AudioBatch Test MS processor and the JUCE plugin entry point.
/// The parameters live in an AudioProcessorValueTreeState, whose XML is the saved plugin state.
/// The latency is a delay line after the processing, which hosts compensate by dropping the delayed start.
/// The processing order matches fixtures/plugin_expected.py operation by operation.

#include "MidSideTestProcessor.h"

#include <utility>

namespace audiobatch::test_plugin
{
namespace
{
constexpr auto gainParameterId {"gain"};
constexpr auto widthParameterId {"width"};
constexpr auto latencyParameterId {"latency"};
constexpr auto stateTag {"AudioBatchTestMS"};

/// Creates the gain, width, and latency parameters in the order that gives them their legacy VST3 ids.
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    const juce::NormalisableRange<float> range(0.0F, parameterMaximum);
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID {gainParameterId, 1}, "Gain", range, parameterDefault
        )
    );
    layout.add(
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID {widthParameterId, 1}, "Width", range, parameterDefault
        )
    );
    layout.add(
        std::make_unique<juce::AudioParameterInt>(
            juce::ParameterID {latencyParameterId, 1}, "Latency", 0, latencyMaximum, 0
        )
    );
    return layout;
}

/// Returns the value of a parameter the layout is known to contain.
std::atomic<float>& getParameterValue(juce::AudioProcessorValueTreeState& parameters, const juce::String& id)
{
    auto* value = parameters.getRawParameterValue(id);
    jassert(value != nullptr);
    return *value;
}
}  // namespace

/// Creates the processor with a stereo main bus, the gain and width at their unity defaults, and no latency.
MidSideTestProcessor::MidSideTestProcessor() :
    AudioProcessor(
        BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput("Output", juce::AudioChannelSet::stereo(), true)
    ),
    parameters(*this, nullptr, stateTag, createParameterLayout()),
    gain(getParameterValue(parameters, gainParameterId)),
    width(getParameterValue(parameters, widthParameterId)),
    latency(getParameterValue(parameters, latencyParameterId))
{}

/// Returns the plugin name.
const juce::String MidSideTestProcessor::getName() const
{
    return JucePlugin_Name;
}

/// The effect takes no MIDI.
bool MidSideTestProcessor::acceptsMidi() const
{
    return false;
}

/// The effect sends no MIDI.
bool MidSideTestProcessor::producesMidi() const
{
    return false;
}

/// The effect processes audio, not MIDI.
bool MidSideTestProcessor::isMidiEffect() const
{
    return false;
}

/// The output ends with the input, so there is no tail.
double MidSideTestProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

/// The plugin has a single unnamed program.
int MidSideTestProcessor::getNumPrograms()
{
    return 1;
}

/// The single program is always current.
int MidSideTestProcessor::getCurrentProgram()
{
    return 0;
}

/// Ignores program changes, since there is only one program.
void MidSideTestProcessor::setCurrentProgram(int /*index*/) {}

/// The single program has no name.
const juce::String MidSideTestProcessor::getProgramName(int /*index*/)
{
    return {};
}

/// Ignores renames, since the single program has no name.
void MidSideTestProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

/// The plugin has no editor, so hosts show their own parameter controls or none.
bool MidSideTestProcessor::hasEditor() const
{
    return false;
}

/// Returns no editor, since hasEditor is false.
juce::AudioProcessorEditor* MidSideTestProcessor::createEditor()
{
    return nullptr;
}

/// Accepts a mono or stereo main bus with the same layout on the input and the output.
bool MidSideTestProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    if (output != juce::AudioChannelSet::mono() && output != juce::AudioChannelSet::stereo()) {
        return false;
    }
    return layouts.getMainInputChannelSet() == output;
}

/// Reads the latency parameter, reports it to the host, and clears the delay line to silence.
void MidSideTestProcessor::prepareToPlay(double /*sampleRate*/, int /*maximumBlockSize*/)
{
    delaySamples = juce::jlimit(0, latencyMaximum, juce::roundToInt(latency.load()));
    delayLine.setSize(juce::jmax(1, getTotalNumOutputChannels()), juce::jmax(1, delaySamples));
    delayLine.clear();
    delayPosition = 0;
    setLatencySamples(delaySamples);
}

/// Nothing to release.
void MidSideTestProcessor::releaseResources() {}

/// Applies the width to the side signal of a stereo block and the gain to every channel,
/// then delays the block by the latency.
/// Each operation is a separate float multiply or add, in the order the expected outputs are computed.
void MidSideTestProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    const juce::ScopedNoDenormals noDenormals;
    const float gainValue = gain.load();
    const float widthValue = width.load();
    const int samples = buffer.getNumSamples();

    if (buffer.getNumChannels() == 2) {
        auto* left = buffer.getWritePointer(0);
        auto* right = buffer.getWritePointer(1);
        for (int sample = 0; sample < samples; ++sample) {
            const float mid = (left[sample] + right[sample]) * 0.5F;
            const float side = ((left[sample] - right[sample]) * 0.5F) * widthValue;
            left[sample] = (mid + side) * gainValue;
            right[sample] = (mid - side) * gainValue;
        }
    } else {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
            auto* samplesOfChannel = buffer.getWritePointer(channel);
            for (int sample = 0; sample < samples; ++sample) {
                samplesOfChannel[sample] = samplesOfChannel[sample] * gainValue;
            }
        }
    }

    delayBlock(buffer);
}

/// Delays every channel of the block by the latency the plugin reported, through one ring buffer per channel.
void MidSideTestProcessor::delayBlock(juce::AudioBuffer<float>& buffer)
{
    if (delaySamples == 0) {
        return;
    }

    const int channels = juce::jmin(buffer.getNumChannels(), delayLine.getNumChannels());
    int position = delayPosition;
    for (int channel = 0; channel < channels; ++channel) {
        auto* samplesOfChannel = buffer.getWritePointer(channel);
        auto* ring = delayLine.getWritePointer(channel);
        position = delayPosition;
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            std::swap(samplesOfChannel[sample], ring[position]);
            position = (position + 1) % delaySamples;
        }
    }
    delayPosition = position;
}

/// Saves the parameter values as the XML of the parameter tree.
void MidSideTestProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    if (const auto xml = parameters.copyState().createXml()) {
        copyXmlToBinary(*xml, destinationData);
    }
}

/// Restores parameter values saved by getStateInformation, and ignores any other data.
void MidSideTestProcessor::setStateInformation(const void* data, const int sizeInBytes)
{
    if (const auto xml = getXmlFromBinary(data, sizeInBytes); xml != nullptr && xml->hasTagName(stateTag)) {
        parameters.replaceState(juce::ValueTree::fromXml(*xml));
    }
}
}  // namespace audiobatch::test_plugin

/// Creates the processor for the JUCE plugin wrapper.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new audiobatch::test_plugin::MidSideTestProcessor();
}
