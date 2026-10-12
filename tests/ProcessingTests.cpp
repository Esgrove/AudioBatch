/// Tests for PluginProcessingService::processFile with small in-process test plugins.
/// Covers mono files through stereo-only and mixed-width chains, the clip guard, the output bit depth,
/// and the output length.
/// Replaced files are deleted instead of moved to the system trash, so the tests never touch the user's trash.

#include "AudioAnalysisService.h"
#include "PluginProcessingService.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <cstring>
#include <limits>
#include <vector>

namespace audiobatch::tests
{
/// Behaviour of a TestPlugin.
struct TestPluginSettings {
    /// Linear gain applied to every main channel.
    float gain = 1.0f;
    /// Extra linear gain applied to the right channel only, after the channel swap.
    float rightGain = 1.0f;
    /// Swaps the left and right channels before the gain.
    bool swapChannels = false;
    /// Accepts a mono main bus in addition to stereo.
    bool acceptsMono = false;
    /// Replaces the first sample of every block with infinity, like an unstable filter.
    bool producesInfinity = false;
    /// Multiplies the gain by this factor after every block, like a plugin that adapts to its input.
    /// The current gain is the plugin state, so restoring the state undoes the adaptation.
    float adaptiveGainStep = 1.0f;
};

/// A test plugin that applies a fixed gain and can swap the left and right channels.
/// It is stereo-only by default, like many effect plugins, and can also accept a mono layout.
/// Swapping exposes whether a mono source reaches the right input of a stereo-only plugin.
class TestPlugin final : public juce::AudioPluginInstance
{
public:
    /// Creates the plugin with the given behaviour.
    explicit TestPlugin(const TestPluginSettings& pluginSettings) :
        juce::AudioPluginInstance(
            BusesProperties()
                .withInput("Input", juce::AudioChannelSet::stereo())
                .withOutput("Output", juce::AudioChannelSet::stereo())
        ),
        settings(pluginSettings),
        currentGain(pluginSettings.gain)
    { }

    /// Applies the gain and the optional channel swap to the main bus channels only,
    /// like a hosted plugin, which never touches buffer channels outside its own buses.
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/) override
    {
        widestBuffer = juce::jmax(widestBuffer, buffer.getNumChannels());
        const auto channels = juce::jmin(buffer.getNumChannels(), getMainBusNumOutputChannels());

        if (settings.swapChannels && channels >= 2) {
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
                const auto left = buffer.getSample(0, sample);
                buffer.setSample(0, sample, buffer.getSample(1, sample));
                buffer.setSample(1, sample, left);
            }
        }

        for (int channel = 0; channel < channels; ++channel) {
            buffer.applyGain(channel, 0, buffer.getNumSamples(), currentGain);
        }

        currentGain *= settings.adaptiveGainStep;

        if (channels >= 2) {
            buffer.applyGain(1, 0, buffer.getNumSamples(), settings.rightGain);
        }

        if (settings.producesInfinity && channels > 0) {
            buffer.setSample(0, 0, std::numeric_limits<float>::infinity());
        }
    }

    /// Returns the largest channel count of any buffer passed to processBlock.
    [[nodiscard]] int getWidestBufferChannels() const
    {
        return widestBuffer;
    }

    /// Describes the plugin for the host.
    void fillInPluginDescription(juce::PluginDescription& description) const override
    {
        description.name = getName();
        description.pluginFormatName = "Test";
        description.numInputChannels = 2;
        description.numOutputChannels = 2;
    }

    /// Returns the plugin name.
    const juce::String getName() const override
    {
        return "Test Plugin";
    }

    /// Needs no preparation.
    void prepareToPlay(double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override { }

    /// Holds no resources.
    void releaseResources() override { }

    /// Has no tail.
    double getTailLengthSeconds() const override
    {
        return 0.0;
    }

    /// Takes no MIDI input.
    bool acceptsMidi() const override
    {
        return false;
    }

    /// Produces no MIDI output.
    bool producesMidi() const override
    {
        return false;
    }

    /// Has no editor.
    bool hasEditor() const override
    {
        return false;
    }

    /// Has a single program.
    int getNumPrograms() override
    {
        return 1;
    }

    /// Always uses the single program.
    int getCurrentProgram() override
    {
        return 0;
    }

    /// Ignores program changes.
    void setCurrentProgram(int /*index*/) override { }

    /// Returns no program name.
    const juce::String getProgramName(int /*index*/) override
    {
        return {};
    }

    /// Ignores program renames.
    void changeProgramName(int /*index*/, const juce::String& /*newName*/) override { }

    /// Saves the current gain as the plugin state.
    void getStateInformation(juce::MemoryBlock& destData) override
    {
        destData.replaceAll(&currentGain, sizeof(currentGain));
    }

    /// Restores the current gain from a state saved by getStateInformation.
    void setStateInformation(const void* data, const int sizeInBytes) override
    {
        if (sizeInBytes == static_cast<int>(sizeof(currentGain))) {
            std::memcpy(&currentGain, data, sizeof(currentGain));
        }
    }

protected:
    /// Accepts only a stereo main bus on both sides, like many effect plugins.
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto input = layouts.getMainInputChannelSet();
        const auto output = layouts.getMainOutputChannelSet();

        if (settings.acceptsMono && input == juce::AudioChannelSet::mono() && output == juce::AudioChannelSet::mono()) {
            return true;
        }

        return input == juce::AudioChannelSet::stereo() && output == juce::AudioChannelSet::stereo();
    }

