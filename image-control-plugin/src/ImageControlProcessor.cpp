/// Implements the ImageControl processor and the JUCE plugin entry point:
/// the bus layouts, reading the parameters each block, the analyzer tap,
/// and saving and restoring the parameters together with the editor settings.

#include "ImageControlProcessor.h"

#include "analysis/StereoSampleFifo.h"
#include "dsp/BandCorrelation.h"
#include "dsp/MultibandImager.h"
#include "ImageControlEditor.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "UserPresets.h"
#include <system_error>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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
/// Host-state comparison metadata is separately versioned from the portable sound format.
constexpr auto COMPARISON_STATE_TAG {"ComparisonState"};
constexpr int COMPARISON_STATE_VERSION {1};

/// Captures a detached host parameter tree, including defaults for parameters absent from legacy state.
/// Unlike reading atomics, this also sees the complete target during a synchronous reentrant restore save.
ProcessingSnapshot soundFromParameterTree(
    const juce::ValueTree& state,
    const juce::AudioProcessorValueTreeState& parameters
)
{
    auto snapshot = captureProcessingState(parameters);
    for (auto& [parameterId, value] : snapshot) {
        const auto* parameter = parameters.getParameter(parameterId);
        const auto child = state.getChildWithProperty("id", parameterId);
        value = child.isValid() ? static_cast<float>(child.getProperty("value"))
                                : parameter->convertFrom0to1(parameter->getDefaultValue());
    }
    return snapshot;
}

/// Reads both persistent slots atomically as a unit, rejecting malformed or future metadata without rejecting sound.
/// The top-level host parameters are authoritative for the selected slot even if its stored copy is stale.
bool restoreComparisonState(
    const juce::XmlElement& root,
    const juce::AudioProcessorValueTreeState& parameters,
    std::array<ProcessingSnapshot, 2>& slots,
    int& active
)
{
    const juce::XmlElement* comparison = nullptr;
    for (const auto* child : root.getChildIterator()) {
        if (child->hasTagName(COMPARISON_STATE_TAG)) {
            if (comparison != nullptr) {
                return false;
            }
            comparison = child;
        }
    }
    if (comparison == nullptr || comparison->getStringAttribute("version") != juce::String(COMPARISON_STATE_VERSION)
        || comparison->getNumAttributes() != 2)
    {
        return false;
    }
    const auto selected = comparison->getStringAttribute("active");
    if (selected != "0" && selected != "1") {
        return false;
    }
    std::array<bool, 2> seen {};
    for (const auto* child : comparison->getChildIterator()) {
        if (!child->hasTagName("Slot") || child->getNumAttributes() != 1) {
            return false;
        }
        const auto index = child->getStringAttribute("id");
        if (index != "0" && index != "1") {
            return false;
        }
        const auto slot = static_cast<std::size_t>(index.getIntValue());
        const auto* sound = child->getFirstChildElement();
        if (seen[slot] || sound == nullptr || sound->getNextElement() != nullptr) {
            return false;
        }
        const auto snapshot = processingStateFromXml(*sound, parameters);
        if (!snapshot) {
            return false;
        }
        slots[slot] = *snapshot;
        seen[slot] = true;
    }
    if (!seen[0] || !seen[1]) {
        return false;
    }
    active = selected.getIntValue();
    return true;
}

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
    defaults.setProperty(SMOOTH_CORRELATION_READOUT_PROPERTY, true, nullptr);
    defaults.setProperty(BYPASS_RENDERING_PROPERTY, false, nullptr);
    defaults.setProperty(HIDE_SPECTROGRAM_PROPERTY, false, nullptr);
    defaults.setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, true, nullptr);
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
/// Missing parameters are explicitly filled with declared defaults, even when restoring onto an edited instance.
void sanitizeParameterState(juce::ValueTree state, const juce::AudioProcessorValueTreeState& parameters)
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
    // APVTS preserves current raw values for missing children on a reused instance.
    // Insert declared defaults explicitly.
    for (const auto& [parameterId, unused] : captureProcessingState(parameters, false)) {
        juce::ignoreUnused(unused);
        if (!state.getChildWithProperty("id", parameterId).isValid()) {
            const auto* parameter = parameters.getParameter(parameterId);
            juce::ValueTree child("PARAM");
            child.setProperty("id", parameterId, nullptr);
            child.setProperty(
                "value", static_cast<double>(parameter->convertFrom0to1(parameter->getDefaultValue())), nullptr
            );
            state.appendChild(child, nullptr);
        }
    }
}

