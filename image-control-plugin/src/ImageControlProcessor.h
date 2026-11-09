/// Declares ImageControlProcessor, the audio processor of the ImageControl VST3 plugin.
/// It owns the parameters, the multiband imager that processes the audio,
/// the ring that carries audio to the editor's spatial spectrogram,
/// and the editor-only settings saved with the plugin state.

#pragma once

#include "analysis/StereoSampleFifo.h"
#include "dsp/MultibandImager.h"
#include "Parameters.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace esgrove::image_control
{
/// Independent correlation readings associated with one processor monitoring lifecycle.
struct BandCorrelationSnapshot final {
    /// Latest readings, with NaNs for unavailable bands.
    BandCorrelations readings {};
    /// Changes on monitoring visibility, prepare, reset, and release, even between display refreshes.
    std::uint32_t generation {0};
};

/// The ImageControl audio processor.
/// The audio thread reads the parameters each block and hands them to the MultibandImager.
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

    void applyMono(juce::AudioBuffer<float>& buffer);
    void processWithBypass(juce::AudioBuffer<float>& buffer, bool bypassed);
    void pushToAnalyzer(const juce::AudioBuffer<float>& buffer, AnalyzerEpoch epoch);

    /// Owns the parameters that the raw value pointers below point into.
    juce::AudioProcessorValueTreeState parameters;
    /// Message-thread-only settings, kept in one tree so the editor's bindings survive state restores.
    juce::ValueTree editorState;
    /// Detached binary editor settings, copied under editorStateMutex and never shared as a live tree.
    juce::MemoryBlock editorStateSnapshot;
    /// Protects the snapshot, pending flag, and publication of the corresponding analyzer tap.
    std::mutex editorStateMutex;
    /// Serializes host save/restore callbacks, without being acquired by the editor or audio callback.
    /// Recursive because parameter listeners can synchronously request a save during a restore.
    std::recursive_mutex stateInformationMutex;
    /// Whether the message-thread tree needs the latest snapshot. Protected by editorStateMutex.
    bool editorStatePending {false};

    /// The audio processing.
    MultibandImager imager;
    /// Carries audio from the audio thread to the editor's analyzer.
    StereoSampleFifo analyzerFifo;

    /// Raw values of the crossover parameters, owned by the parameter tree and never null after construction.
    std::array<std::atomic<float>*, CROSSOVER_COUNT> crossoverValues {};
    /// Raw values of the mid gain parameters, owned by the parameter tree and never null after construction.
    std::array<std::atomic<float>*, BAND_COUNT> midGainValues {};
    /// Raw values of the pan parameters, owned by the parameter tree and never null after construction.
    std::array<std::atomic<float>*, BAND_COUNT> panValues {};
    /// Raw values of the side gain parameters, owned by the parameter tree and never null after construction.
    std::array<std::atomic<float>*, BAND_COUNT> sideGainValues {};
    /// Raw values of the solo parameters, owned by the parameter tree and never null after construction.
    std::array<std::atomic<float>*, BAND_COUNT> soloValues {};
    /// Raw value of the output gain parameter, owned by the parameter tree.
    std::atomic<float>* outputGainValue {nullptr};
    /// Raw value of the bypass parameter, owned by the parameter tree.
    std::atomic<float>* bypassValue {nullptr};
    /// Raw value of the delta parameter, owned by the parameter tree.
    std::atomic<float>* deltaValue {nullptr};

    /// Raw value of the mono parameter, owned by the parameter tree.
    std::atomic<float>* monoValue {nullptr};

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
