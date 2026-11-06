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

namespace esgrove::image_control
{
/// The ImageControl audio processor.
/// The audio thread reads the parameters each block and hands them to the MultibandImager.
/// The analyzer tap pushes either the input or the output to the editor's ring.
/// The state is the XML of the parameter tree with the editor settings as a child element.
class ImageControlProcessor final : public juce::AudioProcessor, private juce::ValueTree::Listener
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

    [[nodiscard]] Crossovers getCrossovers() const;

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
    void setCurrentProgram(int index) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

protected:
    [[nodiscard]] bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

private:
    [[nodiscard]] juce::AudioProcessorEditor* createEditor() override;

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;

    void processWithBypass(juce::AudioBuffer<float>& buffer, bool bypassed);
    void pushToAnalyzer(const juce::AudioBuffer<float>& buffer);

    /// Owns the parameters that the raw value pointers below point into.
    juce::AudioProcessorValueTreeState parameters;
    /// Editor-only settings, kept in one tree so the editor's bindings survive state restores.
    juce::ValueTree editorState;

    MultibandImager imager;
    StereoSampleFifo analyzerFifo;

    std::array<std::atomic<float>*, CROSSOVER_COUNT> crossoverValues {};
    std::array<std::atomic<float>*, BAND_COUNT> midGainValues {};
    std::array<std::atomic<float>*, BAND_COUNT> sideGainValues {};
    /// Raw value of the output gain parameter, owned by the parameter tree.
    std::atomic<float>* outputGainValue {nullptr};
    /// Raw value of the bypass parameter, owned by the parameter tree.
    std::atomic<float>* bypassValue {nullptr};

    /// Whether the analyzer tap is the output rather than the input, mirrored from the editor state.
    std::atomic<bool> analyzeOutput {true};
};
}  // namespace esgrove::image_control
