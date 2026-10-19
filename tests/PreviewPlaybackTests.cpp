/// Device-free regression tests for the live preview renderer and persisted preferences.
/// Synthetic processors expose latency, gain edits, ordering, EOF tails, and invalid output
/// without opening hardware or loading third-party plugins.

#include "gui/components/AudioSettingsComponent.h"
#include "plugins/PluginChain.h"
#include "plugins/PluginPreviewSource.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace audiobatch::tests
{
namespace
{
constexpr double previewTestRate = 8000.0;
constexpr int previewTestBlockSize = 64;

/// Editable behavior shared by the test processor and its simulated editor.
struct PreviewTestSettings {
    float gain = 1.0f;
    float offset = 0.0f;
    int delay = 0;
    int echo = 0;
    double reportedTail = -1.0;
    bool invalid = false;
};

/// Small mutable effect whose editor-equivalent gain field is read directly by the renderer.
class PreviewTestPlugin final : public juce::AudioPluginInstance
{
public:
    /// Uses stereo buses so the same processor can exercise mono duplication and stereo output.
    PreviewTestPlugin() :
        juce::AudioPluginInstance(
            BusesProperties()
                .withInput("Input", juce::AudioChannelSet::stereo())
                .withOutput("Output", juce::AudioChannelSet::stereo())
        )
    {}

    /// Allocates delay history before rendering and reports the configured latency.
    void prepareToPlay(const double sampleRate, int /*maximumExpectedSamplesPerBlock*/) override
    {
        rate = sampleRate;
        setLatencySamples(settings.delay);
        history.setSize(2, settings.delay + settings.echo + 1);
        reset();
    }

    /// Erases all delayed input without changing gain, as a seek must do.
    void reset() override
    {
        history.clear();
        historyPosition = 0;
    }

    /// Owns no external resources.
    void releaseResources() override {}

    /// Delays input and optionally adds an echo, then applies editable gain and offset.
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/) override
    {
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            for (int channel = 0; channel < 2; ++channel) {
                history.setSample(channel, historyPosition, buffer.getSample(channel, sample));
                const auto delayed
                    = (historyPosition + history.getNumSamples() - settings.delay) % history.getNumSamples();
                float output = history.getSample(channel, delayed);
                if (settings.echo > 0) {
                    const auto echoed = (historyPosition + history.getNumSamples() - settings.delay - settings.echo)
                        % history.getNumSamples();
                    output += 0.5f * history.getSample(channel, echoed);
                }
                buffer.setSample(
                    channel,
                    sample,
                    settings.invalid ? std::numeric_limits<float>::quiet_NaN()
                                     : output * settings.gain + settings.offset
                );
            }
            historyPosition = (historyPosition + 1) % history.getNumSamples();
        }
    }

    /// Describes the test effect without relying on a plugin installation.
    void fillInPluginDescription(juce::PluginDescription& description) const override
    {
        description.name = getName();
    }
    /// Names the processor for error diagnostics.
    const juce::String getName() const override
    {
        return "Preview test effect";
    }
    /// Reports the echo or an explicitly overridden synthetic tail.
    double getTailLengthSeconds() const override
    {
        return settings.reportedTail >= 0.0 ? settings.reportedTail : settings.echo / rate;
    }
    /// Does not receive MIDI.
    bool acceptsMidi() const override
    {
        return false;
    }
    /// Does not produce MIDI.
    bool producesMidi() const override
    {
        return false;
    }
    /// Needs no native editor in device-free tests.
    bool hasEditor() const override
    {
        return false;
    }
    /// Has a single program.
    int getNumPrograms() override
    {
        return 1;
    }
    /// Always uses that program.
    int getCurrentProgram() override
    {
        return 0;
    }
    /// Ignores program selection.
    void setCurrentProgram(int /*index*/) override {}
    /// Does not name the program.
    const juce::String getProgramName(int /*index*/) override
    {
        return {};
    }
    /// Ignores program renaming.
    void changeProgramName(int /*index*/, const juce::String& /*name*/) override {}
    /// Serializes the current editable gain for state-retention checks.
    void getStateInformation(juce::MemoryBlock& state) override
    {
        state.replaceAll(&settings.gain, sizeof(settings.gain));
    }
    /// Restores a correctly sized saved gain.
    void setStateInformation(const void* data, const int size) override
    {
        if (std::cmp_equal(size, sizeof(settings.gain))) {
            std::memcpy(&settings.gain, data, sizeof(settings.gain));
        }
    }

    /// Provides editor-equivalent access without replacing the running processor.
    PreviewTestSettings& getSettings()
    {
        return settings;
    }

private:
    /// Supplies no editor.
    juce::AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }

    PreviewTestSettings settings;
    juce::AudioBuffer<float> history;
    double rate = previewTestRate;
    int historyPosition = 0;
};

