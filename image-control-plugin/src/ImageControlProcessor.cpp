/// Implements the ImageControl processor and the JUCE plugin entry point:
/// the bus layouts, reading the parameters each block, the analyzer tap,
/// and saving and restoring the parameters together with the editor settings.

#include "ImageControlProcessor.h"

#include "analysis/StereoSampleFifo.h"
#include "dsp/BandCorrelation.h"
#include "dsp/MultibandImager.h"
#include "ImageControlEditor.h"
#include "Parameters.h"
#include <system_error>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

namespace esgrove::image_control
{
namespace
{
/// Tag of the parameter tree, and the root element of the saved state.
constexpr auto STATE_TAG {"ImageControl"};
/// Tag of the editor settings element inside the saved state.
constexpr auto EDITOR_STATE_TAG {"EditorState"};

/// Builds deterministic editor defaults independently of any edits on the current instance.
/// Optional geometry is absent so the editor's layout defaults apply when loading a state without it.
juce::ValueTree defaultEditorState()
{
    juce::ValueTree defaults(EDITOR_STATE_TAG);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        defaults.setProperty(linkPropertyName(band), true, nullptr);
    }
    defaults.setProperty(ANALYZE_OUTPUT_PROPERTY, true, nullptr);
    defaults.setProperty(PERSISTENCE_PROPERTY, DEFAULT_PERSISTENCE_SECONDS, nullptr);
    defaults.setProperty(PAN_MIDPOINT_PROPERTY, DEFAULT_PAN_MIDPOINT_DB, nullptr);
    defaults.setProperty(PAN_EDGE_PROPERTY, DEFAULT_PAN_EDGE_DB, nullptr);
    defaults.setProperty(CONTRAST_PROPERTY, DEFAULT_CONTRAST_PERCENT, nullptr);
    defaults.setProperty(SOLID_RENDERING_PROPERTY, false, nullptr);
    defaults.setProperty(PHASE_COLOURS_PROPERTY, false, nullptr);
    defaults.setProperty(MEL_SCALE_PROPERTY, false, nullptr);
    defaults.setProperty(SHOW_FPS_PROPERTY, false, nullptr);
    defaults.setProperty(SHOW_BAND_CORRELATION_PROPERTY, true, nullptr);
    defaults.setProperty(BYPASS_RENDERING_PROPERTY, false, nullptr);
    defaults.setProperty(HIDE_SPECTROGRAM_PROPERTY, false, nullptr);
    return defaults;
}

/// Copies a detached editor tree into plain bytes that can cross threads without sharing a live ValueTree.
juce::MemoryBlock serializeEditorState(const juce::ValueTree& state)
{
    juce::MemoryBlock snapshot;
    {
        juce::MemoryOutputStream stream(snapshot, false);
        state.writeToStream(stream);
    }
    return snapshot;
}

/// Returns whether this is the actual message thread, without creating a MessageManager on a host worker.
bool isMessageThread()
{
    const auto* manager = juce::MessageManager::getInstanceWithoutCreating();
    return manager != nullptr && manager->isThisTheMessageThread();
}

/// Replaces invalid saved numeric values with the current finite value before APVTS normalizes them.
/// Missing parameters remain absent so APVTS restores their declared defaults.
void sanitizeParameterState(const juce::ValueTree& state, const juce::AudioProcessorValueTreeState& parameters)
{
    for (auto child : state) {
        if (const auto* parameter = parameters.getParameter(child.getProperty("id").toString())) {
            const auto text = child.getProperty("value").toString().trim().toStdString();
            const std::span<const char> characters(text.data(), text.size());
            const auto number = !characters.empty() && characters.front() == '+' ? characters.subspan(1) : characters;
            const auto* last = std::to_address(number.end());
            float value = 0.0F;
            const auto parsed = std::from_chars(number.data(), last, value);
            if (parsed.ec != std::errc {} || parsed.ptr != last || !std::isfinite(value)) {
                value = plainValue(*parameter);
            }
            child.setProperty("value", static_cast<double>(value), nullptr);
        }
    }
}

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
    editorState(defaultEditorState())
{
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        crossoverValues.at(crossover) = getRawValue(parameters, crossoverParameterId(crossover));
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        midGainValues.at(band) = getRawValue(parameters, midGainParameterId(band));
        sideGainValues.at(band) = getRawValue(parameters, sideGainParameterId(band));
        panValues.at(band) = getRawValue(parameters, panParameterId(band));
        soloValues.at(band) = getRawValue(parameters, soloParameterId(band));
    }
    outputGainValue = getRawValue(parameters, OUTPUT_GAIN_ID);
    bypassValue = getRawValue(parameters, BYPASS_ID);
    deltaValue = getRawValue(parameters, DELTA_ID);
    monoValue = getRawValue(parameters, MONO_ID);
    editorStateSnapshot = serializeEditorState(editorState);
    editorState.addListener(this);
}