/// Converts an effective raw control only when its exact value changes, retaining audio-thread-owned targets.
template<typename Convert>
bool updateCachedControl(const float raw, float& previousRaw, float& target, const bool initialized, Convert convert)
{
    if (initialized && juce::exactlyEqual(raw, previousRaw)) {
        return false;
    }
    previousRaw = raw;
    target = convert(raw);
    return true;
}

/// Returns the raw value of a parameter the layout is known to contain.
std::atomic<float>& getRawValue(const juce::AudioProcessorValueTreeState& parameters, const juce::String& parameterId)
{
    // JUCE owns the lookup result, which is required by the fixed layout and remains valid for the APVTS lifetime.
    auto* value = parameters.getRawParameterValue(parameterId);
    jassert(value != nullptr);
    return *value;
}

/// Binds every indexed raw value during construction, preserving the parameter layout's index order.
template<std::size_t... Indices>
std::array<std::reference_wrapper<std::atomic<float>>, sizeof...(Indices)> getRawValues(
    const juce::AudioProcessorValueTreeState& parameters,
    juce::String (&parameterId)(std::size_t),
    std::index_sequence<Indices...> /*indices*/
)
{
    return {getRawValue(parameters, parameterId(Indices))...};
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
    editorState(defaultEditorState()),
    history(parameters),
    crossoverValues(getRawValues(parameters, crossoverParameterId, std::make_index_sequence<CROSSOVER_COUNT> {})),
    midGainValues(getRawValues(parameters, midGainParameterId, std::make_index_sequence<BAND_COUNT> {})),
    panValues(getRawValues(parameters, panParameterId, std::make_index_sequence<BAND_COUNT> {})),
    recoverValues(getRawValues(parameters, recoverParameterId, std::make_index_sequence<BAND_COUNT> {})),
    sideGainValues(getRawValues(parameters, sideGainParameterId, std::make_index_sequence<BAND_COUNT> {})),
    soloValues(getRawValues(parameters, soloParameterId, std::make_index_sequence<BAND_COUNT> {})),
    widthValues(getRawValues(parameters, widthParameterId, std::make_index_sequence<BAND_COUNT> {})),
    outputGainValue(getRawValue(parameters, OUTPUT_GAIN_ID)),
    bypassValue(getRawValue(parameters, BYPASS_ID)),
    deltaValue(getRawValue(parameters, DELTA_ID)),
    monoValue(getRawValue(parameters, MONO_ID))
{
    editorStateSnapshot = serializeEditorState(editorState);
    history.setStateMutex(stateInformationMutex);
    history.setMetadataChangedCallback([this] { notifyComparisonChanged(); });
    comparisonSlots.fill(captureProcessingState(parameters));
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

/// Returns the processor-owned history for explicit message-thread control adapters.
ParameterHistory& ImageControlProcessor::getHistory()
{
    return history;
}

/// Captures the current sound, including headless host automation but excluding audition and editor controls.
ProcessingSnapshot ImageControlProcessor::getSoundSnapshot()
{
    const std::scoped_lock stateLock(stateInformationMutex);
    return captureProcessingState(parameters);
}

/// Returns session-only preset provenance, logically cleared by a host restore without touching UI state.
std::optional<UserPreset> ImageControlProcessor::getSelectedUserPreset() const
{
    const std::scoped_lock stateLock(stateInformationMutex);
    if (selectedPresetGeneration != history.generation()) {
        return std::nullopt;
    }
    return selectedUserPresets.at(static_cast<std::size_t>(activeComparisonSlot));
}

/// Keeps the active slot's preset baseline across editor replacement, independently of its source file.
void ImageControlProcessor::setSelectedUserPreset(std::optional<UserPreset> preset)
{
    const std::scoped_lock stateLock(stateInformationMutex);
    const auto generation = history.generation();
    if (selectedPresetGeneration != generation) {
        selectedUserPresets = {};
        selectedPresetGeneration = generation;
    }
    selectedUserPresets.at(static_cast<std::size_t>(activeComparisonSlot)) = std::move(preset);
}

/// Returns the selected persistent comparison slot without touching the message-thread editor tree.
int ImageControlProcessor::getActiveComparisonSlot() const
{
    const std::scoped_lock stateLock(stateInformationMutex);
    return activeComparisonSlot;
}

/// Switches sounds as one local delta command, refreshing the outgoing slot from live automation first.
/// Metadata is committed before parameter notifications so reentrant host saves observe the new selection.
bool ImageControlProcessor::selectComparisonSlot(const int slot)
{
    const std::scoped_lock stateLock(stateInformationMutex);
    if (slot < 0 || slot > 1 || slot == activeComparisonSlot || applyingSoundCommand || history.isEditing()
        || history.isReplaying())
    {
        return false;
    }
    const juce::ScopedValueSetter<bool> guard(applyingSoundCommand, true);
    const auto generation = history.generation();
    const auto previous = activeComparisonSlot;
    comparisonSlots.at(static_cast<std::size_t>(previous)) = captureProcessingState(parameters);
    const auto target = comparisonSlots.at(static_cast<std::size_t>(slot));
    history.beginEdit(slot == 0 ? "Switch to A" : "Switch to B");
    history.setMetadataChange([this, previous, slot](const bool undo) {
        comparisonSlots.at(static_cast<std::size_t>(activeComparisonSlot)) = captureProcessingState(parameters);
        activeComparisonSlot = undo ? previous : slot;
    });
    activeComparisonSlot = slot;
    const auto applied = applySoundSnapshot(target, generation);
    history.endEdit();
    return applied && history.generation() == generation;
}

/// Copies the active live sound to the inactive slot without sending any parameter writes.
/// Undo patches only the destination slot, retaining subsequent unrelated sound or editor changes.
bool ImageControlProcessor::copyComparisonToOtherSlot()
{
    const std::scoped_lock stateLock(stateInformationMutex);
    if (applyingSoundCommand || history.isEditing() || history.isReplaying()) {
        return false;
    }
    const juce::ScopedValueSetter<bool> guard(applyingSoundCommand, true);
    const auto generation = history.generation();
    const auto source = captureProcessingState(parameters);
    comparisonSlots.at(static_cast<std::size_t>(activeComparisonSlot)) = source;
    const auto destination = static_cast<std::size_t>(1 - activeComparisonSlot);
    const auto previous = comparisonSlots.at(destination);
    if (sameProcessingState(previous, source)) {
        return false;
    }
    history.beginEdit(activeComparisonSlot == 0 ? "Copy A to B" : "Copy B to A");
    history.setMetadataChange([this, destination, previous, source](const bool undo) {
        comparisonSlots.at(destination) = undo ? previous : source;
    });
    comparisonSlots.at(destination) = source;
    history.endEdit();
    return history.generation() == generation;
}

/// Recalls a validated portable sound as one chronological history command, preserving audition and editor settings.
/// A complete finite in-range set is mandatory. Only changed parameters notify the host.
bool ImageControlProcessor::loadSoundSnapshot(const ProcessingSnapshot& snapshot, const juce::String& description)
{
    const std::scoped_lock stateLock(stateInformationMutex);
    if (applyingSoundCommand || history.isEditing() || history.isReplaying()) {
        return false;
    }
    const auto document = processingStateToXml(snapshot);
    if (document == nullptr) {
        return false;
    }
    const auto validated = processingStateFromXml(*document, parameters);
    if (!validated) {
        return false;
    }
    if (sameProcessingState(captureProcessingState(parameters), *validated)) {
        return true;
    }
    const juce::ScopedValueSetter<bool> guard(applyingSoundCommand, true);
    const auto generation = history.generation();
    history.beginEdit(description);
    const auto applied = applySoundSnapshot(*validated, generation);
    history.endEdit();
    return applied && history.generation() == generation;
}

/// Sends balanced gestures only for the target's changed IDs, recording each before its first local write.
/// Synchronous host restore aborts remaining writes so its new state cannot be overwritten by stale command work.
bool ImageControlProcessor::applySoundSnapshot(const ProcessingSnapshot& snapshot, const std::uint64_t generation)
{
    for (const auto& [parameterId, value] : snapshot) {
        if (history.generation() != generation) {
            return false;
        }
        auto* parameter = parameters.getParameter(parameterId);
        if (parameter == nullptr || juce::exactlyEqual(plainValue(*parameter), value)) {
            continue;
        }
        history.touch(parameterId);
        parameter->beginChangeGesture();
        if (history.generation() == generation) {
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        }
        parameter->endChangeGesture();
    }
    return history.generation() == generation;
}

/// Marks persistent A/B metadata dirty after a complete original, undo, or redo command, never from the audio thread.
void ImageControlProcessor::notifyComparisonChanged()
{
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true));
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
        crossovers.at(crossover) = crossoverValues.at(crossover).get().load();
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

/// Ignores renames because the single program keeps its fixed Default name.
void ImageControlProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

/// Saves the thread-safe parameter copy and detached editor snapshot, including unapplied restores.
/// Host save/restore calls serialize with each other, but never wait for message-thread dispatch.
/// Like APVTS copyState, this allocates and locks and is not suitable for the realtime audio callback.
void ImageControlProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    const std::scoped_lock stateLock(stateInformationMutex);
    const auto state = parameters.copyState();
    comparisonSlots.at(static_cast<std::size_t>(activeComparisonSlot)) = soundFromParameterTree(state, parameters);
    juce::MemoryBlock snapshot;
    {
        const std::scoped_lock editorLock(editorStateMutex);
        snapshot = editorStateSnapshot;
    }
    if (const auto xml = state.createXml()) {
        const auto editor = juce::ValueTree::readFromData(snapshot.getData(), snapshot.getSize());
        xml->addChildElement(editor.createXml().release());
        auto comparison = std::make_unique<juce::XmlElement>(COMPARISON_STATE_TAG);
        comparison->setAttribute("version", COMPARISON_STATE_VERSION);
        comparison->setAttribute("active", activeComparisonSlot);
        for (std::size_t index = 0; index < comparisonSlots.size(); ++index) {
            auto slot = std::make_unique<juce::XmlElement>("Slot");
            slot->setAttribute("id", static_cast<int>(index));
            slot->addChildElement(processingStateToXml(comparisonSlots.at(index)).release());
            comparison->addChildElement(slot.release());
        }
        xml->addChildElement(comparison.release());
        copyXmlToBinary(*xml, destinationData);
    }
}