/// Collects callback output until the renderer finishes, including any trailing zero padding in the final callback.
std::vector<float> renderPreview(PluginPreviewSource& source, const int blockSize = previewTestBlockSize)
{
    std::vector<float> samples;
    juce::AudioBuffer<float> output(2, blockSize);
    constexpr int maximumTestBlocks = 1000;
    for (int block = 0; block < maximumTestBlocks && source.isPlaying(); ++block) {
        source.getNextAudioBlock({&output, 0, blockSize});
        const std::span blockSamples(output.getReadPointer(0), static_cast<std::size_t>(blockSize));
        samples.insert(samples.end(), blockSamples.begin(), blockSamples.end());
    }
    return samples;
}

/// Builds preparation data from already prepared in-process processors.
plugin_processing::ChainPreparation previewPreparation(const std::vector<juce::AudioPluginInstance*>& plugins)
{
    plugin_processing::ChainPreparation result;
    result.processChannelCount = 2;
    for (const auto* plugin : plugins) {
        result.latencySamples += plugin->getLatencySamples();
        result.tailSamples += plugin_processing::getPluginTailSamples(*plugin, previewTestRate);
    }
    return result;
}
}  // namespace

/// Covers the actual live rendering and preference policy, not GUI rendering or screenshots.
class PreviewPlaybackTests final : public juce::UnitTest
{
public:
    /// Registers a lightweight suite with the existing factory-driven runner.
    PreviewPlaybackTests() : juce::UnitTest("Preview playback", "AudioBatch") {}

    /// Runs device-independent signal and persistence checks.
    void runTest() override
    {
        testLatency();
        testCursor();
        testGainAndEdits();
        testTailAndReset();
        testStopRampAndTailUpdates();
        testInvalidOutput();
        testResampling();
        testPreferences();
        testLatencyFormattingAndTailLimit();
        testUnavailableSlotLatency();
    }

private:
    /// Preserves the first and last source samples across zero, short, and multi-block latency.
    void testLatency()
    {
        beginTest("Compensation discards exactly the delay while preserving final source audio");
        for (const auto delay : {0, 5, 131, 511}) {
            for (const bool compensate : {false, true}) {
                juce::AudioBuffer<float> audio(2, 512);
                audio.clear();
                audio.setSample(0, 100, 0.5f);
                audio.setSample(0, 511, 0.25f);
                juce::MemoryAudioSource input(audio, false);
                PreviewTestPlugin plugin;
                plugin.getSettings().delay = delay;
                plugin.prepareToPlay(previewTestRate, previewTestBlockSize);
                PluginPreviewSource source;
                source.setInput(&input, 512.0 / previewTestRate, 2);
                source.setChain({&plugin}, previewPreparation({&plugin}), true, compensate);
                source.prepareToPlay(previewTestBlockSize, previewTestRate);
                source.start();
                const auto output = renderPreview(source);
                expect(!source.isPlaying());
                expect(!source.hasFailed());
                expect(output.size() >= 512 + static_cast<std::size_t>(delay));
                if (output.size() >= 512 + static_cast<std::size_t>(delay)) {
                    expectWithinAbsoluteError(output[100 + static_cast<std::size_t>(delay)], 0.5f, 0.00001f);
                    expectWithinAbsoluteError(output[511 + static_cast<std::size_t>(delay)], 0.25f, 0.00001f);
                }
                expectWithinAbsoluteError(source.getCurrentPosition(), 512.0 / previewTestRate, 0.000001);
                source.releaseResources();
            }
        }
    }

    /// Holds the audible clock during non-block-aligned priming, unlike uncompensated input time.
    void testCursor()
    {
        beginTest("Only compensated playback freezes its logical cursor during pre-roll");
        for (const bool compensate : {false, true}) {
            juce::AudioBuffer<float> audio(2, 512);
            audio.clear();
            juce::MemoryAudioSource input(audio, false);
            PreviewTestPlugin plugin;
            plugin.getSettings().delay = 131;
            plugin.prepareToPlay(previewTestRate, previewTestBlockSize);
            PluginPreviewSource source;
            source.setInput(&input, 512.0 / previewTestRate, 2);
            source.setChain({&plugin}, previewPreparation({&plugin}), true, compensate);
            source.prepareToPlay(previewTestBlockSize, previewTestRate);
            source.start();
            juce::AudioBuffer<float> output(2, previewTestBlockSize);
            source.getNextAudioBlock({&output, 0, previewTestBlockSize});
            source.getNextAudioBlock({&output, 0, previewTestBlockSize});
            expectWithinAbsoluteError(
                source.getCurrentPosition(), compensate ? 0.0 : 128.0 / previewTestRate, 0.000001
            );
            source.getNextAudioBlock({&output, 0, previewTestBlockSize});
            expectWithinAbsoluteError(
                source.getCurrentPosition(), (compensate ? 61.0 : 192.0) / previewTestRate, 0.000001
            );
            source.stop();
            expect(!source.isPlaying());
        }
    }