private:
    /// Has no editor.
    juce::AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }

    TestPluginSettings settings;
    float currentGain;
    int widestBuffer = 0;
};

/// Checks plugin processing output against the committed fixtures.
class ProcessingTests final : public juce::UnitTest
{
public:
    ProcessingTests() : juce::UnitTest("Plugin processing", "AudioBatch") { }

    /// Runs the processing checks.
    void runTest() override
    {
        const TemporaryDirectory directory;

        beginTest("A mono file feeds both inputs of a stereo-only plugin and stays mono");
        {
            const auto source = directory.copyFixture("sine_mono_48000_24.aif");
            TestPlugin plugin({.swapChannels = true});
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expectEquals(result.analysisRecord.channels, 1);
            expectEquals(result.analysisRecord.bitsPerSample, 24);
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), fixturePeakDecibels, 0.05);
            expect(result.notice.isEmpty(), result.notice);
        }

        beginTest("A mono plugin followed by a stereo-only plugin passes the processed signal to both channels");
        {
            const auto source = directory.copyFixture("sine_mono_48000_24.aif", "mono_chain.aif");
            TestPlugin monoGain({.gain = juce::Decibels::decibelsToGain(6.0f), .acceptsMono = true});
            TestPlugin stereoSwap({.swapChannels = true});
            const auto result = process(source, {&monoGain, &stereoSwap});

            expect(!result.hasError(), result.errorMessage);
            expectEquals(result.analysisRecord.channels, 1);
            // Both stereo channels carry the boosted signal, so their average keeps the full 6 dB boost.
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), fixturePeakDecibels + 6.0, 0.05);
            // The mono plugin only sees its own channel, although the chain buffer is stereo.
            expectEquals(monoGain.getWidestBufferChannels(), 1);
            expectEquals(stereoSwap.getWidestBufferChannels(), 2);
        }

        beginTest("A stereo plugin followed by a mono plugin passes the average of both channels on");
        {
            const auto source = directory.copyFixture("sine_mono_48000_24.aif", "stereo_then_mono.aif");
            // The stereo plugin silences its right channel, so a dropped channel would keep the full level,
            // while the average of both channels is 6 dB lower.
            TestPlugin leftOnly({.rightGain = 0.0f});
            TestPlugin monoPassThrough({.acceptsMono = true});
            const auto result = process(source, {&leftOnly, &monoPassThrough});

            expect(!result.hasError(), result.errorMessage);
            expectEquals(result.analysisRecord.channels, 1);
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), fixturePeakDecibels - 6.0206, 0.05);
        }

        beginTest("A plugin that produces infinity fails the file and leaves the source unchanged");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "unstable.aif");
            const auto sourceSize = source.getSize();
            const auto sourceModified = source.getLastModificationTime();
            TestPlugin plugin({.producesInfinity = true});
            const auto result = process(source, {&plugin});

            expect(result.hasError());
            expect(!result.committed);
            expect(result.errorMessage.contains("invalid samples"), result.errorMessage);
            expectEquals(source.getSize(), sourceSize);
            expect(source.getLastModificationTime() == sourceModified);
        }

        beginTest("The saved plugin state is restored before every file");
        {
            const auto first = directory.copyFixture("sine_stereo_44100_16.aif", "first.aif");
            const auto second = directory.copyFixture("sine_stereo_44100_16.aif", "second.aif");
            TestPlugin plugin({.adaptiveGainStep = 0.99f});
            juce::MemoryBlock initialState;
            plugin.getStateInformation(initialState);

            const auto firstResult = process(first, {&plugin}, {initialState});
            const auto secondResult = process(second, {&plugin}, {initialState});

            expect(!firstResult.hasError(), firstResult.errorMessage);
            expect(!secondResult.hasError(), secondResult.errorMessage);
            // Without the restore, the second file would start from the gain the first file adapted to.
            expectWithinAbsoluteError(
                peakDecibels(secondResult.analysisRecord), peakDecibels(firstResult.analysisRecord), 0.01
            );
        }

        beginTest("An .aif source rewritten in place disposes of a copy of the original first");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "in_place.aif");
            const auto sourceSize = source.getSize();
            TestPlugin plugin({.gain = 0.5f});
            auto options = makeOptions({&plugin});
            juce::StringArray disposedNames;
            juce::Array<juce::int64> disposedSizes;
            options.disposeFile = [&](const juce::File& file) {
                disposedNames.add(file.getFileName());
                disposedSizes.add(file.getSize());
                return file.deleteFile();
            };

            const auto result
                = PluginProcessingService::processFile(AudioAnalysisService::analyzeFile(source), options, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expectEquals(disposedNames.joinIntoString(","), juce::String("in_place.aif"));
            expectEquals(disposedSizes[0], sourceSize);
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), fixturePeakDecibels - 6.0206, 0.05);
        }

        beginTest("A converted source is disposed of once its .aif output is in place");
        {
            const auto source = directory.copyFixture("sine_stereo_48000_24.wav", "converted.wav");
            TestPlugin plugin({});
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expect(result.committed);
            expect(!source.exists());
            expectEquals(result.outputFile.getFileName(), juce::String("converted.aif"));
            expectEquals(result.analysisRecord.bitsPerSample, 24);
            expectEquals(result.analysisRecord.sampleRate, 48000);
        }

        beginTest("Output that would clip is turned down to full scale and reported");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif");
            const auto sourceLength = AudioAnalysisService::analyzeFile(source).lengthInSamples;
            TestPlugin plugin({.gain = juce::Decibels::decibelsToGain(12.0f)});
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expectEquals(result.analysisRecord.channels, 2);
            expectEquals(result.analysisRecord.bitsPerSample, 16);
            expectEquals(result.analysisRecord.lengthInSamples, sourceLength);
            expect(peakDecibels(result.analysisRecord) <= 0.0, juce::String(peakDecibels(result.analysisRecord)));
            expect(peakDecibels(result.analysisRecord) > -0.1, juce::String(peakDecibels(result.analysisRecord)));
            expect(result.notice.contains("clipping"), result.notice);
        }
    }