/// Prepares the imager for the sample rate and channel count with the current crossovers,
/// and reports its latency to the host.
void ImageControlProcessor::prepareToPlay(const double sampleRate, const int maximumBlockSize)
{
    updateProcessingTargets(bypassValue.load() >= 0.5F);
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
    monoSmoother.setCurrentAndTargetValue(monoValue.load() >= 0.5F ? 1.0F : 0.0F);
    setLatencySamples(imager.getLatencySamples());
}

/// Processes a block with the current parameters, bypassed when the bypass parameter is on.
void ImageControlProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    processWithBypass(buffer, bypassValue.load() >= 0.5F);
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
/// Bypassing glides every gain and Width to unity, Recover off, every pan to the centre, and solo-muted bands back in,
/// instead of skipping the imager,
/// so the latency stays the same and the output becomes the delayed input bit for bit without a click.
/// Delta mode subtracts the delayed input from the output, and bypassing turns it off,
/// so bypass restores the delayed input before the final Mono fold.
/// Offline rendering designs moved crossovers synchronously, so its output does not depend on thread timing.
void ImageControlProcessor::processWithBypass(juce::AudioBuffer<float>& buffer, const bool bypassed)
{
    const juce::ScopedNoDenormals noDenormals;

    updateProcessingTargets(bypassed);
    Crossovers rawCrossovers {};
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        rawCrossovers[crossover] = crossoverValues[crossover].get().load();
    }
    imager.setCrossovers(rawCrossovers, isNonRealtime());
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

