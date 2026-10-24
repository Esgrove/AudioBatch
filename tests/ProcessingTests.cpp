/// Tests for processFileWithPlugins with small in-process test plugins.
/// Covers mono files through stereo-only and mixed-width chains, undithered 16-bit output without a clip guard,
/// latency compensation, rendering and trimming the tail, per-file state, cancellation, and the output length.
/// Replaced files are deleted instead of moved to the system trash, so the tests never touch the user's trash.

#include "analysis/AudioAnalysis.h"
#include "plugins/PluginChainPreparation.h"
#include "plugins/PluginProcessing.h"
#include "plugins/PluginProcessingCoordinator.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/AudioMetadata.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include <condition_variable>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <generalencapsulatedobjectframe.h>
#include <limits>
#include <mutex>
#include <utility>
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
    /// Delays the output by this many samples and reports the delay as the plugin latency.
    int latencySamples = 0;
    /// Adds an echo at half level this many samples after the input, and reports the echo as the tail.
    int echoSamples = 0;
    /// Tail length reported without producing any tail, in seconds, for a plugin without an echo.
    double reportedTailSeconds = 0.0;
    /// Emits a repeated pattern spanning 16-bit quantization boundaries and the PCM clipping limits.
    bool producesQuantizationPattern {false};
};

/// A test plugin that applies a fixed gain and can swap the left and right channels.
/// It is stereo-only by default, like many effect plugins, and can also accept a mono layout.
/// Swapping exposes whether a mono source reaches the right input of a stereo-only plugin.
class TestPlugin : public juce::AudioPluginInstance
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
    {}

    /// Applies the gain and the optional channel swap to the main bus channels only,
    /// like a hosted plugin, which never touches buffer channels outside its own buses.
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/) override
    {
        widestBuffer = juce::jmax(widestBuffer, buffer.getNumChannels());
        const auto channels = juce::jmin(buffer.getNumChannels(), getMainBusNumOutputChannels());
        applyDelays(buffer, channels);

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

        if (settings.producesQuantizationPattern) {
            constexpr float leastSignificantBit {1.0f / 32'768.0f};
            constexpr std::array pattern {
                0.0f,
                0.25f * leastSignificantBit,
                -0.25f * leastSignificantBit,
                1.25f * leastSignificantBit,
                -1.25f * leastSignificantBit,
                1.75f * leastSignificantBit,
                -1.75f * leastSignificantBit,
                0.25f,
                -0.25f,
                1.25f,
                -1.25f,
                1.0f - leastSignificantBit,
            };
            for (int channel = 0; channel < channels; ++channel) {
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
                    buffer.setSample(channel, sample, pattern[static_cast<std::size_t>(sample) % pattern.size()]);
                }
            }
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

    /// Reports the latency and clears the delay history.
    void prepareToPlay(const double sampleRate, int /*maximumExpectedSamplesPerBlock*/) override
    {
        preparedSampleRate = sampleRate;
        setLatencySamples(settings.latencySamples);
        const auto historyLength
            = static_cast<std::size_t>(juce::jmax(settings.latencySamples, settings.echoSamples)) + 1;
        history.assign(2, std::vector<float>(historyLength, 0.0f));
        historyPosition = 0;
    }

    /// Clears the delay history, so no signal from an earlier file is left in it.
    void reset() override
    {
        for (auto& channelHistory : history) {
            std::ranges::fill(channelHistory, 0.0f);
        }

        historyPosition = 0;
    }

    /// Holds no resources.
    void releaseResources() override {}

    /// Returns the echo length, or the reported tail when there is no echo.
    double getTailLengthSeconds() const override
    {
        return settings.echoSamples > 0 ? settings.echoSamples / preparedSampleRate : settings.reportedTailSeconds;
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
    void setCurrentProgram(int /*index*/) override {}

    /// Returns no program name.
    const juce::String getProgramName(int /*index*/) override
    {
        return {};
    }

    /// Ignores program renames.
    void changeProgramName(int /*index*/, const juce::String& /*newName*/) override {}

    /// Saves the current gain as the plugin state.
    void getStateInformation(juce::MemoryBlock& destData) override
    {
        destData.replaceAll(&currentGain, sizeof(currentGain));
    }

    /// Restores the current gain from a state saved by getStateInformation.
    void setStateInformation(const void* data, const int sizeInBytes) override
    {
        if (std::cmp_equal(sizeInBytes, sizeof(currentGain))) {
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

    /// Delays the main channels by the latency and adds the echo, through a per-channel history of recent input.
    void applyDelays(juce::AudioBuffer<float>& buffer, const int channels)
    {
        if ((settings.latencySamples == 0 && settings.echoSamples == 0) || history.empty()) {
            return;
        }

        const auto historyLength = history.front().size();
        const auto delayedPosition = [&](const int delay) {
            return (historyPosition + historyLength - static_cast<std::size_t>(delay)) % historyLength;
        };

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            for (int channel = 0; channel < juce::jmin(channels, 2); ++channel) {
                auto& channelHistory = history[static_cast<std::size_t>(channel)];
                channelHistory[historyPosition] = buffer.getSample(channel, sample);

                auto output = channelHistory[delayedPosition(settings.latencySamples)];
                if (settings.echoSamples > 0) {
                    output += 0.5f * channelHistory[delayedPosition(settings.echoSamples)];
                }

                buffer.setSample(channel, sample, output);
            }

            historyPosition = (historyPosition + 1) % historyLength;
        }
    }

    TestPluginSettings settings;
    float currentGain;
    int widestBuffer = 0;
    double preparedSampleRate = 44100.0;
    std::vector<std::vector<float>> history;
    std::size_t historyPosition = 0;
};

/// Records preparation order and emulates an Audio Unit that needs initialization before state restoration.
/// Its null native handle exercises the AU lifecycle without calling a real Audio Unit.
class PreparationTestPlugin final : public TestPlugin, public juce::AudioPluginExtensions::AudioUnitClient
{
public:
    /// Starts with a saved gain different from the unity gain reinstated by every AU preparation.
    explicit PreparationTestPlugin(const bool emulateAudioUnit) :
        TestPlugin({.gain = 0.25f}),
        audioUnit(emulateAudioUnit)
    {}

    /// Advertises AU extensions only when testing the Audio Unit lifecycle.
    juce::AudioPluginExtensions::AudioUnitClient* getAudioUnitClient() override
    {
        return audioUnit ? this : nullptr;
    }

    /// Exposes the same format identity to const host queries.
    const juce::AudioPluginExtensions::AudioUnitClient* getAudioUnitClient() const override
    {
        return audioUnit ? this : nullptr;
    }

    /// Prevents native offline-render and latency queries from reaching the operating system.
    AudioUnit getAudioUnitHandle() const noexcept override
    {
        return nullptr;
    }

    /// Emulates AU initialization resetting parameters, so a second preparation destroys restored state.
    void prepareToPlay(const double sampleRate, const int samplesPerBlock) override
    {
        lifecycle.add("prepare");
        preparedBlockSize = samplesPerBlock;
        lastSampleRate = sampleRate;
        TestPlugin::prepareToPlay(sampleRate, samplesPerBlock);
        initialized = true;
        if (audioUnit) {
            constexpr float defaultGain = 1.0f;
            TestPlugin::setStateInformation(&defaultGain, sizeof(defaultGain));
        }
    }

    /// Rejects premature AU restoration and records whether other formats were restored before activation.
    void setStateInformation(const void* data, const int sizeInBytes) override
    {
        lifecycle.add(initialized ? "restore-initialized" : "restore-inactive");
        if (!audioUnit || initialized) {
            TestPlugin::setStateInformation(data, sizeInBytes);
        }
    }

    /// Records the chain reset without changing the restored gain.
    void reset() override
    {
        lifecycle.add("reset");
        TestPlugin::reset();
    }

    /// Requires the next file to initialize the plugin again before AU state can be restored.
    void releaseResources() override
    {
        lifecycle.add("release");
        initialized = false;
        TestPlugin::releaseResources();
    }

    /// Returns the ordered host lifecycle calls without allowing tests to mutate the record.
    [[nodiscard]] const juce::StringArray& getLifecycle() const
    {
        return lifecycle;
    }

    /// Returns the block size supplied by the most recent preparation.
    [[nodiscard]] int getPreparedBlockSize() const
    {
        return preparedBlockSize;
    }

    /// Returns the sample rate supplied by the most recent preparation.
    [[nodiscard]] double getLastSampleRate() const
    {
        return lastSampleRate;
    }

    /// Starts a fresh lifecycle record without changing the plugin's initialization state.
    void clearLifecycle()
    {
        lifecycle.clear();
    }

private:
    /// Ordered host lifecycle calls, excluding audio rendering.
    juce::StringArray lifecycle;
    /// Most recent preparation block size, including editor-specific sizes.
    int preparedBlockSize = 0;
    /// Most recent preparation sample rate.
    double lastSampleRate = 0.0;

    bool audioUnit;
    bool initialized = false;
};

/// Checks plugin processing output against the committed fixtures.
class ProcessingTests final : public juce::UnitTest
{
public:
    ProcessingTests() : juce::UnitTest("Plugin processing", "AudioBatch") {}

    /// Runs the processing checks.
    void runTest() override
    {
        const TemporaryDirectory directory;

        beginTest("Plugin worker budgets reserve one physical core and handle unavailable counts");
        {
            expectEquals(getPluginProcessingWorkerCount(16, 32), 15);
            expectEquals(getPluginProcessingWorkerCount(8, 16), 7);
            expectEquals(getPluginProcessingWorkerCount(1, 2), 1);
            expectEquals(getPluginProcessingWorkerCount(2, 4), 1);
            expectEquals(getPluginProcessingWorkerCount(0, 32), 15);
            expectEquals(getPluginProcessingWorkerCount(-1, 17), 7);
            expectEquals(getPluginProcessingWorkerCount(0, 1), 1);
            expectEquals(getPluginProcessingWorkerCount(0, -1), 1);
            expectEquals(getPluginProcessingWorkerCount(16, 16), 15);
        }

        beginTest("Coordinator queues selected files in input order and skips duplicates and missing files");
        testCoordinator(directory, 1);

        beginTest("Coordinator can start six independent files concurrently");
        testCoordinator(directory, 6);

        beginTest("An empty queue or invalid chain does not announce processing");
        {
            const ScopedLogCapture capture;
            PluginProcessingCoordinator coordinator(1);
            TestPlugin plugin(TestPluginSettings {});
            const auto options = makeOptions({&plugin});
            expectEquals(coordinator.start({}, options, {}), 0);
            PluginChainInstances chain;
            chain.push_back(std::make_unique<TestPlugin>(TestPluginSettings {}));
            std::vector<PluginChainInstances> chains;
            chains.push_back(std::move(chain));
            expectEquals(coordinator.start({}, options, std::move(chains)), 0);
            expect(capture.getMessages().isEmpty());
        }

        beginTest("A mono file feeds both inputs of a stereo-only plugin and stays mono");
        {
            const auto source = directory.copyFixture("sine_mono_48000_24.aif");
            TestPlugin plugin({.swapChannels = true});
            const ScopedLogCapture capture;
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expectEquals(result.analysisRecord.channels, 1);
            expectEquals(result.analysisRecord.bitsPerSample, 16);
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), fixturePeakDecibels, 0.05);
            expect(result.notice.isEmpty(), result.notice);
            const auto messages = capture.getMessages();
            expect(std::ranges::any_of(messages, [&](const auto& message) {
                return message.contains("[INFO]: Processed: ")
                    && message.endsWith(utils::format(" s, {}", source.getFileName().quoted()));
            }));
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

        beginTest("AU saved gain survives one preparation and rendering for each released file");
        {
            PreparationTestPlugin plugin(true);
            juce::MemoryBlock savedState;
            plugin.getStateInformation(savedState);
            const auto expectedPeak = peakDecibels(analyzeFile(fixtureFile("sine_stereo_44100_16.aif")))
                + juce::Decibels::gainToDecibels(0.25);

            for (int fileIndex = 0; fileIndex < 2; ++fileIndex) {
                const auto source
                    = directory.copyFixture("sine_stereo_44100_16.aif", utils::format("au_state_{}.aif", fileIndex));
                plugin.clearLifecycle();
                const auto result = process(source, {&plugin}, {savedState});

                expect(!result.hasError(), result.errorMessage);
                expect(result.committed);
                expectWithinAbsoluteError(peakDecibels(result.analysisRecord), expectedPeak, 0.01);
                expectEquals(plugin.getPreparedBlockSize(), plugin_processing::processingBlockSize);
                expectWithinAbsoluteError(plugin.getLastSampleRate(), 44100.0, 0.001);
                expectEquals(
                    plugin.getLifecycle().joinIntoString(","), juce::String("prepare,restore-initialized,reset,release")
                );
            }
        }

        beginTest("The editor preparation helper restores AU gain after a single 512-sample preparation");
        {
            PreparationTestPlugin plugin(true);
            juce::MemoryBlock savedState;
            plugin.getStateInformation(savedState);

            plugin_processing::restoreStateAndPrepare(plugin, savedState, 48000.0, 512);

            expectEquals(plugin.getPreparedBlockSize(), 512);
            expectWithinAbsoluteError(plugin.getLastSampleRate(), 48000.0, 0.001);
            expectEquals(plugin.getLifecycle().joinIntoString(","), juce::String("prepare,restore-initialized"));
            expectPreparedGain(plugin, 0.25f);
            plugin_processing::releasePluginResources({&plugin});
        }

        beginTest("An AU with empty saved state is prepared once and reset without restoring");
        {
            PreparationTestPlugin plugin(true);
            const auto preparation = plugin_processing::prepareChain({&plugin}, makeOptions({&plugin}), 2, 44100.0);

            expect(preparation.errorMessage.isEmpty(), preparation.errorMessage);
            expectEquals(plugin.getLifecycle().joinIntoString(","), juce::String("prepare,reset"));
            expectEquals(plugin.getPreparedBlockSize(), plugin_processing::processingBlockSize);
            expectPreparedGain(plugin, 1.0f);
            plugin_processing::releasePluginResources({&plugin});
        }

        beginTest("The editor preparation helper restores non-AU state before activation");
        {
            PreparationTestPlugin plugin(false);
            juce::MemoryBlock savedState;
            plugin.getStateInformation(savedState);

            for (int preparationIndex = 0; preparationIndex < 2; ++preparationIndex) {
                plugin.clearLifecycle();
                plugin_processing::restoreStateAndPrepare(plugin, savedState, 48000.0, 512);

                expectEquals(plugin.getLifecycle().joinIntoString(","), juce::String("restore-inactive,prepare"));
                expectEquals(plugin.getPreparedBlockSize(), 512);
                expectWithinAbsoluteError(plugin.getLastSampleRate(), 48000.0, 0.001);
                expectPreparedGain(plugin, 0.25f);
                plugin_processing::releasePluginResources({&plugin});
            }
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

            const auto result = processFileWithPlugins(analyzeFile(source), options, {&plugin});

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
            expectEquals(result.analysisRecord.bitsPerSample, 16);
            expectEquals(result.analysisRecord.sampleRate, 48000);
        }

        beginTest("Plugin latency is removed from the start of the output");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "latency.aif");
            const auto sourceLength = analyzeFile(source).lengthInSamples;
            TestPlugin plugin({.latencySamples = 1000});
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expectEquals(result.analysisRecord.lengthInSamples, sourceLength);
            expectWithinAbsoluteError(
                maximumDifference(fixtureFile("sine_stereo_44100_16.aif"), result.outputFile, 512), 0.0f, 0.001f
            );
        }

        beginTest("The tail of a plugin is rendered past the end of the source");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "echo.aif");
            const auto sourceLength = analyzeFile(source).lengthInSamples;
            constexpr int echoSamples = 4410;
            TestPlugin plugin({.echoSamples = echoSamples});
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expect(result.analysisRecord.lengthInSamples > sourceLength + echoSamples - 20);
            expect(result.analysisRecord.lengthInSamples <= sourceLength + echoSamples);
        }

        beginTest("A silent tail allowance is trimmed back to the source length");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "silent_tail.aif");
            const auto sourceLength = analyzeFile(source).lengthInSamples;
            TestPlugin plugin({.reportedTailSeconds = 0.5});
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expectEquals(result.analysisRecord.lengthInSamples, sourceLength);
        }

        beginTest("Plugin processing retains custom tags and Serato markers but drops the overview");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "serato_tags.aif");
            AudioMetadata metadata;
            metadata.properties.push_back({.key = "CUSTOM_DATA", .values = {"custom value"}});
            const TagLib::ByteVector payload("\0\1\2\xff", 4);
            for (const auto* description : {"Serato Markers2", "Serato BeatGrid", "Serato Overview"}) {
                TagLib::ID3v2::GeneralEncapsulatedObjectFrame frame;
                frame.setDescription(description);
                frame.setObject(payload);
                const auto bytes = frame.render();
                metadata.id3v2Frames.emplace_back(bytes.data(), bytes.size());
            }
            expect(writeMetadata(source, metadata));
            TestPlugin plugin({.gain = 0.5f});
            const auto result = process(source, {&plugin});
            expect(!result.hasError(), result.errorMessage);
            AudioMetadata written;
            expect(readMetadata(result.outputFile, written));
            metadata.id3v2Frames.pop_back();
            expect(written.id3v2Frames.size() == metadata.id3v2Frames.size() + 1);
            for (const auto& frame : metadata.id3v2Frames) {
                expect(std::ranges::find(written.id3v2Frames, frame) != written.id3v2Frames.end());
            }
            const auto custom = std::ranges::find_if(written.properties, [](const auto& property) {
                return property.key == "CUSTOM_DATA";
            });
            expect(custom != written.properties.end() && custom->values[0] == "custom value");
        }

        beginTest("A cancelled run leaves the source untouched and removes its temporary files");
        {
            const TemporaryDirectory cancelDirectory;
            const auto source = cancelDirectory.copyFixture("sine_stereo_44100_16.aif", "cancelled.aif");
            const auto sourceSize = source.getSize();
            const auto sourceModified = source.getLastModificationTime();
            TestPlugin plugin({.gain = 0.5f});
            const auto result
                = processFileWithPlugins(analyzeFile(source), makeOptions({&plugin}), {&plugin}, [] { return true; });

            expect(result.hasError());
            expect(result.errorMessage.contains("cancelled"), result.errorMessage);
            expectEquals(source.getSize(), sourceSize);
            expect(source.getLastModificationTime() == sourceModified);
            expectEquals(source.getParentDirectory().findChildFiles(juce::File::findFiles, false).size(), 1);
        }

        beginTest("Cancellation stops changed-source analysis before preparing the plugin chain");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "cancelled_refresh.aif");
            auto staleRecord = analyzeFile(source);
            staleRecord.modifiedTimeMs -= 1000;
            TestPlugin plugin({});
            const auto cancelledAnalysis = analyzeFile(source, [] { return true; });
            const auto result
                = processFileWithPlugins(staleRecord, makeOptions({&plugin}), {&plugin}, [] { return true; });

            expect(result.hasError());
            expectEquals(result.errorMessage, cancelledAnalysis.errorMessage);
            expect(!result.committed);
            expectEquals(plugin.getWidestBufferChannels(), 0);
        }

        beginTest("Normalization before the chain follows the selected peak target");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "pre_peak.aif");
            TestPlugin plugin({});
            auto options = makeOptions({&plugin});
            AudioNormalizationOptions target;
            target.mode = AudioNormalizationMode::peak;
            target.targetPeakDbfs = -3.0;
            options.normalizeBeforePlugin = target;

            const auto result = processFileWithPlugins(analyzeFile(source), options, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), -3.0, 0.05);
        }

        beginTest("Normalization before the chain follows the selected loudness target");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "pre_loudness.aif");
            TestPlugin plugin({});
            auto options = makeOptions({&plugin});
            AudioNormalizationOptions target;
            target.mode = AudioNormalizationMode::loudness;
            target.targetLufs = -20.0;
            options.normalizeBeforePlugin = target;

            const auto result = processFileWithPlugins(analyzeFile(source), options, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expectWithinAbsoluteError(result.analysisRecord.integratedLufs, -20.0, 0.1);
        }

        beginTest("A custom gain wins over normalization before the chain");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "pre_custom.aif");
            TestPlugin plugin({});
            auto options = makeOptions({&plugin});
            options.normalizeBeforePlugin = AudioNormalizationOptions {};
            auto record = analyzeFile(source);
            record.hasCustomGain = true;
            record.customGainDb = -6.0f;

            const auto result = processFileWithPlugins(record, options, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), fixturePeakDecibels - 6.0, 0.05);
        }

        beginTest("Every source depth produces 16-bit PCM while preserving rate and channel count");
        {
            for (const auto* fixture :
                 {"sine_stereo_44100_16.aif", "sine_stereo_48000_24.wav", "sine_stereo_44100_32f.wav"})
            {
                const auto source = directory.copyFixture(fixture);
                const auto original = analyzeFile(source);
                TestPlugin plugin({});
                const auto result = process(source, {&plugin});
                expect(!result.hasError(), result.errorMessage);
                expectEquals(result.analysisRecord.bitsPerSample, 16);
                expectEquals(result.analysisRecord.sampleRate, original.sampleRate);
                expectEquals(result.analysisRecord.channels, original.channels);
                expectEquals(result.analysisRecord.lengthInSamples, original.lengthInSamples);
                const auto reader = utils::createAudioReader(result.outputFile);
                expect(reader != nullptr);
                if (reader != nullptr) {
                    expectEquals(reader->bitsPerSample, 16U);
                    expect(!reader->usesFloatingPointData);
                }
            }
        }

        beginTest("Digital silence stays exactly silent without AudioBatch dithering");
        {
            const auto source = directory.copyFixture("silence_stereo_44100_16.wav");
            TestPlugin plugin({});
            const auto result = process(source, {&plugin});
            expect(!result.hasError(), result.errorMessage);
            const auto reader = utils::createAudioReader(result.outputFile);
            expect(reader != nullptr);
            if (reader != nullptr) {
                juce::AudioBuffer<float> samples(2, static_cast<int>(reader->lengthInSamples));
                expect(reader->read(&samples, 0, samples.getNumSamples(), 0, true, true));
                expectWithinAbsoluteError(samples.getMagnitude(0, samples.getNumSamples()), 0.0f, 0.0f);
            }
        }

        beginTest("PCM output truncates low bits without dither, attenuation, or overflow wraparound");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "quantization.aif");
            TestPlugin plugin({.producesQuantizationPattern = true});
            const auto result = process(source, {&plugin});
            expect(!result.hasError(), result.errorMessage);
            expect(result.notice.contains("Output clipping"), result.notice);
            const auto reader = utils::createAudioReader(result.outputFile);
            expect(reader != nullptr);
            if (reader != nullptr) {
                constexpr std::array expected {0, 0, -1, 1, -2, 1, -2, 8192, -8192, 32'767, -32'768, 32'767};
                std::array<int, 512> left {};
                std::array<int, 512> right {};
                const std::array channels {left.data(), right.data()};
                expect(reader->read(channels.data(), 2, 0, static_cast<int>(left.size()), false));
                for (std::size_t sample = 0; sample < left.size(); ++sample) {
                    const auto expectedSample = expected[sample % expected.size()] * 65'536;
                    expectEquals(left[sample], expectedSample);
                    expectEquals(right[sample], expectedSample);
                }
            }
        }

        beginTest("Float overs between plugins do not report clipping when the final output is in range");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "float_overs.aif");
            TestPlugin boost({.gain = 4.0f});
            TestPlugin attenuate({.gain = 0.25f});
            const ScopedLogCapture capture;
            const auto result = process(source, {&boost, &attenuate});

            expect(!result.hasError(), result.errorMessage);
            expect(result.notice.isEmpty(), result.notice);
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), fixturePeakDecibels, 0.05);
            expect(std::ranges::none_of(capture.getMessages(), [](const auto& message) {
                return message.contains("Output clipping");
            }));
        }

        beginTest("Output that would clip saturates without automatic gain reduction and reports clipping");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif");
            const auto sourceLength = analyzeFile(source).lengthInSamples;
            TestPlugin plugin({.gain = juce::Decibels::decibelsToGain(12.0f)});
            const ScopedLogCapture capture;
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), result.errorMessage);
            expectEquals(result.analysisRecord.channels, 2);
            expectEquals(result.analysisRecord.bitsPerSample, 16);
            expectEquals(result.analysisRecord.lengthInSamples, sourceLength);
            expect(peakDecibels(result.analysisRecord) <= 0.0, juce::String(peakDecibels(result.analysisRecord)));
            expect(peakDecibels(result.analysisRecord) > -0.1, juce::String(peakDecibels(result.analysisRecord)));
            expect(result.notice.contains("Output clipping"), result.notice);
            expect(std::ranges::any_of(capture.getMessages(), [&](const auto& message) {
                return message.endsWith(
                    utils::format(
                        "[WARN]: Plugin processing warning: {}, {}",
                        result.outputFile.getFullPathName().quoted(),
                        result.notice
                    )
                );
            }));
        }
    }

