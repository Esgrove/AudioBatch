/// Declares ImageControlProcessor, the audio processor shared by ImageControl's plugin formats.
/// It owns the parameters, the multiband imager that processes the audio,
/// the ring that carries audio to the editor's spatial spectrogram,
/// and the editor-only settings saved with the plugin state.

#pragma once

#include "analysis/StereoSampleFifo.h"
#include "dsp/MultibandImager.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "UserPresets.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace esgrove::image_control
{
/// Independent correlation readings associated with one processor monitoring lifecycle.
struct BandCorrelationSnapshot final {
    /// Latest readings, with NaNs for unavailable bands.
    BandCorrelations readings {};
    /// Changes on monitoring visibility, prepare, reset, and release, even between display refreshes.
    std::uint32_t generation {0};
};

/// Last effective raw controls and their converted targets, owned exclusively by the processing lifecycle thread.
/// Bypass supplies neutral values here without modifying host parameters.
struct ProcessingControlValues final {
    /// Mid values, in dB for raw controls and linear amplitude for converted targets.
    BandGains mid {};
    /// Side values, in dB for raw controls and linear amplitude for converted targets.
    BandGains side {};
    /// Pan values, in percent for raw controls and signed unit amounts for converted targets.
    BandPans pans {};
    /// Independent Width multipliers.
    BandWidths widths {};
    /// Recover values, in percent for raw controls and unit amounts for converted targets.
    BandRecoveries recoveries {};
    /// Solo switches before their combined audible-band conversion.
    BandSolos solos {};
    /// Output gain, in dB for raw controls and linear amplitude for converted targets.
    float output {0.0F};
    /// Effective Delta switch.
    bool delta {false};
};

/// One detached active-slot presentation, captured under the processor's host-state mutex without disk access.
struct PresetPresentation final {
    /// Current sound, including headless automation.
    ProcessingSnapshot sound;
    /// Persistent identity, name, and baseline, even when the library entry is unavailable.
    std::optional<PresetProvenance> provenance;
    /// Exact library revision, present only after an explicit successful preset operation or reconnection.
    std::optional<UserPreset> selection;
    /// Restore generation that asynchronous commands must still match when they complete.
    std::uint64_t generation {0};
    /// Active comparison slot paired with all other fields.
    int comparisonSlot {0};
    /// Whether the sound differs exactly from its retained effective recall reference, with a legacy mapping fallback.
    bool modified {false};
};

/// The ImageControl audio processor.
/// The audio thread reads parameters each block, reuses unchanged conversions, and updates MultibandImager targets.
/// The analyzer tap pushes either the input or the output to the editor's ring.
/// The state is the XML of the parameter tree with the editor settings as a child element.
class ImageControlProcessor final :
    public juce::AudioProcessor,
    private juce::ValueTree::Listener,
    private juce::AsyncUpdater
{
public:
    ImageControlProcessor();

    ~ImageControlProcessor() override;

    ImageControlProcessor(const ImageControlProcessor&) = delete;
    ImageControlProcessor(ImageControlProcessor&&) = delete;
    ImageControlProcessor& operator=(const ImageControlProcessor&) = delete;
    ImageControlProcessor& operator=(ImageControlProcessor&&) = delete;

    [[nodiscard]] juce::AudioProcessorValueTreeState& getParameters();
    [[nodiscard]] juce::ValueTree& getEditorState();
    [[nodiscard]] StereoSampleFifo& getAnalyzerFifo();

    [[nodiscard]] ParameterHistory& getHistory();
    [[nodiscard]] ProcessingSnapshot getSoundSnapshot();
    [[nodiscard]] PresetPresentation getPresetPresentation();
    [[nodiscard]] std::optional<PresetProvenance> getPresetProvenance() const;
    [[nodiscard]] std::optional<UserPreset> getSelectedUserPreset() const;
    [[nodiscard]] int getActiveComparisonSlot() const;
    [[nodiscard]] bool copyComparisonToOtherSlot();
    [[nodiscard]] bool loadSoundSnapshot(const ProcessingSnapshot& snapshot, const juce::String& description);
    [[nodiscard]] bool loadUserPreset(const UserPreset& preset, std::uint64_t generation, int comparisonSlot);
    [[nodiscard]] bool reconnectSelectedUserPreset(
        std::optional<UserPreset> preset,
        std::uint64_t generation,
        int comparisonSlot
    );
    [[nodiscard]] bool renameSelectedUserPreset(const UserPreset& preset, std::uint64_t generation, int comparisonSlot);
    [[nodiscard]] bool selectComparisonSlot(int slot);
    [[nodiscard]] bool setSelectedUserPreset(
        std::optional<UserPreset> preset,
        std::uint64_t generation,
        int comparisonSlot
    );
    void setSelectedUserPreset(std::optional<UserPreset> preset);

    [[nodiscard]] BandCorrelationSnapshot getBandCorrelationSnapshot() const noexcept;
    [[nodiscard]] BandCorrelations getBandCorrelations() const noexcept;
    [[nodiscard]] Crossovers getCrossovers() const;

    void setCorrelationEnabled(bool enabled) noexcept;

    [[nodiscard]] const juce::String getName() const override;
    [[nodiscard]] const juce::String getProgramName(int index) override;
    [[nodiscard]] juce::AudioProcessorParameter* getBypassParameter() const override;

    [[nodiscard]] bool acceptsMidi() const override;
    [[nodiscard]] bool hasEditor() const override;
    [[nodiscard]] bool isMidiEffect() const override;
    [[nodiscard]] bool producesMidi() const override;

    [[nodiscard]] double getTailLengthSeconds() const override;

    [[nodiscard]] int getCurrentProgram() override;
    [[nodiscard]] int getNumPrograms() override;

    void changeProgramName(int index, const juce::String& newName) override;
    void getStateInformation(juce::MemoryBlock& destinationData) override;
    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void releaseResources() override;
    void reset() override;
    void setCurrentProgram(int index) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

protected:
    [[nodiscard]] bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

private:
    [[nodiscard]] juce::AudioProcessorEditor* createEditor() override;

    void handleAsyncUpdate() override;
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;

    void applyPendingEditorState();

    [[nodiscard]] bool applySoundSnapshot(const ProcessingSnapshot& snapshot, std::uint64_t generation);
    void notifyComparisonChanged();

    void applyMono(juce::AudioBuffer<float>& buffer);
    void processWithBypass(juce::AudioBuffer<float>& buffer, bool bypassed);
    void pushToAnalyzer(const juce::AudioBuffer<float>& buffer, AnalyzerEpoch epoch);
    void updateProcessingTargets(bool bypassed);

    /// Owns the parameters that the raw value references below bind to.
    juce::AudioProcessorValueTreeState parameters;
    /// Message-thread-only settings, kept in one tree so the editor's bindings survive state restores.
    juce::ValueTree editorState;
    /// Detached latest requested parameter tree, retained to finish a reentrant restore authoritatively.
    /// Protected by stateInformationMutex and never shared with the APVTS-owned tree.
    juce::ValueTree restoredParameterRequest;
    /// Detached accepted raw readback of the complete restore, published before live parameter notifications.
    /// Kept separate from the request so saving cannot add another skew conversion to the applied parameter values.
    juce::ValueTree restoredParameterTarget;
    /// Detached binary editor settings, copied under editorStateMutex and never shared as a live tree.
    juce::MemoryBlock editorStateSnapshot;
    /// Protects the snapshot, pending flag, and publication of the corresponding analyzer tap.
    std::mutex editorStateMutex;
    /// Serializes host save/restore and local history commands, never the audio callback or live editor tree.
    /// Recursive because parameter listeners can synchronously request a save during a restore or local command.
    mutable std::recursive_mutex stateInformationMutex;
    /// Explicit local history outlives editors and never observes incoming parameter automation.
    ParameterHistory history;
    /// Persistent sound slots, protected by stateInformationMutex. The selected slot is refreshed from live parameters.
    std::array<ProcessingSnapshot, 2> comparisonSlots;
    /// Selected comparison slot. Zero is A and one is B, protected by stateInformationMutex.
    int activeComparisonSlot {0};
    /// Persistent per-slot identity and recall baseline, independent of library availability and history replay.
    PresetProvenanceSlots presetProvenance;
    /// Transient exact file revisions, cleared on every accepted host restore and never serialized.
    std::array<std::optional<UserPreset>, 2> selectedUserPresets;
    /// Prevents reentrant toolbar commands while a local command is notifying parameter listeners.
    bool applyingSoundCommand {false};
    /// Prevents nested APVTS connection passes. Reentrant restores publish their target for the outer pass to apply.
    bool applyingParameterRestore {false};
    /// Whether the message-thread tree needs the latest snapshot. Protected by editorStateMutex.
    bool editorStatePending {false};

    /// The audio processing.
    MultibandImager imager;
    /// Carries audio from the audio thread to the editor's analyzer.
    StereoSampleFifo analyzerFifo;

    /// Raw crossover values, owned by the parameter tree and valid for this processor's lifetime.
    std::array<std::reference_wrapper<std::atomic<float>>, CROSSOVER_COUNT> crossoverValues;
    /// Raw Mid gain values, owned by the parameter tree and valid for this processor's lifetime.
    std::array<std::reference_wrapper<std::atomic<float>>, BAND_COUNT> midGainValues;
    /// Raw pan values, owned by the parameter tree and valid for this processor's lifetime.
    std::array<std::reference_wrapper<std::atomic<float>>, BAND_COUNT> panValues;
    /// Raw recovery percentages, owned by the parameter tree and valid for this processor's lifetime.
    std::array<std::reference_wrapper<std::atomic<float>>, BAND_COUNT> recoverValues;
    /// Raw Side gain values, owned by the parameter tree and valid for this processor's lifetime.
    std::array<std::reference_wrapper<std::atomic<float>>, BAND_COUNT> sideGainValues;
    /// Raw Solo values, owned by the parameter tree and valid for this processor's lifetime.
    std::array<std::reference_wrapper<std::atomic<float>>, BAND_COUNT> soloValues;
    /// Raw Width values, owned by the parameter tree and valid for this processor's lifetime.
    std::array<std::reference_wrapper<std::atomic<float>>, BAND_COUNT> widthValues;
    /// Raw output gain, owned by the parameter tree and valid for this processor's lifetime.
    std::atomic<float>& outputGainValue;
    /// Raw bypass value, owned by the parameter tree and valid for this processor's lifetime.
    std::atomic<float>& bypassValue;
    /// Raw Delta value, owned by the parameter tree and valid for this processor's lifetime.
    std::atomic<float>& deltaValue;

    /// Raw Mono value, owned by the parameter tree and valid for this processor's lifetime.
    std::atomic<float>& monoValue;

    /// Effective raw values seen by the last callback, prepare, or reset. Never changed by parameter listeners.
    ProcessingControlValues cachedRawControls;
    /// Converted targets reused when their exact raw values are unchanged.
    ProcessingControlValues cachedProcessingTargets;
    /// Whether the processing lifecycle has initialized every cached conversion and imager target.
    bool processingControlsValid {false};

    /// Audio-thread-owned click-free fold of the final output before Post analysis.
    SmoothedGain monoSmoother;
    /// Whether an editor requests correlation measurements. Never serialized.
    std::atomic<bool> correlationEnabled {false};
    /// Last metered callback time. Zero means no reading has been published for this view generation.
    std::atomic<std::uint32_t> correlationUpdateMilliseconds {0};
    /// Monitor generation rejects a callback finishing after its requesting view was hidden or closed.
    std::atomic<std::uint32_t> correlationGeneration {1};
    /// Generation of the most recently published processor meter readings.
    std::atomic<std::uint32_t> publishedCorrelationGeneration {0};
    /// Last view generation applied to the imager, accessed only by the processing thread.
    std::uint32_t activeCorrelationGeneration {0};
    /// Expiry is computed from the prepared host block size before callbacks can publish readings.
    std::atomic<std::uint32_t> correlationExpiryMilliseconds {0};
    /// Message-thread-only guard suppressing snapshot, analyzer-tap, and dirty updates during tree restores.
    bool restoringState {false};
};
}  // namespace esgrove::image_control