    /// Checks wet-only input gain, serial ordering, and direct mutable processor edits without a restart.
    void testGainAndEdits()
    {
        beginTest("Dry bypass ignores custom gain and live parameter edits affect the next rendered block");
        juce::AudioBuffer<float> audio(1, 1024);
        juce::FloatVectorOperations::fill(audio.getWritePointer(0), 0.5f, audio.getNumSamples());
        juce::MemoryAudioSource input(audio, false);
        PreviewTestPlugin first;
        PreviewTestPlugin second;
        first.getSettings().gain = 2.0f;
        first.getSettings().offset = 0.1f;
        second.getSettings().gain = 0.5f;
        second.getSettings().offset = 0.2f;
        first.prepareToPlay(previewTestRate, previewTestBlockSize);
        second.prepareToPlay(previewTestRate, previewTestBlockSize);
        PluginPreviewSource source;
        source.setInput(&input, 1024.0 / previewTestRate, 1);
        source.setChain({}, plugin_processing::ChainPreparation(), false, true);
        source.setInputGain(2.0f, true);
        source.prepareToPlay(previewTestBlockSize, previewTestRate);
        source.start();
        juce::AudioBuffer<float> output(2, 128);
        source.getNextAudioBlock({&output, 0, 128});
        expectWithinAbsoluteError(output.getSample(0, 127), 0.5f, 0.00001f);
        source.stop();
        input.setNextReadPosition(0);
        source.setChain({&first, &second}, previewPreparation({&first, &second}), true, true);
        source.setPosition(0.0);
        source.start();
        source.getNextAudioBlock({&output, 0, 128});
        expectWithinAbsoluteError(output.getSample(0, 127), 1.25f, 0.00001f);
        expectWithinAbsoluteError(output.getSample(1, 127), 1.25f, 0.00001f);
        second.getSettings().gain = 0.25f;
        source.getNextAudioBlock({&output, 0, 128});
        expect(source.isPlaying());
        expectWithinAbsoluteError(output.getSample(0, 127), 0.725f, 0.00001f);
        source.stop();
        input.setNextReadPosition(0);
        source.setChain({}, plugin_processing::ChainPreparation(), true, true);
        source.setPosition(0.0);
        source.start();
        source.getNextAudioBlock({&output, 0, 128});
        expectWithinAbsoluteError(output.getSample(0, 127), 1.0f, 0.00001f);
    }

    /// Drains the delayed final sample and echo, then clears effect history on a seek/restart.
    void testTailAndReset()
    {
        beginTest("EOF drains latency and the reported tail, and restarting clears the old tail");
        for (const bool compensate : {false, true}) {
            juce::AudioBuffer<float> audio(2, 256);
            audio.clear();
            audio.setSample(0, 255, 1.0f);
            juce::MemoryAudioSource input(audio, false);
            PreviewTestPlugin plugin;
            plugin.getSettings().delay = 17;
            plugin.getSettings().echo = 23;
            plugin.prepareToPlay(previewTestRate, previewTestBlockSize);
            PluginPreviewSource source;
            source.setInput(&input, 256.0 / previewTestRate, 2);
            source.setChain({&plugin}, previewPreparation({&plugin}), true, compensate);
            source.prepareToPlay(previewTestBlockSize, previewTestRate);
            source.start();
            const auto output = renderPreview(source, 37);
            expect(output.size() >= 296);
            if (output.size() >= 296) {
                expectWithinAbsoluteError(output[272], 1.0f, 0.00001f);
                expectWithinAbsoluteError(output[295], 0.5f, 0.00001f);
            }
            input.setNextReadPosition(0);
            source.setPosition(0.0);
            source.start();
            juce::AudioBuffer<float> fresh(2, 64);
            source.getNextAudioBlock({&fresh, 0, 64});
            expectWithinAbsoluteError(fresh.getMagnitude(0, 64), 0.0f, 0.00001f);
        }
    }