private:
    /// Checks FIFO pickup with one worker and a bounded start barrier with six workers.
    /// Existing outputs reject each job before plugin preparation, avoiding message-loop waits and file replacement.
    void testCoordinator(const TemporaryDirectory& directory, const int workerCount)
    {
        const ScopedLogCapture capture;
        const std::vector<juce::String> names {"zulu", "alpha", "mike", "bravo", "yankee", "charlie"};
        std::vector<AudioAnalysisRecord> records;
        std::vector<juce::String> expectedPaths;
        for (const auto& fileStem : names) {
            const auto source = directory.copyFixture("sine_stereo_44100_16.wav", fileStem + ".wav");
            const auto existingOutput = directory.copyFixture("sine_stereo_44100_16.aif", fileStem + ".aif");
            expect(existingOutput.existsAsFile());
            records.push_back(analyzeFile(source));
            expectedPaths.push_back(source.getFullPathName());
        }
        records.push_back(records.front());
        AudioAnalysisRecord missing;
        missing.file = directory.getChildFile("missing.wav");
        missing.fullPath = missing.file.getFullPathName();
        records.push_back(missing);

        std::mutex mutex;
        std::condition_variable condition;
        std::vector<juce::String> startedPaths;
        int completedFiles = 0;
        int resultCount = 0;
        bool released = workerCount == 1;
        PluginProcessingCoordinator coordinator(workerCount);
        coordinator.setStartingCallback([&](const juce::File& file) {
            std::unique_lock lock(mutex);
            utils::logInfo("Test file starting: {}", file.getFileName().quoted());
            startedPaths.push_back(file.getFullPathName());
            condition.notify_all();
            condition.wait_for(lock, std::chrono::seconds(10), [&] { return released; });
        });
        coordinator.setResultCallback([&](const PluginProcessingResult&) {
            const std::scoped_lock lock(mutex);
            ++resultCount;
        });
        coordinator.setCompletionCallback([&](const int totalFiles) {
            const std::scoped_lock lock(mutex);
            completedFiles = totalFiles;
            condition.notify_all();
        });

        std::vector<PluginChainInstances> chains;
        for (int index = 0; index < workerCount; ++index) {
            PluginChainInstances chain;
            chain.push_back(std::make_unique<TestPlugin>(TestPluginSettings {}));
            chains.push_back(std::move(chain));
        }
        const auto options = makeOptions({chains.front().front().get()});
        expectEquals(coordinator.start(records, options, std::move(chains)), 6);
        {
            std::unique_lock lock(mutex);
            if (workerCount > 1) {
                expect(condition.wait_for(lock, std::chrono::seconds(5), [&] { return startedPaths.size() == 6; }));
                released = true;
                condition.notify_all();
            }
            expect(condition.wait_for(lock, std::chrono::seconds(10), [&] { return completedFiles == 6; }));
        }
        coordinator.cancelAndWait();
        expectEquals(resultCount, 6);
        const auto messages = capture.getMessages();
        expect(!messages.isEmpty());
        if (!messages.isEmpty()) {
            expect(
                messages[0].endsWith(
                    utils::format("[INFO]: Plugin processing started: 6 files, up to {} concurrent", workerCount)
                )
            );
        }
        expectEquals(
            static_cast<int>(std::ranges::count_if(
                messages, [](const auto& message) { return message.contains("Plugin processing started:"); }
            )),
            1
        );
        if (workerCount == 1) {
            expect(startedPaths == expectedPaths);
        }
    }

    /// Checks the restored gain through audio rendering rather than relying on serialized state alone.
    void expectPreparedGain(juce::AudioPluginInstance& plugin, const float gain)
    {
        juce::AudioBuffer<float> buffer(2, 1);
        buffer.setSample(0, 0, 0.5f);
        buffer.setSample(1, 0, 0.5f);
        juce::MidiBuffer midi;
        plugin.processBlock(buffer, midi);
        expectWithinAbsoluteError(buffer.getSample(0, 0), 0.5f * gain, 0.000001f);
        expectWithinAbsoluteError(buffer.getSample(1, 0), 0.5f * gain, 0.000001f);
    }

    /// Processes the file through the given plugin chain on the calling thread.
    /// Each plugin gets the saved state at its index in `states`, or no saved state.
    static PluginProcessingResult process(
        const juce::File& source,
        const std::vector<juce::AudioPluginInstance*>& chain,
        const std::vector<juce::MemoryBlock>& states = {}
    )
    {
        return processFileWithPlugins(analyzeFile(source), makeOptions(chain, states), chain);
    }

    /// Returns the largest absolute sample difference between the first samples of two files,
    /// or 1 when either file cannot be read.
    static float maximumDifference(const juce::File& expected, const juce::File& actual, const int numSamples)
    {
        auto& formatManager = utils::threadLocalFormatManager();
        const std::unique_ptr<juce::AudioFormatReader> expectedReader(formatManager.createReaderFor(expected));
        const std::unique_ptr<juce::AudioFormatReader> actualReader(formatManager.createReaderFor(actual));

        if (expectedReader == nullptr || actualReader == nullptr) {
            return 1.0f;
        }

        juce::AudioBuffer<float> expectedSamples(static_cast<int>(expectedReader->numChannels), numSamples);
        juce::AudioBuffer<float> actualSamples(static_cast<int>(actualReader->numChannels), numSamples);
        expectedReader->read(&expectedSamples, 0, numSamples, 0, true, true);
        actualReader->read(&actualSamples, 0, numSamples, 0, true, true);

        float difference = 0.0f;
        for (int channel = 0; channel < juce::jmin(expectedSamples.getNumChannels(), actualSamples.getNumChannels());
             ++channel)
        {
            for (int sample = 0; sample < numSamples; ++sample) {
                difference = juce::jmax(
                    difference,
                    std::abs(expectedSamples.getSample(channel, sample) - actualSamples.getSample(channel, sample))
                );
            }
        }

        return difference;
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