/// Updates changed raw controls and converted targets without touching filter history, kernels, or metering.
/// The cache belongs to the audio thread, or prepare/reset while callbacks are stopped, never parameter listeners.
/// Preparation and reset call this before settling the imager, including after host bypass or stopped parameter edits.
void ImageControlProcessor::updateProcessingTargets(const bool bypassed)
{
    ProcessingControlValues raw;
    raw.widths.fill(1.0F);
    if (!bypassed) {
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            raw.solos[band] = soloValues[band].get().load() >= 0.5F;
            raw.mid[band] = midGainValues[band].get().load();
            raw.side[band] = sideGainValues[band].get().load();
            raw.pans[band] = panValues[band].get().load();
            raw.widths[band] = widthValues[band].get().load();
            raw.recoveries[band] = recoverValues[band].get().load();
        }
        raw.output = outputGainValue.load();
        raw.delta = deltaValue.load() >= 0.5F;
    }
    bool gainsChanged = false;
    bool pansChanged = false;
    bool widthsChanged = false;
    bool recoveriesChanged = false;
    const auto unchangedScale = [](const float value) { return value; };
    const auto recoveryScale = [](const float percent) { return percent / RECOVER_MAX_PERCENT; };
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        gainsChanged |= updateCachedControl(
            raw.mid[band],
            cachedRawControls.mid[band],
            cachedProcessingTargets.mid[band],
            processingControlsValid,
            bandGainFromDecibels
        );
        gainsChanged |= updateCachedControl(
            raw.side[band],
            cachedRawControls.side[band],
            cachedProcessingTargets.side[band],
            processingControlsValid,
            bandGainFromDecibels
        );
        pansChanged |= updateCachedControl(
            raw.pans[band],
            cachedRawControls.pans[band],
            cachedProcessingTargets.pans[band],
            processingControlsValid,
            panFromPercent
        );
        widthsChanged |= updateCachedControl(
            raw.widths[band],
            cachedRawControls.widths[band],
            cachedProcessingTargets.widths[band],
            processingControlsValid,
            unchangedScale
        );
        recoveriesChanged |= updateCachedControl(
            raw.recoveries[band],
            cachedRawControls.recoveries[band],
            cachedProcessingTargets.recoveries[band],
            processingControlsValid,
            recoveryScale
        );
    }
    gainsChanged |= updateCachedControl(
        raw.output,
        cachedRawControls.output,
        cachedProcessingTargets.output,
        processingControlsValid,
        outputGainFromDecibels
    );
    if (gainsChanged) {
        imager.setGains(cachedProcessingTargets.mid, cachedProcessingTargets.side, cachedProcessingTargets.output);
    }
    if (pansChanged) {
        imager.setPans(cachedProcessingTargets.pans);
    }
    if (widthsChanged) {
        imager.setWidths(cachedProcessingTargets.widths);
    }
    if (recoveriesChanged) {
        imager.setRecoveries(cachedProcessingTargets.recoveries);
    }
    if (!processingControlsValid || raw.solos != cachedRawControls.solos) {
        cachedRawControls.solos = raw.solos;
        imager.setSolos(soloGains(raw.solos));
    }
    if (!processingControlsValid || raw.delta != cachedRawControls.delta) {
        cachedRawControls.delta = raw.delta;
        imager.setDelta(raw.delta);
    }
    processingControlsValid = true;
}

