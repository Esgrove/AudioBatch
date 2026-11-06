/// Implements the ImageControl processor and the JUCE plugin entry point:
/// the bus layouts, reading the parameters each block, the analyzer tap,
/// and saving and restoring the parameters together with the editor settings.

#include "ImageControlProcessor.h"

#include "analysis/SpatialAnalyzer.h"
#include "analysis/StereoSampleFifo.h"
#include "ImageControlEditor.h"
#include "Parameters.h"
#include "ui/SpatialSpectrogramView.h"

#include <atomic>
#include <cstddef>
#include <span>

namespace esgrove::image_control
{
namespace
{
/// Tag of the parameter tree, and the root element of the saved state.
constexpr auto STATE_TAG {"ImageControl"};
/// Tag of the editor settings element inside the saved state.
constexpr auto EDITOR_STATE_TAG {"EditorState"};

/// Returns the raw value of a parameter the layout is known to contain.
std::atomic<float>* getRawValue(const juce::AudioProcessorValueTreeState& parameters, const juce::String& parameterId)
{
    auto* value = parameters.getRawParameterValue(parameterId);
    jassert(value != nullptr);
    return value;
}
}  // namespace

/// Creates the processor with a stereo main bus, the default parameters, and default editor settings.
ImageControlProcessor::ImageControlProcessor() :
    AudioProcessor(
        BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput("Output", juce::AudioChannelSet::stereo(), true)
    ),
    parameters(*this, nullptr, STATE_TAG, createParameterLayout()),
    editorState(EDITOR_STATE_TAG)
{
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        crossoverValues.at(crossover) = getRawValue(parameters, crossoverParameterId(crossover));
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        midGainValues.at(band) = getRawValue(parameters, midGainParameterId(band));
        sideGainValues.at(band) = getRawValue(parameters, sideGainParameterId(band));
        editorState.setProperty(linkPropertyName(band), true, nullptr);
    }
    outputGainValue = getRawValue(parameters, OUTPUT_GAIN_ID);
    bypassValue = getRawValue(parameters, BYPASS_ID);
    editorState.setProperty(ANALYZE_OUTPUT_PROPERTY, true, nullptr);
    editorState.setProperty(PERSISTENCE_PROPERTY, DEFAULT_PERSISTENCE_SECONDS, nullptr);
    editorState.setProperty(FOCUS_PROPERTY, DEFAULT_FOCUS_DB, nullptr);
    editorState.setProperty(PHASE_COLOURS_PROPERTY, false, nullptr);
    editorState.setProperty(MEL_SCALE_PROPERTY, false, nullptr);
    editorState.setProperty(SHOW_FPS_PROPERTY, false, nullptr);
    editorState.addListener(this);
}

/// Stops listening to the editor settings.
ImageControlProcessor::~ImageControlProcessor()
{
    editorState.removeListener(this);
}

/// Returns the parameter tree, for the editor's attachments.
juce::AudioProcessorValueTreeState& ImageControlProcessor::getParameters()
{
    return parameters;
}

/// Returns the editor-only settings. Use them on the message thread only.
juce::ValueTree& ImageControlProcessor::getEditorState()
{
    return editorState;
}

/// Returns the ring the audio thread fills for the editor's analyzer.
StereoSampleFifo& ImageControlProcessor::getAnalyzerFifo()
{
    return analyzerFifo;
}

/// Returns the crossover parameters ordered the way the imager applies them.
Crossovers ImageControlProcessor::getCrossovers() const
{
    Crossovers crossovers {};
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        crossovers.at(crossover) = crossoverValues.at(crossover)->load();
    }
    return orderCrossovers(crossovers);
}

/// Returns the plugin name.
const juce::String ImageControlProcessor::getName() const
{
    return JucePlugin_Name;
}

/// The single program has no name.
const juce::String ImageControlProcessor::getProgramName(int /*index*/)
{
    return {};
}

/// Creates the editor with the spatial spectrogram and the band controls. The host takes ownership.
juce::AudioProcessorEditor* ImageControlProcessor::createEditor()
{
    return new ImageControlEditor(*this);
}

/// The effect takes no MIDI.
bool ImageControlProcessor::acceptsMidi() const
{
    return false;
}

/// The plugin has its own editor.
bool ImageControlProcessor::hasEditor() const
{
    return true;
}

/// The effect processes audio, not MIDI.
bool ImageControlProcessor::isMidiEffect() const
{
    return false;
}

/// The effect sends no MIDI.
bool ImageControlProcessor::producesMidi() const
{
    return false;
}

/// Reports no tail.
/// After latency compensation, the linear-phase kernels ring for half their length after the input ends,
/// but only at the level of their stopband, so a host keeps the original length.
double ImageControlProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

/// The single program is always current.
int ImageControlProcessor::getCurrentProgram()
{
    return 0;
}