    /// Stops bad output before non-finite values can reach the device.
    void testInvalidOutput()
    {
        beginTest("Non-finite output is an explicit stopped failure");
        juce::AudioBuffer<float> audio(2, 256);
        audio.clear();
        juce::MemoryAudioSource input(audio, false);
        PreviewTestPlugin plugin;
        plugin.getSettings().invalid = true;
        plugin.prepareToPlay(previewTestRate, previewTestBlockSize);
        PluginPreviewSource source;
        source.setInput(&input, 256.0 / previewTestRate, 2);
        source.setChain({&plugin}, previewPreparation({&plugin}), true, true);
        source.prepareToPlay(previewTestBlockSize, previewTestRate);
        source.start();
        juce::AudioBuffer<float> output(2, 64);
        source.getNextAudioBlock({&output, 0, 64});
        expect(source.hasFailed());
        expect(!source.isPlaying());
        expectEquals(output.getMagnitude(0, 64), 0.0f);
    }

    /// Allows live tail changes without a restart, while an explicit stop immediately cancels continuation intent.
    void testStopRampAndTailUpdates()
    {
        beginTest("Tail updates preserve the cursor, and user Stop fades without retaining playing intent");
        juce::AudioBuffer<float> audio(2, 256);
        juce::FloatVectorOperations::fill(audio.getWritePointer(0), 0.5f, 256);
        juce::FloatVectorOperations::fill(audio.getWritePointer(1), 0.5f, 256);
        juce::MemoryAudioSource input(audio, false);
        PluginPreviewSource source;
        source.setInput(&input, 256.0 / previewTestRate, 2);
        source.setChain({}, plugin_processing::ChainPreparation(), true, true);
        source.prepareToPlay(64, previewTestRate);
        source.start();
        juce::AudioBuffer<float> output(2, 64);
        source.getNextAudioBlock({&output, 0, 64});
        const auto position = source.getCurrentPosition();
        source.setTailSamples(23);
        expectWithinAbsoluteError(source.getCurrentPosition(), position, 0.000001);
        expect(source.isPlaying());
        const auto tailOutput = renderPreview(source);
        expectEquals(static_cast<int>(tailOutput.size()), 256);
        if (tailOutput.size() >= 192) {
            expectWithinAbsoluteError(tailOutput[191], 0.5f, 0.00001f);
            expectEquals(tailOutput.back(), 0.0f);
        }
        input.setNextReadPosition(0);
        source.setPosition(0.0);
        source.start();
        source.getNextAudioBlock({&output, 0, 64});
        source.requestStop();
        expect(!source.isPlaying());
        source.getNextAudioBlock({&output, 0, 64});
        expectWithinAbsoluteError(output.getSample(0, 0), 0.5f, 0.00001f);
        expect(output.getSample(0, 63) < 0.01f);
        source.getNextAudioBlock({&output, 0, 64});
        expectEquals(output.getMagnitude(0, 64), 0.0f);
    }

    /// Exercises real JUCE resampling before device-rate processing, including final partial buffers.
    void testResampling()
    {
        beginTest("The audible duration remains in seconds when source and device rates differ");
        juce::AudioBuffer<float> audio(2, 441);
        audio.clear();
        juce::MemoryAudioSource input(audio, false);
        juce::ResamplingAudioSource resampler(&input, false, 2);
        resampler.setResamplingRatio(44100.0 / 48000.0);
        PluginPreviewSource source;
        source.setInput(&resampler, 0.01, 2);
        source.setChain({}, plugin_processing::ChainPreparation(), false, true);
        source.prepareToPlay(64, 48000.0);
        source.start();
        const auto output = renderPreview(source, 37);
        expectEquals(static_cast<int>(output.size()), 481);
        expectWithinAbsoluteError(source.getCurrentPosition(), 0.01, 0.000001);
    }

