/// Device-independent playback through an already prepared live plugin chain.
/// PluginPreviewSource tracks audible time separately from decoded input,
/// discards compensation pre-roll, and drains delayed audio and bounded effect tails.

#pragma once

#include "plugins/PluginChainPreparation.h"

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
    [[nodiscard]] bool hasFailed() const noexcept;
    [[nodiscard]] double getCurrentPosition() const noexcept;

private:
    void allocateBuffers();
    void resetPlayback();
    int processInputBlock(int samples, juce::int64 sourceRemaining);
    void copyOutput(const juce::AudioSourceChannelInfo& info, int offset, int samples, int signalChannels);

    juce::AudioSource* input = nullptr;
    std::vector<juce::AudioPluginInstance*> plugins;
    std::vector<juce::AudioBuffer<float>> pluginViews;
    juce::AudioBuffer<float> scratch;
    juce::AudioBuffer<float> blockView;
    juce::MidiBuffer midi;
    juce::SmoothedValue<float> inputGain {1.0f};
    double processingRate = 48000.0;
    double duration = 0.0;
    double position = 0.0;
    int blockSize = 512;
    int sourceChannels = 2;
    int processingChannels = 2;
    int transitionSamples = 0;
    int fadeOutRemaining = 0;
    juce::int64 lengthSamples = 0;
    juce::int64 startSample = 0;
    juce::int64 processedSamples = 0;
    juce::int64 emittedSamples = 0;
    juce::int64 latencySamples = 0;
    juce::int64 tailSamples = 0;
    juce::int64 preRollRemaining = 0;
    bool wet = false;
    bool compensation = true;
    bool playing = false;
    bool failed = false;
    bool stopRequested = false;
};
}  // namespace audiobatch
