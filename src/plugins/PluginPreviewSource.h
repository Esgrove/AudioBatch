/// Device-independent playback through an already prepared live plugin chain.
/// PluginPreviewSource tracks audible time separately from decoded input,
/// discards compensation pre-roll, and drains delayed audio and bounded effect tails.

#pragma once

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
    void setInputGain(float linearGain, bool immediately = false);
    void setTailSamples(juce::int64 samples);
    void setPosition(double seconds);
    void start();
    void stop();
    void requestStop();

    [[nodiscard]] bool isPlaying() const noexcept;
    [[nodiscard]] bool isStopping() const noexcept;
    [[nodiscard]] bool hasFailed() const noexcept;
    [[nodiscard]] double getCurrentPosition() const noexcept;

private:
    int processInputBlock(int samples, juce::int64 sourceRemaining);

    void allocateBuffers();
    void copyOutput(const juce::AudioSourceChannelInfo& info, int offset, int samples, int signalChannels);
    void resetPlayback();

    juce::AudioBuffer<float> blockView;
    juce::AudioBuffer<float> scratch;
    juce::AudioSource* input {nullptr};
    juce::MidiBuffer midi;
    juce::SmoothedValue<float> inputGain {1.0f};
    juce::int64 emittedSamples {0};
    juce::int64 latencySamples {0};
    juce::int64 lengthSamples {0};
    juce::int64 preRollRemaining {0};
    juce::int64 processedSamples {0};
    juce::int64 startSample {0};
    juce::int64 tailSamples {0};
    std::vector<juce::AudioBuffer<float>> pluginViews;
    std::vector<juce::AudioPluginInstance*> plugins;

    bool compensation {true};
    bool failed {false};
    bool playing {false};
    bool stopRequested {false};
    bool wet {false};

    double duration {0.0};
    double position {0.0};
    double processingRate {utils::defaultSampleRate};

    int blockSize {512};
    int fadeOutRemaining {0};
    int processingChannels {2};
    int sourceChannels {2};
    int transitionSamples {0};
};
}  // namespace audiobatch
