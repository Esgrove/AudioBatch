/// Declares MidSideTestProcessor, the audio processor of the AudioBatch Test MS plugin.
/// The C++ and the Rust test suites both host this VST3 effect,
/// and compare what it renders against the expected files in the root fixtures directory.
/// Its processing is plain float arithmetic, so the expected output can be computed exactly outside any host.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace audiobatch::test_plugin
{
/// Index and VST3 id of the linear gain parameter, which spans 0 to 2.
constexpr int gainParameterIndex {0};
/// Index and VST3 id of the stereo width parameter, which spans 0 to 2.
constexpr int widthParameterIndex {1};
/// Upper end of both parameter ranges, which start from zero.
constexpr float parameterMaximum {2.0F};
/// Default of both parameters, which leaves the signal unchanged.
constexpr float parameterDefault {1.0F};

/// A mid/side width and gain effect without an editor, latency, or tail.
/// A stereo input is split into mid and side, the side is scaled by the width,
/// and both output channels are scaled by the gain.
/// A mono input is only scaled by the gain.
class MidSideTestProcessor final : public juce::AudioProcessor
{
public:
    MidSideTestProcessor();

    [[nodiscard]] const juce::String getName() const override;
    [[nodiscard]] bool acceptsMidi() const override;
    [[nodiscard]] bool producesMidi() const override;
    [[nodiscard]] bool isMidiEffect() const override;
    [[nodiscard]] double getTailLengthSeconds() const override;

    [[nodiscard]] int getNumPrograms() override;
    [[nodiscard]] int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    [[nodiscard]] const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    [[nodiscard]] bool hasEditor() const override;
    [[nodiscard]] juce::AudioProcessorEditor* createEditor() override;

    [[nodiscard]] bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    void getStateInformation(juce::MemoryBlock& destinationData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

private:
    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>& gain;
    std::atomic<float>& width;
};
}  // namespace audiobatch::test_plugin