/// Folds the final processed output equally into both channels before Post analysis, with a click-free blend.
/// Mono follows Delta and bypass and applies identically to real-time and offline output.
void ImageControlProcessor::applyMono(juce::AudioBuffer<float>& buffer)
{
    monoSmoother.setTargetValue(monoValue.load() >= 0.5F ? 1.0F : 0.0F);
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
    updateProcessingTargets(bypassValue.load() >= 0.5F);
    imager.reset();
    analyzerFifo.beginEpoch();
    monoSmoother.setCurrentAndTargetValue(monoValue.load() >= 0.5F ? 1.0F : 0.0F);
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
    std::array<ProcessingSnapshot, 2> restoredSlots;
    int restoredActive = 0;
    const auto hasComparison = restoreComparisonState(*xml, parameters, restoredSlots, restoredActive);
    // Comparison metadata must not become APVTS children, or subsequent saves would duplicate stale metadata.
    while (auto* comparison = xml->getChildByName(COMPARISON_STATE_TAG)) {
        xml->removeChildElement(comparison, true);
    }
    const auto restoredParameters = juce::ValueTree::fromXml(*xml);
    {
        const std::scoped_lock stateLock(stateInformationMutex);
        sanitizeParameterState(restoredParameters, parameters);
        const auto restoredSound = soundFromParameterTree(restoredParameters, parameters);
        if (!hasComparison) {
            restoredSlots.fill(restoredSound);
            restoredActive = 0;
        }
        restoredSlots.at(static_cast<std::size_t>(restoredActive)) = restoredSound;
        comparisonSlots = std::move(restoredSlots);
        activeComparisonSlot = restoredActive;
        // Only atomic invalidation runs here. Message-thread use later reclaims local command storage.
        history.invalidate();
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
