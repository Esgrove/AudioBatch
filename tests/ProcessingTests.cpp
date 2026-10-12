/// Tests for PluginProcessingService::processFile with small in-process test plugins.
/// Covers mono files through stereo-only and mixed-width chains, the clip guard, the output bit depth,
/// and the output length.
/// Only `.aif` sources are processed, which are rewritten in place,
/// because processing other formats moves the source to the system trash.

#include "AudioAnalysisService.h"
#include "PluginProcessingService.h"
#include "TestHelpers.h"
#include "TestSuites.h"

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
        settings(pluginSettings)
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
            buffer.applyGain(channel, 0, buffer.getNumSamples(), settings.gain);
        }

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

    /// Has no state to save.
    void getStateInformation(juce::MemoryBlock& /*destData*/) override { }

    /// Has no state to restore.
    void setStateInformation(const void* /*data*/, int /*sizeInBytes*/) override { }

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
            expect(result.errorMessage.contains("invalid samples"), result.errorMessage);
            expectEquals(source.getSize(), sourceSize);
            expect(source.getLastModificationTime() == sourceModified);
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
    static PluginProcessingResult process(
        const juce::File& source,
        const std::vector<juce::AudioPluginInstance*>& chain
    )
    {
        PluginProcessingOptions options;

        for (const auto* plugin : chain) {
            PluginDescriptorRef descriptor;
            descriptor.identifierString = "test-plugin";
            descriptor.name = plugin->getName();
            options.plugins.push_back(descriptor);
        }

        return PluginProcessingService::processFile(AudioAnalysisService::analyzeFile(source), options, chain);
    }
};

std::unique_ptr<juce::UnitTest> createProcessingTests()
{
    return std::make_unique<ProcessingTests>();
}
}  // namespace audiobatch::tests