private:
    /// Processes the file through the given plugin chain on the calling thread.
    /// Each plugin gets the saved state at its index in `states`, or no saved state.
    static PluginProcessingResult process(
        const juce::File& source,
        const std::vector<juce::AudioPluginInstance*>& chain,
        const std::vector<juce::MemoryBlock>& states = {}
    )
    {
        return PluginProcessingService::processFile(
            AudioAnalysisService::analyzeFile(source), makeOptions(chain, states), chain
        );
    }

    /// Builds options for the given chain that delete replaced files instead of moving them to the system trash.
    /// Each plugin gets the saved state at its index in `states`, or no saved state.
    static PluginProcessingOptions makeOptions(
        const std::vector<juce::AudioPluginInstance*>& chain,
        const std::vector<juce::MemoryBlock>& states = {}
    )
    {
        PluginProcessingOptions options;
        options.disposeFile = [](const juce::File& file) { return file.deleteFile(); };

        for (std::size_t index = 0; index < chain.size(); ++index) {
            PluginDescriptorRef descriptor;
            descriptor.identifierString = "test-plugin";
            descriptor.name = chain[index]->getName();

            if (index < states.size()) {
                descriptor.state = states[index];
            }

            options.plugins.push_back(descriptor);
        }

        return options;
    }
};

std::unique_ptr<juce::UnitTest> createProcessingTests()
{
    return std::make_unique<ProcessingTests>();
}
}  // namespace audiobatch::tests