    /// Uses an isolated settings file to verify unset defaults, persistence, and actual continuation policy.
    void testPreferences()
    {
        beginTest("Preferences round-trip without changing their unset defaults or auto-starting stopped playback");
        const TemporaryDirectory temporary;
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        const auto file = temporary.getChildFile("preview-settings.xml");
        {
            juce::PropertiesFile settings(file, options);
            const auto defaults = loadAudioPreviewPreferences(settings);
            expect(!defaults.pluginChainEnabled);
            expect(defaults.latencyCompensated);
            expect(!defaults.continuePlayback);
            expect(defaults.seratoWaveformColours);
            const AudioPreviewPreferences changed {
                .pluginChainEnabled = true,
                .latencyCompensated = false,
                .continuePlayback = true,
                .seratoWaveformColours = false,
            };
            expect(saveAudioPreviewPreferences(settings, changed));
        }
        {
            const juce::PropertiesFile settings(file, options);
            const auto saved = loadAudioPreviewPreferences(settings);
            expect(saved.pluginChainEnabled);
            expect(!saved.latencyCompensated);
            expect(saved.continuePlayback);
            expect(!saved.seratoWaveformColours);
            expect(shouldContinuePreviewPlayback(saved, true, true));
            expect(!shouldContinuePreviewPlayback(saved, false, true));
            expect(!shouldContinuePreviewPlayback(saved, true, false));
            expect(!shouldContinuePreviewPlayback(AudioPreviewPreferences(), true, true));
        }
    }

    /// Verifies exact display units and the existing finite/infinite tail limit without rendering a long tail.
    void testLatencyFormattingAndTailLimit()
    {
        beginTest("Latency uses processing-rate milliseconds and effect tails retain the batch bound");
        expectEquals(plugin_processing::formatPluginLatency(512, 48000.0), juce::String("512 samples (10.67 ms)"));
        expectEquals(plugin_processing::formatPluginLatency(0, 44100.0), juce::String("0 samples (0.00 ms)"));
        PreviewTestPlugin plugin;
        plugin.getSettings().reportedTail = std::numeric_limits<double>::infinity();
        expectEquals(plugin_processing::getPluginTailSamples(plugin, previewTestRate), juce::int64 {240000});
        plugin.getSettings().reportedTail = 0.001;
        expectEquals(plugin_processing::getPluginTailSamples(plugin, previewTestRate), juce::int64 {8});
    }

    /// Exercises disabled-slot totals and failed loading through the real slot model without opening editors.
    void testUnavailableSlotLatency()
    {
        beginTest("Unavailable enabled slots cannot masquerade as zero latency or lose their saved state");
        const TemporaryDirectory temporary;
        juce::PropertiesFile::Options options;
        options.applicationName = "PreviewChainTests";
        options.filenameSuffix = ".xml";
        options.folderName = temporary.getChildFile("properties").getFullPathName();
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::ApplicationProperties properties;
        properties.setStorageParameters(options);
        auto* settings = properties.getUserSettings();
        expect(settings->getFile().isAChildOf(temporary.getChildFile("properties")));
        juce::PluginDescription description;
        description.name = "Unavailable test effect";
        description.pluginFormatName = "Unavailable test format";
        description.fileOrIdentifier = "not-installed";
        juce::XmlElement chainXml("PLUGIN_CHAIN");
        auto* slotXml = chainXml.createNewChildElement("SLOT");
        slotXml->setAttribute("enabled", 0);
        const juce::MemoryBlock savedState("saved plugin state", 18);
        slotXml->setAttribute("state", juce::Base64::toBase64(savedState.getData(), savedState.getSize()));
        slotXml->addChildElement(description.createXml().release());
        settings->setValue("pluginChain", chainXml.toString());
        juce::CriticalSection renderingLock;
        PluginChain chain(properties, renderingLock);
        expectEquals(chain.getNumSlots(), 1);
        expect(!chain.getSlotLatency(0).has_value());
        expect(chain.getTotalLatency().has_value());
        expectEquals(chain.getTotalLatency().value_or(-1), juce::int64 {0});
        const auto disabled = chain.preparePreview(44100.0, 128, 1, false, false);
        expect(disabled.instances.empty());
        expectWithinAbsoluteError(chain.getPreviewSampleRate(), 44100.0, 0.001);
        chain.setSlotEnabled(0, true);
        expect(!chain.getTotalLatency().has_value());
        const auto unavailable = chain.preparePreview(44100.0, 128, 1);
        expect(unavailable.preparation.errorMessage.isNotEmpty());
        expect(unavailable.instances.empty());
        expect(!chain.getTotalLatency().has_value());
        const auto enabled = chain.getEnabledPlugins();
        expectEquals(static_cast<int>(enabled.size()), 1);
        if (!enabled.empty()) {
            expect(enabled.front().descriptorRef.state == savedState);
        }
        chain.clearChain();
        expectEquals(chain.getNumSlots(), 0);
        expectEquals(chain.getTotalLatency().value_or(-1), juce::int64 {0});
    }
};

/// Creates the device-free live preview suite for explicit test registration.
std::unique_ptr<juce::UnitTest> createPreviewPlaybackTests()
{
    return std::make_unique<PreviewPlaybackTests>();
}
}  // namespace audiobatch::tests