/// The plugin has a single unnamed program.
int ImageControlProcessor::getNumPrograms()
{
    return 1;
}

/// Ignores renames, since the single program has no name.
void ImageControlProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

/// Saves the parameter tree as XML, with the editor settings as a child element.
void ImageControlProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    const auto state = parameters.copyState();
    if (const auto xml = state.createXml()) {
        xml->addChildElement(editorState.createXml().release());
        copyXmlToBinary(*xml, destinationData);
    }
}

/// Prepares the imager for the sample rate and channel count with the current crossovers,
/// and reports its latency to the host.
void ImageControlProcessor::prepareToPlay(const double sampleRate, int /*maximumBlockSize*/)
{
    imager.prepare(sampleRate, getTotalNumOutputChannels(), getCrossovers());
    setLatencySamples(imager.getLatencySamples());
}

/// Processes a block with the current parameters, bypassed when the bypass parameter is on.
void ImageControlProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    processWithBypass(buffer, bypassValue->load() >= 0.5F);
}

/// Processes a block bypassed when a host bypasses the plugin without the bypass parameter.
void ImageControlProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    processWithBypass(buffer, true);
}

/// Returns the bypass parameter, which hosts use as the plugin's own bypass.
juce::AudioProcessorParameter* ImageControlProcessor::getBypassParameter() const
{
    return parameters.getParameter(BYPASS_ID);
}

/// Hands the current parameters to the imager, processes the block, and pushes the analyzer tap.
/// Bypassing glides every gain to unity instead of skipping the imager,
/// so the latency stays the same and the output becomes the delayed input bit for bit without a click.
/// Offline rendering designs moved crossovers synchronously, so its output does not depend on thread timing.
void ImageControlProcessor::processWithBypass(juce::AudioBuffer<float>& buffer, const bool bypassed)
{
    const juce::ScopedNoDenormals noDenormals;

    BandGains midGains {};
    BandGains sideGains {};
    midGains.fill(1.0F);
    sideGains.fill(1.0F);
    float outputGain = 1.0F;
    if (!bypassed) {
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            midGains.at(band) = bandGainFromDecibels(midGainValues.at(band)->load());
            sideGains.at(band) = bandGainFromDecibels(sideGainValues.at(band)->load());
        }
        outputGain = outputGainFromDecibels(outputGainValue->load());
    }
    imager.setGains(midGains, sideGains, outputGain);
    imager.setCrossovers(getCrossovers(), isNonRealtime());

    const bool tapOutput = analyzeOutput.load(std::memory_order_relaxed);
    if (!tapOutput) {
        pushToAnalyzer(buffer);
    }
    imager.process(buffer);
    if (tapOutput) {
        pushToAnalyzer(buffer);
    }
}

/// Stops the imager's design thread.
void ImageControlProcessor::releaseResources()
{
    imager.release();
}

/// Ignores program changes, since there is only one program.
void ImageControlProcessor::setCurrentProgram(int /*index*/) {}

/// Restores state saved by getStateInformation, and ignores any other data.
/// The editor settings are copied into the existing tree, so the editor's bindings to it stay valid.
void ImageControlProcessor::setStateInformation(const void* data, const int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(STATE_TAG)) {
        return;
    }
    if (auto* savedEditorState = xml->getChildByName(EDITOR_STATE_TAG)) {
        editorState.copyPropertiesFrom(juce::ValueTree::fromXml(*savedEditorState), nullptr);
        xml->removeChildElement(savedEditorState, true);
    }
    parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

/// Accepts a mono or stereo main bus with the same layout on the input and the output.
bool ImageControlProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    if (output != juce::AudioChannelSet::mono() && output != juce::AudioChannelSet::stereo()) {
        return false;
    }
    return layouts.getMainInputChannelSet() == output;
}

/// Mirrors the analyzer tap setting of the editor state for the audio thread.
void ImageControlProcessor::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree == editorState && property == juce::Identifier(ANALYZE_OUTPUT_PROPERTY)) {
        analyzeOutput.store(static_cast<bool>(tree.getProperty(property)), std::memory_order_relaxed);
    }
}

/// Pushes a block to the analyzer ring, with a mono signal on both channels.
void ImageControlProcessor::pushToAnalyzer(const juce::AudioBuffer<float>& buffer)
{
    const auto samples = static_cast<std::size_t>(buffer.getNumSamples());
    if (buffer.getNumChannels() == 0 || samples == 0) {
        return;
    }
    const std::span<const float> left(buffer.getReadPointer(0), samples);
    const std::span<const float> right(buffer.getReadPointer(buffer.getNumChannels() > 1 ? 1 : 0), samples);
    analyzerFifo.push(left, right);
}
}  // namespace esgrove::image_control

/// Creates the processor for the JUCE plugin wrapper.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new esgrove::image_control::ImageControlProcessor();
}