/// Cancels queued editor restores and stops listening to the editor settings.
/// As with the editor and audio callbacks, the host must finish any in-flight callback before destruction.
ImageControlProcessor::~ImageControlProcessor()
{
    cancelPendingUpdate();
    editorState.removeListener(this);
}

/// Returns the parameter tree, for the editor's attachments.
juce::AudioProcessorValueTreeState& ImageControlProcessor::getParameters()
{
    return parameters;
}

/// Returns the identity-stable editor settings, applying queued restores before message-thread access.
/// The returned tree and every copy or property Value bound to it may only be used on the message thread.
juce::ValueTree& ImageControlProcessor::getEditorState()
{
    jassert(isMessageThread());
    if (!restoringState) {
        handleUpdateNowIfNeeded();
        // The worker can publish a snapshot immediately before it posts the async notification.
        applyPendingEditorState();
    }
    return editorState;
}

/// Returns the ring the audio thread fills for the editor's analyzer.
StereoSampleFifo& ImageControlProcessor::getAnalyzerFifo()
{
    return analyzerFifo;
}

/// Returns the most recently published independent band correlation values.
BandCorrelations ImageControlProcessor::getBandCorrelations() const noexcept
{
    return getBandCorrelationSnapshot().readings;
}

/// Associates readings with their lifecycle so the editor can reset history even if it missed invalidation.
BandCorrelationSnapshot ImageControlProcessor::getBandCorrelationSnapshot() const noexcept
{
    const auto generation = correlationGeneration.load(std::memory_order_acquire);
    const auto updated = correlationUpdateMilliseconds.load(std::memory_order_acquire);
    const auto age = juce::Time::getMillisecondCounter() - updated;
    if (!correlationEnabled.load(std::memory_order_relaxed) || updated == 0
        || publishedCorrelationGeneration.load(std::memory_order_acquire) != generation
        || age > correlationExpiryMilliseconds.load(std::memory_order_relaxed))
    {
        BandCorrelations unavailable {};
        unavailable.fill(std::numeric_limits<float>::quiet_NaN());
        return {.readings = unavailable, .generation = generation};
    }
    auto readings = imager.getBandCorrelations();
    if (correlationGeneration.load(std::memory_order_acquire) != generation) {
        readings.fill(std::numeric_limits<float>::quiet_NaN());
    }
    return {.readings = readings, .generation = generation};
}

