/// Implements the AudioBatch Test MS processor and the JUCE plugin entry point.
/// The parameters live in an AudioProcessorValueTreeState, whose XML is the saved plugin state.
/// The processing order matches fixtures/plugin_expected.py operation by operation.

#include "MidSideTestProcessor.h"

namespace audiobatch::test_plugin
{
namespace
{
constexpr auto gainParameterId {"gain"};
constexpr auto widthParameterId {"width"};
constexpr auto stateTag {"AudioBatchTestMS"};

/// Creates the gain and width parameters in the order that gives them their legacy VST3 ids.
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

/// Creates the processor with a stereo main bus and both parameters at their unity defaults.
MidSideTestProcessor::MidSideTestProcessor() :
    AudioProcessor(
        BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput("Output", juce::AudioChannelSet::stereo(), true)
    ),
    parameters(*this, nullptr, stateTag, createParameterLayout()),
    gain(getParameterValue(parameters, gainParameterId)),
    width(getParameterValue(parameters, widthParameterId))
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

/// Nothing to prepare, since the processing keeps no state between samples.
void MidSideTestProcessor::prepareToPlay(double /*sampleRate*/, int /*maximumBlockSize*/) {}

/// Nothing to release.
void MidSideTestProcessor::releaseResources() {}

/// Applies the width to the side signal of a stereo block and the gain to every channel.
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
        return;
    }

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        auto* samplesOfChannel = buffer.getWritePointer(channel);
        for (int sample = 0; sample < samples; ++sample) {
            samplesOfChannel[sample] = samplesOfChannel[sample] * gainValue;
        }
    }
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
