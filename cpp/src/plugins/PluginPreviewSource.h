/// Device-independent playback through an already prepared live plugin chain.
/// PluginPreviewSource tracks audible time separately from decoded input,
/// discards compensation pre-roll, and drains delayed audio and bounded effect tails.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "plugins/PluginChainPreparation.h"
#include "utils/AudioFormatUtils.h"

#include <JuceHeader.h>

#include <vector>

namespace audiobatch
{
/// Processes device-rate input without owning the input source or plugin instances.
/// The host must serialize all access, prepare plugins on the message thread,
/// and keep the source and processors alive until they have been detached.
class PluginPreviewSource final : public juce::AudioSource
{
public:
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;
    void setInput(juce::AudioSource* source, double durationSeconds, int channels);
    void setChain(
        std::vector<juce::AudioPluginInstance*> processors,
        const plugin_processing::ChainPreparation& preparation,
        bool enabled,
        bool compensate
    );
    void setInputGain(LinearGain gain, bool immediately = false);
    void setTailSamples(juce::int64 samples);
    void setPosition(double seconds);
    void start();
    void stop();
    void requestStop();

    [[nodiscard]] bool isPlaying() const noexcept;
    [[nodiscard]] bool isStopping() const noexcept;
    [[nodiscard]] bool hasFailed() const noexcept;
    [[nodiscard]] juce::String describeFailure() const;
    [[nodiscard]] double getCurrentPosition() const noexcept;

private:
    int processInputBlock(int samples, juce::int64 sourceRemaining);

    void allocateBuffers();
    void copyOutput(const juce::AudioSourceChannelInfo& info, int offset, int samples, int signalChannels);
    void resetPlayback();

    /// View of the scratch buffer with the channels and the length of the current block.
    juce::AudioBuffer<float> blockView;
    /// Buffer the input is read into and the chain processes, as wide as the widest plugin.
    juce::AudioBuffer<float> scratch;
    /// Non-owning source of the device-rate input, or null while no file is attached.
    /// The host keeps it alive until setInput replaces or detaches it.
    juce::AudioSource* input {nullptr};
    /// Empty MIDI buffer that every plugin gets, allocated before rendering.
    juce::MidiBuffer midi;
    /// The custom gain of the file, ramped while the chain is on.
    juce::SmoothedValue<float> inputGain {1.0f};
    /// Samples copied to the output since the start or the last seek.
    juce::int64 emittedSamples {0};
    /// Summed latency of the chain while it is on, zero otherwise.
    juce::int64 latencySamples {0};
    /// Length of the file at the processing rate.
    juce::int64 lengthSamples {0};
    /// Samples of the latency pre-roll still to render silently after a start or a seek.
    juce::int64 preRollRemaining {0};
    /// Samples rendered through the chain since the start or the last seek, including the pre-roll and the tail.
    juce::int64 processedSamples {0};
    /// Position at the processing rate that the start or the last seek began rendering from.
    juce::int64 startSample {0};
    /// Summed bounded tail of the chain while it is on, zero otherwise.
    juce::int64 tailSamples {0};
    /// Views of the scratch buffer with the channel count of each plugin, in chain order.
    std::vector<juce::AudioBuffer<float>> pluginViews;
    /// Non-owning, never null processors of the attached chain in chain order.
    /// The host keeps them alive and prepared until setChain replaces or detaches them.
    std::vector<juce::AudioPluginInstance*> plugins;

    /// True to render the latency pre-roll, so the audio lines up with the position.
    bool compensation {true};
    /// True after a plugin failed or produced non-finite audio, until the next start or seek.
    bool failed {false};
    /// True while rendering, including the pre-roll, the tail, and a stop fade.
    bool playing {false};
    /// True while a requested stop fades out.
    bool stopRequested {false};
    /// True while the audio runs through the chain.
    bool wet {false};

    /// Length of the file in seconds.
    double duration {0.0};
    /// Audible position in the file in seconds, held at the end while the tail drains.
    double position {0.0};
    /// Sample rate of the device that the input and the chain run at.
    double processingRate {utils::defaultSampleRate};

    /// Most samples rendered through the chain at a time.
    int blockSize {512};
    /// Samples of the stop fade still to render.
    int fadeOutRemaining {0};
    /// Index of the plugin whose process call failed in the last block, or -1 when none did.
    int failedPluginIndex {-1};
    /// Channels of the scratch buffer, the widest bus of the chain and at least two.
    int processingChannels {2};
    /// Channels of the file, one or two.
    int sourceChannels {2};
    /// Samples of the start fade rendered so far.
    int transitionSamples {0};
};
}  // namespace audiobatch