/// Lets an open editor request band metering. Hidden meters and closed editors disable the extra work.
void ImageControlProcessor::setCorrelationEnabled(const bool enabled) noexcept
{
    if (correlationEnabled.exchange(enabled, std::memory_order_acq_rel) != enabled) {
        correlationGeneration.fetch_add(1, std::memory_order_acq_rel);
        correlationUpdateMilliseconds.store(0, std::memory_order_release);
    }
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

/// The single program is called Default, since the VST3 validator rejects a program without a name.
const juce::String ImageControlProcessor::getProgramName(int /*index*/)
{
    return "Default";
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

/// Reports the FIR post-ringing beyond the latency-compensated input endpoint.
/// The bound is independent of parameters, since solos or unequal band gains expose the full kernel response.
double ImageControlProcessor::getTailLengthSeconds() const
{
    return imager.getTailLengthSeconds();
}

/// The single program is always current.
int ImageControlProcessor::getCurrentProgram()
{
    return 0;
}

/// The plugin has a single program.
int ImageControlProcessor::getNumPrograms()
{
    return 1;
}

/// Ignores renames, since the single program has no name.
void ImageControlProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

/// Saves the thread-safe parameter copy and detached editor snapshot, including unapplied restores.
/// Host save/restore calls serialize with each other, but never wait for message-thread dispatch.
/// Like APVTS copyState, this allocates and locks and is not suitable for the realtime audio callback.
void ImageControlProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    const std::scoped_lock stateLock(stateInformationMutex);
    const auto state = parameters.copyState();
    juce::MemoryBlock snapshot;
    {
        const std::scoped_lock editorLock(editorStateMutex);
        snapshot = editorStateSnapshot;
    }
    if (const auto xml = state.createXml()) {
        const auto editor = juce::ValueTree::readFromData(snapshot.getData(), snapshot.getSize());
        xml->addChildElement(editor.createXml().release());
        copyXmlToBinary(*xml, destinationData);
    }
}

/// Prepares the imager for the sample rate and channel count with the current crossovers,
/// and reports its latency to the host.
void ImageControlProcessor::prepareToPlay(const double sampleRate, const int maximumBlockSize)
{
    imager.prepare(sampleRate, getTotalNumOutputChannels(), getCrossovers());
    analyzerFifo.setSampleRate(sampleRate);
    analyzerFifo.beginEpoch();
    constexpr double MILLISECONDS_PER_SECOND {1000.0};
    constexpr std::uint32_t MIN_CORRELATION_EXPIRY_MILLISECONDS {250};
    const auto blockExpiry = static_cast<std::uint32_t>(
        std::ceil(static_cast<double>(std::max(maximumBlockSize, 0)) * 2.0 * MILLISECONDS_PER_SECOND / sampleRate)
    );
    correlationExpiryMilliseconds.store(
        std::max(MIN_CORRELATION_EXPIRY_MILLISECONDS, blockExpiry), std::memory_order_relaxed
    );
    correlationGeneration.fetch_add(1, std::memory_order_acq_rel);
    correlationUpdateMilliseconds.store(0, std::memory_order_release);
    monoSmoother.reset(sampleRate, GAIN_SMOOTHING_SECONDS);
    monoSmoother.setCurrentAndTargetValue(monoValue->load() >= 0.5F ? 1.0F : 0.0F);
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
/// Bypassing glides every gain to unity, every pan to the centre, and every band muted by a solo back in,
/// instead of skipping the imager,
/// so the latency stays the same and the output becomes the delayed input bit for bit without a click.
/// Delta mode subtracts the delayed input from the output, and bypassing turns it off,
/// so bypass restores the delayed input before the final Mono fold.
/// Offline rendering designs moved crossovers synchronously, so its output does not depend on thread timing.
void ImageControlProcessor::processWithBypass(juce::AudioBuffer<float>& buffer, const bool bypassed)
{
    const juce::ScopedNoDenormals noDenormals;

    BandGains midGains {};
    BandGains sideGains {};
    midGains.fill(1.0F);
    sideGains.fill(1.0F);
    BandPans pans {};
    BandSolos solos {};
    float outputGain = 1.0F;
    if (!bypassed) {
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            solos.at(band) = soloValues.at(band)->load() >= 0.5F;
            midGains.at(band) = bandGainFromDecibels(midGainValues.at(band)->load());
            sideGains.at(band) = bandGainFromDecibels(sideGainValues.at(band)->load());
            pans.at(band) = panFromPercent(panValues.at(band)->load());
        }
        outputGain = outputGainFromDecibels(outputGainValue->load());
    }
    imager.setGains(midGains, sideGains, outputGain);
    imager.setPans(pans);
    imager.setSolos(soloGains(solos));
    imager.setDelta(!bypassed && deltaValue->load() >= 0.5F);
    imager.setCrossovers(getCrossovers(), isNonRealtime());
    const auto meterGeneration = correlationGeneration.load(std::memory_order_acquire);
    const bool meterEnabled = correlationEnabled.load(std::memory_order_relaxed) && !isNonRealtime();
    if (activeCorrelationGeneration != meterGeneration) {
        imager.setCorrelationEnabled(false);
        activeCorrelationGeneration = meterGeneration;
    }
    imager.setCorrelationEnabled(meterEnabled);

    const auto analyzerEpoch = analyzerFifo.getEpoch();
    const bool tapOutput = StereoSampleFifo::analyzesOutput(analyzerEpoch);
    if (!tapOutput) {
        pushToAnalyzer(buffer, analyzerEpoch);
    }
    imager.process(buffer);
    applyMono(buffer);
    if (tapOutput) {
        pushToAnalyzer(buffer, analyzerEpoch);
    }
    if (meterEnabled && buffer.getNumSamples() > 0) {
        publishedCorrelationGeneration.store(meterGeneration, std::memory_order_release);
        correlationUpdateMilliseconds.store(juce::Time::getMillisecondCounter(), std::memory_order_release);
    }
}

/// Folds the final processed output equally into both channels before Post analysis, with a click-free blend.
/// Mono follows Delta and bypass and applies identically to real-time and offline output.
void ImageControlProcessor::applyMono(juce::AudioBuffer<float>& buffer)
{
    monoSmoother.setTargetValue(monoValue->load() >= 0.5F ? 1.0F : 0.0F);
    if (buffer.getNumChannels() < 2) {
        monoSmoother.skip(buffer.getNumSamples());
        return;
    }
    if (!monoSmoother.isSmoothing() && juce::exactlyEqual(monoSmoother.getCurrentValue(), 0.0F)) {
        return;
    }
    const auto samples = static_cast<std::size_t>(buffer.getNumSamples());
    const std::span<float> left(buffer.getWritePointer(0), samples);
    const std::span<float> right(buffer.getWritePointer(1), samples);
    for (std::size_t sample = 0; sample < samples; ++sample) {
        const float blend = monoSmoother.getNextValue();
        const float mono = (left[sample] + right[sample]) * 0.5F;
        if (juce::exactlyEqual(blend, 1.0F)) {
            left[sample] = mono;
            right[sample] = mono;
        } else {
            left[sample] += blend * (mono - left[sample]);
            right[sample] += blend * (mono - right[sample]);
        }
    }
}

/// Clears pending audio and initializes the Mono blend from its parameter, preserving parameters and prepared kernels.
void ImageControlProcessor::reset()
{
    correlationUpdateMilliseconds.store(0, std::memory_order_release);
    correlationGeneration.fetch_add(1, std::memory_order_acq_rel);
    imager.reset();
    analyzerFifo.beginEpoch();
    monoSmoother.setCurrentAndTargetValue(monoValue->load() >= 0.5F ? 1.0F : 0.0F);
}

/// Stops the imager's design thread.
void ImageControlProcessor::releaseResources()
{
    correlationUpdateMilliseconds.store(0, std::memory_order_release);
    correlationGeneration.fetch_add(1, std::memory_order_acq_rel);
    imager.release();
}

/// Ignores program changes, since there is only one program.
void ImageControlProcessor::setCurrentProgram(int /*index*/) {}

/// Restores parameters and publishes detached editor settings on any host thread, ignoring foreign state.
/// Missing editor properties take deterministic defaults, independent of the current instance.
/// The analyzer tap and subsequent saves see the restored snapshot before this returns.
/// Only the message thread updates the bound editor tree, synchronously here or through AsyncUpdater.
/// Host calls serialize by stateInformationMutex acquisition. Editor changes order by editorStateMutex publication:
/// an edit after a queued restore overrides only that property, and a later restore replaces the whole snapshot.
void ImageControlProcessor::setStateInformation(const void* data, const int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(STATE_TAG)) {
        return;
    }
    auto restoredEditor = defaultEditorState();
    if (auto* savedEditorState = xml->getChildByName(EDITOR_STATE_TAG)) {
        const auto saved = juce::ValueTree::fromXml(*savedEditorState);
        for (int property = 0; property < saved.getNumProperties(); ++property) {
            const auto name = saved.getPropertyName(property);
            restoredEditor.setProperty(name, saved.getProperty(name), nullptr);
        }
        // Linear focus has no equivalent on the pan curve, so missing curve settings take their defaults.
        restoredEditor.removeProperty(LEGACY_FOCUS_PROPERTY, nullptr);
        xml->removeChildElement(savedEditorState, true);
    }
    auto snapshot = serializeEditorState(restoredEditor);
    const auto restoredParameters = juce::ValueTree::fromXml(*xml);
    {
        const std::scoped_lock stateLock(stateInformationMutex);
        sanitizeParameterState(restoredParameters, parameters);
        {
            const std::scoped_lock editorLock(editorStateMutex);
            editorStateSnapshot = std::move(snapshot);
            editorStatePending = true;
            analyzerFifo.setTap(static_cast<bool>(restoredEditor.getProperty(ANALYZE_OUTPUT_PROPERTY, true)));
        }
        parameters.replaceState(restoredParameters);
    }
    if (isMessageThread() && !restoringState) {
        applyPendingEditorState();
    } else {
        // A restore requested reentrantly by a tree listener must run after the outer tree copy finishes.
        triggerAsyncUpdate();
    }
}

/// Applies the latest detached snapshot without holding any host-state lock across tree listeners.
/// A concurrent restore can publish again while listeners run. It remains pending and its tap stays authoritative.
void ImageControlProcessor::applyPendingEditorState()
{
    jassert(isMessageThread());
    if (restoringState) {
        return;
    }
    juce::MemoryBlock snapshot;
    {
        const std::scoped_lock editorLock(editorStateMutex);
        if (!editorStatePending) {
            return;
        }
        snapshot = editorStateSnapshot;
        editorStatePending = false;
    }
    const auto restoredEditor = juce::ValueTree::readFromData(snapshot.getData(), snapshot.getSize());
    const juce::ScopedValueSetter<bool> restoring(restoringState, true);
    editorState.copyPropertiesAndChildrenFrom(restoredEditor, nullptr);
}

/// Delivers queued settings exclusively on the message thread, preserving the bound tree's identity.
void ImageControlProcessor::handleAsyncUpdate()
{
    applyPendingEditorState();
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

/// Publishes a message-thread property edit and marks it dirty, without publishing intermediate restore callbacks.
/// Reads the bound tree only on this thread and under the snapshot lock.
/// Patching just the changed property preserves unrelated settings from a restore still queued for this tree.
void ImageControlProcessor::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    jassert(isMessageThread());
    if (tree != editorState || restoringState) {
        return;
    }
    {
        const std::scoped_lock editorLock(editorStateMutex);
        auto snapshot = juce::ValueTree::readFromData(editorStateSnapshot.getData(), editorStateSnapshot.getSize());
        if (tree.hasProperty(property)) {
            snapshot.setProperty(property, tree.getProperty(property), nullptr);
        } else {
            snapshot.removeProperty(property, nullptr);
        }
        editorStateSnapshot = serializeEditorState(snapshot);
        if (property == juce::Identifier(ANALYZE_OUTPUT_PROPERTY)) {
            analyzerFifo.setTap(static_cast<bool>(snapshot.getProperty(property, true)));
        }
    }
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true));
}

/// Pushes a block to the analyzer ring, with a mono signal on both channels.
void ImageControlProcessor::pushToAnalyzer(const juce::AudioBuffer<float>& buffer, const AnalyzerEpoch epoch)
{
    const auto samples = static_cast<std::size_t>(buffer.getNumSamples());
    if (buffer.getNumChannels() == 0 || samples == 0) {
        return;
    }
    const std::span<const float> left(buffer.getReadPointer(0), samples);
    const std::span<const float> right(buffer.getReadPointer(buffer.getNumChannels() > 1 ? 1 : 0), samples);
    analyzerFifo.push(left, right, epoch);
}
}  // namespace esgrove::image_control

/// Creates the processor for the JUCE plugin wrapper.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new esgrove::image_control::ImageControlProcessor();
}
