/// Device-free regression tests for the live preview renderer, cue positioning, persisted preferences,
/// and the output device status shown in the status bar.
/// Synthetic processors expose latency, gain edits, ordering, EOF tails, and invalid output.
/// The committed silence fixture exercises deferred playback without opening hardware or third-party plugins.

#include "gui/AudioPreviewPlayer.h"
#include "gui/components/AudioSettingsComponent.h"
#include "gui/components/StatusBar.h"
#include "gui/GuiLayout.h"
#include "gui/LiveChain.h"
#include "plugins/PluginLibrary.h"
#include "plugins/PluginPreviewSource.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <algorithm>
#include <cmath>
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

/// Stores the application settings of a test in the given temporary directory, so the user's settings stay untouched.
void useTemporarySettings(juce::ApplicationProperties& properties, const TemporaryDirectory& temporary)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "PreviewChainTests";
    options.filenameSuffix = ".xml";
    options.folderName = temporary.getChildFile("properties").getFullPathName();
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    properties.setStorageParameters(options);
}

/// Stores a chain of disabled slots for plugins that are not installed, one slot per name, each with the given state.
void storeUnavailableChain(
    juce::PropertiesFile& settings,
    const juce::StringArray& pluginNames,
    const juce::MemoryBlock& savedState
)
{
    juce::XmlElement chainXml("PLUGIN_CHAIN");
    for (const auto& name : pluginNames) {
        juce::PluginDescription description;
        description.name = name;
        description.pluginFormatName = "Unavailable test format";
        description.fileOrIdentifier = "not-installed";
        auto* slotXml = chainXml.createNewChildElement("SLOT");
        slotXml->setAttribute("enabled", 0);
        slotXml->setAttribute("state", juce::Base64::toBase64(savedState.getData(), savedState.getSize()));
        slotXml->addChildElement(description.createXml().release());
    }
    settings.setValue("pluginChain", chainXml.toString());
}

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
        testCueStartPosition();
        testInitialPosition();
        testDeviceOwnership();
        testOutputStatusFormatting();
        testPreferences();
        testLoudnessAxisIntervals();
        testLatencyFormattingAndTailLimit();
        testUnavailableSlotLatency();
        testSlotIds();
    }

private:
    /// Checks numeric tick layout without constructing or rendering a GUI component.
    void testLoudnessAxisIntervals()
    {
        beginTest("Loudness ticks use even intervals instead of independently rounded labels");
        expectEquals(layout::waveform::loudnessTickInterval(-24, -4, 230), 4);
        expectEquals(layout::waveform::loudnessTickInterval(-23, -5, 230), 4);
        expectEquals(layout::waveform::loudnessTickInterval(-24, -4, 48), 10);
        expectEquals(layout::waveform::loudnessTickInterval(-60, 6, 48), 20);
        expectEquals(layout::waveform::loudnessTickInterval(-6, -4, 230), 1);
        expectEquals(layout::waveform::loudnessTickInterval(5, 6, 48), 1);

        beginTest("Every supported loudness range has uniform in-range ticks at compact and expanded heights");
        for (int minimum = -60; minimum < 6; ++minimum) {
            for (int maximum = minimum + 1; maximum <= 6; ++maximum) {
                for (const auto height : {48, 64, 128, 230, 480}) {
                    const auto interval = layout::waveform::loudnessTickInterval(minimum, maximum, height);
                    const auto first = static_cast<int>(std::ceil(static_cast<double>(minimum) / interval)) * interval;
                    expect(first >= minimum && first <= maximum);
                    expect(interval <= 20);
                    expect(interval == 1 || interval % 2 == 0);
                    if (maximum - minimum > interval) {
                        expect(
                            static_cast<double>(interval * height) / (maximum - minimum)
                            >= static_cast<double>(layout::waveform::loudnessFontHeight)
                        );
                    }
                    int previous = first - interval;
                    for (int value = first; value <= maximum; value += interval) {
                        expectEquals(value - previous, interval);
                        expect(value >= minimum);
                        expect(interval == 1 || value % 2 == 0);
                        previous = value;
                    }
                }
            }
        }
    }

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
        source.setInputGain(LinearGain {2.0f}, true);
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
        resampler.setResamplingRatio(44'100.0 / 48'000.0);
        PluginPreviewSource source;
        source.setInput(&resampler, 0.01, 2);
        source.setChain({}, plugin_processing::ChainPreparation(), false, true);
        source.prepareToPlay(64, 48'000.0);
        source.start();
        const auto output = renderPreview(source, 37);
        expectEquals(static_cast<int>(output.size()), 481);
        expectWithinAbsoluteError(source.getCurrentPosition(), 0.01, 0.000001);
    }

    /// Selects chronological cues rather than hot-cue indices and rejects missing or unsafe start positions.
    void testCueStartPosition()
    {
        const AudioPreviewPreferences enabled {.playFromFirstCuePoint = true};
        serato::Markers markers;

        beginTest("Cue starts require an enabled preference and nonempty markers");
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, nullptr, 10.0), 0.0, 0.000001);
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 0.0, 0.000001);
        markers.cues.push_back({.positionMilliseconds = 1250, .index = 7, .colour = {}, .name = {}});
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 1.25, 0.000001);
        expectWithinAbsoluteError(
            getPreviewStartPositionSeconds(AudioPreviewPreferences(), &markers, 10.0), 0.0, 0.000001
        );

        beginTest("Unsorted cues use the earliest time even when another cue has index zero or is after halfway");
        markers.cues.insert(markers.cues.begin(), {.positionMilliseconds = 7000, .index = 0, .colour = {}, .name = {}});
        markers.cues.push_back({.positionMilliseconds = 2500, .index = 1, .colour = {}, .name = {}});
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 1.25, 0.000001);

        beginTest("A cue exactly halfway is accepted but one millisecond later falls back to zero");
        markers.cues = {{.positionMilliseconds = 5000, .index = 0, .colour = {}, .name = {}}};
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 5.0, 0.000001);
        markers.cues.front().positionMilliseconds = 5001;
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 0.0, 0.000001);

        beginTest("Nonpositive and nonfinite durations cannot select a cue start");
        markers.cues.front().positionMilliseconds = 1250;
        for (const auto duration :
             {
                 0.0,
                 -1.0,
                 std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity(),
                 -std::numeric_limits<double>::infinity(),
             })
        {
            expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, duration), 0.0, 0.000001);
        }

        beginTest("A zero-position cue remains at the beginning even with later cues");
        markers.cues.push_back({.positionMilliseconds = 0, .index = 7, .colour = {}, .name = {}});
        expectWithinAbsoluteError(getPreviewStartPositionSeconds(enabled, &markers, 10.0), 0.0, 0.000001);
    }

    /// Exercises deferred cue resolution on one player with a simulated output driver.
    /// Each load establishes a fresh wait so completion, cancellation, release, and replacement remain independent.
    void testInitialPosition()
    {
        const juce::URL silenceUrl(fixtureFile("silence_stereo_44100_16.wav"));
        TestAudioDeviceState device;
        AudioPreviewPlayer player;
        installTestAudioDevice(player.getDeviceManager(), device);

        beginTest("Resolving a stopped preview positions it once without starting playback");
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
        expect(player.load(silenceUrl, true));
        expect(player.hasLoadedSource());
        expect(!player.isPlaying());
        player.completeInitialPosition(0.25);
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);
        expect(!player.isPlaying());
        player.completeInitialPosition(0.5);
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);
        expect(!player.isPlaying());

        beginTest("A pending start is logically playing at zero until its initial position is resolved");
        expect(player.load(silenceUrl, true));
        player.start();
        expect(player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.0, 0.000001);
        player.completeInitialPosition(0.25);
        expect(player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);
        player.completeInitialPosition(0.5);
        expect(player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);

        beginTest("Stopping during the initial wait cancels the pending start but still allows cue positioning");
        expect(player.load(silenceUrl, true));
        player.start();
        expect(player.isPlaying());
        player.stop();
        expect(!player.isPlaying());
        player.completeInitialPosition(0.25);
        expect(!player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);
        player.start();
        expect(player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.25, 0.000001);

        beginTest("An explicit seek cancels the initial wait while preserving a pending start");
        expect(player.load(silenceUrl, true));
        player.start();
        player.setPosition(0.375);
        expect(player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.375, 0.000001);
        player.completeInitialPosition(0.25);
        expect(player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.375, 0.000001);

        beginTest("An explicit seek cancels a stopped preview's initial wait without starting it");
        expect(player.load(silenceUrl, true));
        player.setPosition(0.375);
        expect(!player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.375, 0.000001);
        player.completeInitialPosition(0.25);
        expect(!player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.375, 0.000001);

        beginTest("Release clears a pending start and ignores late initial-position completion");
        expect(player.load(silenceUrl, true));
        player.start();
        expect(player.isPlaying());
        player.release();
        expect(!player.hasLoadedSource());
        expect(!player.getLoadedDurationSeconds().has_value());
        expect(!player.isPlaying());
        player.completeInitialPosition(0.25);
        player.start();
        expect(!player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.0, 0.000001);

        beginTest("A replacement load defaults to no wait and discards the previous pending start");
        expect(player.load(silenceUrl, true));
        expect(!player.isPlaying());
        player.start();
        expect(player.isPlaying());
        expect(player.load(silenceUrl));
        expect(player.hasLoadedSource());
        expect(!player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.0, 0.000001);
        player.completeInitialPosition(0.25);
        expect(!player.isPlaying());
        expectWithinAbsoluteError(player.getCurrentPosition(), 0.0, 0.000001);
        player.start();
        expect(player.isPlaying());
        player.stop();
        expect(!player.isPlaying());
        renderTestAudioDevice(device);
        player.updatePlaybackStatus();
        expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
    }

    /// Exercises the real device manager and player lifetime without native audio hardware.
    void testDeviceOwnership()
    {
        const juce::URL audioUrl(fixtureFile("silence_stereo_44100_16.wav"));
        TestAudioDeviceState device;
        {
            AudioPreviewPlayer player;
            installTestAudioDevice(player.getDeviceManager(), device);
            beginTest("Startup and file selection leave the audio device closed");
            expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
            expect(player.load(audioUrl));
            player.updatePlaybackStatus();
            expectEquals(device.opens, 0);
            expect(!player.getOutputStatus().open);
            expectEquals(formatAudioOutputStatus(player.getOutputStatus()), juce::String("Default output"));

            beginTest("Playback opens only output channels and stopping releases the driver after its fade");
            player.start();
            expect(player.isPlaying());
            expectEquals(device.opens, 1);
            expectEquals(device.inputChannels, 0);
            expectEquals(device.outputChannels, 2);
            expect(player.getOutputStatus().open);
            expectEquals(
                formatAudioOutputStatus(player.getOutputStatus()), juce::String("Test output, 48 kHz, 512 samples")
            );
            renderTestAudioDevice(device);
            player.stop();
            renderTestAudioDevice(device);
            player.updatePlaybackStatus();
            expect(!player.isPlaying());
            expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
            expectEquals(device.closes, 1);

            beginTest("A closed output device keeps reporting its last configuration");
            expect(!player.getOutputStatus().open);
            expectEquals(
                formatAudioOutputStatus(player.getOutputStatus()), juce::String("Test output, 48 kHz, 512 samples")
            );

            beginTest("Suspension releases immediately and does not restart until an explicit play");
            const auto position = player.getCurrentPosition();
            player.start();
            expectEquals(device.opens, 2);
            player.setApplicationActive(false);
            expect(!player.isPlaying());
            expect(player.hasLoadedSource());
            expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
            expectEquals(device.closes, 2);
            expectWithinAbsoluteError(player.getCurrentPosition(), position, 0.000001);
            player.updatePlaybackStatus();
            expectEquals(device.opens, 2);
            player.setApplicationActive(true);
            expect(!player.isPlaying());
            expectEquals(device.opens, 2);

            beginTest("Audio Settings leases the device only while the application is focused");
            player.setDeviceSettingsActive(true);
            expect(player.getDeviceManager().getCurrentAudioDevice() != nullptr);
            expect(!player.isPlaying());
            player.updatePlaybackStatus();
            expect(player.getDeviceManager().getCurrentAudioDevice() != nullptr);
            player.setApplicationActive(false);
            expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
            player.setApplicationActive(true);
            expect(player.getDeviceManager().getCurrentAudioDevice() != nullptr);
            expect(!player.isPlaying());
            player.setDeviceSettingsActive(false);
            expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);

            beginTest("EOF releases the output device even without waveform repainting");
            player.setPosition(0.999);
            player.start();
            renderTestAudioDevice(device);
            player.updatePlaybackStatus();
            expect(!player.isPlaying());
            expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
            expectEquals(device.opens, device.closes);

            beginTest("An unavailable device reports its error and does not leave logical playback active");
            juce::String error;
            player.setErrorCallback([&](const juce::String& message) { error = message; });
            device.openError = "Test output is in use by another application";
            player.start();
            expect(!player.isPlaying());
            expectEquals(error, device.openError);
            expect(player.getDeviceManager().getCurrentAudioDevice() == nullptr);
            device.openError.clear();
            player.setErrorCallback({});

            beginTest("Shutdown balances all device opens even if playback is still active");
            player.start();
            expect(player.isPlaying());
            expectEquals(device.opens, device.closes + 1);
        }
        expectEquals(device.opens, device.closes);
        expect(device.callback == nullptr);
    }

    /// Checks the status bar text of the output device for fractional rates and values that are not known yet.
    void testOutputStatusFormatting()
    {
        beginTest("Output status text leaves out unknown values and keeps fractional sample rates");
        expectEquals(
            formatAudioOutputStatus(
                {.deviceName = "Speakers", .sampleRate = 44'100.0, .bufferSize = 256, .open = true}
            ),
            juce::String("Speakers, 44.1 kHz, 256 samples")
        );
        expectEquals(
            formatAudioOutputStatus({.deviceName = "Speakers", .sampleRate = 0.0, .bufferSize = 0, .open = false}),
            juce::String("Speakers")
        );
        expectEquals(
            formatAudioOutputStatus({.deviceName = {}, .sampleRate = 96'000.0, .bufferSize = 0, .open = false}),
            juce::String("Default output, 96 kHz")
        );
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
            expect(!defaults.playFromFirstCuePoint);
            expect(!AudioPreviewPreferences().playFromFirstCuePoint);
            expect(defaults.seratoWaveformColours);
            expect(!defaults.loudnessGraphEnabled);
            expectEquals(defaults.loudnessGraphMinimum, -24.0);
            expectEquals(defaults.loudnessGraphMaximum, -4.0);
            const AudioPreviewPreferences changed {
                .pluginChainEnabled = true,
                .latencyCompensated = false,
                .continuePlayback = true,
                .playFromFirstCuePoint = true,
                .seratoWaveformColours = false,
                .loudnessGraphEnabled = true,
                .loudnessGraphMinimum = -32.0,
                .loudnessGraphMaximum = -2.0,
            };
            expect(saveAudioPreviewPreferences(settings, changed));
        }
        {
            juce::PropertiesFile settings(file, options);
            const auto saved = loadAudioPreviewPreferences(settings);
            expect(saved.pluginChainEnabled);
            expect(!saved.latencyCompensated);
            expect(saved.continuePlayback);
            expect(saved.playFromFirstCuePoint);
            expect(!saved.seratoWaveformColours);
            expect(saved.loudnessGraphEnabled);
            expectEquals(saved.loudnessGraphMinimum, -32.0);
            expectEquals(saved.loudnessGraphMaximum, -2.0);
            expect(shouldContinuePreviewPlayback(saved, true, true));
            expect(!shouldContinuePreviewPlayback(saved, false, true));
            expect(!shouldContinuePreviewPlayback(saved, true, false));
            expect(!shouldContinuePreviewPlayback(AudioPreviewPreferences(), true, true));
            auto disabled = saved;
            disabled.playFromFirstCuePoint = false;
            disabled.loudnessGraphEnabled = false;
            expect(saveAudioPreviewPreferences(settings, disabled));
        }
        {
            const juce::PropertiesFile settings(file, options);
            expect(!loadAudioPreviewPreferences(settings).playFromFirstCuePoint);
            const auto disabled = loadAudioPreviewPreferences(settings);
            expect(!disabled.loudnessGraphEnabled);
            expectEquals(disabled.loudnessGraphMinimum, -32.0);
            expectEquals(disabled.loudnessGraphMaximum, -2.0);
        }

        beginTest("Invalid loudness axis settings are repaired without changing other preferences");
        expect(isValidLoudnessGraphRange(-60.0, 6.0));
        expect(isValidLoudnessGraphRange(5.0, 6.0));
        expect(!isValidLoudnessGraphRange(-4.0, -24.0));
        expect(!isValidLoudnessGraphRange(-4.0, -4.0));
        expect(!isValidLoudnessGraphRange(-4.0, -3.5));
        expect(!isValidLoudnessGraphRange(-24.5, -4.0));
        expect(!isValidLoudnessGraphRange(std::nextafter(-24.0, -23.0), -4.0));
        expect(!isValidLoudnessGraphRange(-24.0, -3.5));
        expect(!isValidLoudnessGraphRange(-61.0, -4.0));
        expect(!isValidLoudnessGraphRange(-24.0, 7.0));
        expect(!isValidLoudnessGraphRange(std::numeric_limits<double>::quiet_NaN(), -4.0));
        expect(!isValidLoudnessGraphRange(-24.0, std::numeric_limits<double>::infinity()));
        for (const auto* invalid : {"nan", "-4junk", "", "-2", "-999", "-61", "-24.5"}) {
            juce::PropertiesFile settings(file, options);
            settings.setValue("loudnessGraphMinimum", invalid);
            settings.setValue("loudnessGraphMaximum", "-4");
            const auto repaired = loadAudioPreviewPreferences(settings);
            expectEquals(repaired.loudnessGraphMinimum, -24.0);
            expectEquals(repaired.loudnessGraphMaximum, -4.0);
            expect(repaired.pluginChainEnabled);
            expect(saveAudioPreviewPreferences(settings, repaired));
            auto rejected = repaired;
            rejected.loudnessGraphMaximum = -40.0;
            expect(!saveAudioPreviewPreferences(settings, rejected));
            expectEquals(loadAudioPreviewPreferences(settings).loudnessGraphMaximum, -4.0);
        }

        beginTest("Integer loudness limits round trip at the supported endpoints");
        {
            juce::PropertiesFile settings(file, options);
            auto endpoints = loadAudioPreviewPreferences(settings);
            endpoints.loudnessGraphMinimum = -60.0;
            endpoints.loudnessGraphMaximum = 6.0;
            expect(saveAudioPreviewPreferences(settings, endpoints));
        }
        {
            juce::PropertiesFile settings(file, options);
            const auto saved = loadAudioPreviewPreferences(settings);
            expectEquals(saved.loudnessGraphMinimum, -60.0);
            expectEquals(saved.loudnessGraphMaximum, 6.0);
            auto fractional = saved;
            fractional.loudnessGraphMaximum = 5.5;
            expect(!saveAudioPreviewPreferences(settings, fractional));
            expectEquals(loadAudioPreviewPreferences(settings).loudnessGraphMaximum, 6.0);

            beginTest("Saved whole-number decimals remain compatible with integer-only limits");
            settings.setValue("loudnessGraphMinimum", "-60.0");
            settings.setValue("loudnessGraphMaximum", "6.0");
            const auto legacy = loadAudioPreviewPreferences(settings);
            expectEquals(legacy.loudnessGraphMinimum, -60.0);
            expectEquals(legacy.loudnessGraphMaximum, 6.0);
            settings.setValue("loudnessGraphMaximum", "7");
            const auto repaired = loadAudioPreviewPreferences(settings);
            expectEquals(repaired.loudnessGraphMinimum, -24.0);
            expectEquals(repaired.loudnessGraphMaximum, -4.0);
        }

        beginTest("Loudness range editing accepts at most two digits and never strips pasted decimals");
        for (const auto* input : {"", "-", "0", "6", "-6", "-60", "60", "-99"}) {
            expect(isValidLoudnessGraphLimitInput(input), input);
        }
        for (const auto* input : {"123", "-123", "006", "6.0", "-2.5", "1-2", "--6", "nan", "6 "}) {
            expect(!isValidLoudnessGraphLimitInput(input), input);
        }
    }

    /// Verifies exact display units and the existing finite/infinite tail limit without rendering a long tail.
    void testLatencyFormattingAndTailLimit()
    {
        beginTest("Latency uses processing-rate milliseconds and effect tails retain the batch bound");
        expectEquals(plugin_processing::formatPluginLatency(512, 48'000.0), juce::String("512 samples (10.67 ms)"));
        expectEquals(plugin_processing::formatPluginLatency(0, 44'100.0), juce::String("0 samples (0.00 ms)"));
        expectEquals(formatPluginPreviewRate(48'000.0, true), juce::String("Preview rate: 48000 Hz."));
        expectEquals(formatPluginPreviewRate(44'100.0, true), juce::String("Preview rate: 44100 Hz."));
        expectEquals(
            formatPluginPreviewRate(48'000.0, false), juce::String("No audio device. Inspection rate: 48000 Hz.")
        );
        expectEquals(
            formatPluginPreviewRate(44'100.0, false), juce::String("No audio device. Inspection rate: 44100 Hz.")
        );
        PreviewTestPlugin plugin;
        plugin.getSettings().reportedTail = std::numeric_limits<double>::infinity();
        expectEquals(plugin_processing::getPluginTailSamples(plugin, previewTestRate), juce::int64 {160'000});
        plugin.getSettings().reportedTail = 0.001;
        expectEquals(plugin_processing::getPluginTailSamples(plugin, previewTestRate), juce::int64 {8});
    }

    /// Exercises disabled-slot totals and failed loading through the real slot model without opening editors.
    void testUnavailableSlotLatency()
    {
        beginTest("Unavailable enabled slots cannot masquerade as zero latency or lose their saved state");
        const TemporaryDirectory temporary;
        juce::ApplicationProperties properties;
        useTemporarySettings(properties, temporary);
        auto* settings = properties.getUserSettings();
        expect(settings->getFile().isAChildOf(temporary.getChildFile("properties")));
        const juce::MemoryBlock savedState("saved plugin state", 18);
        storeUnavailableChain(*settings, {"Unavailable test effect"}, savedState);
        juce::CriticalSection renderingLock;
        juce::AudioDeviceManager previewDeviceManager;
        PluginLibrary library(properties);
        LiveChain chain(library, properties, renderingLock, previewDeviceManager);
        expect(!chain.hasOpenPreviewDevice());
        expectEquals(chain.getNumSlots(), 1);
        const auto slotIds = chain.getSlotIds();
        const auto slotId = slotIds.empty() ? PluginSlotId {} : slotIds.front();
        expect(!chain.getSlotLatency(slotId).has_value());
        expect(chain.getTotalLatency().has_value());
        expectEquals(chain.getTotalLatency().value_or(-1), juce::int64 {0});
        const auto disabled = chain.preparePreview(44'100.0, 128, 1, false, false);
        expect(disabled.instances.empty());
        expectWithinAbsoluteError(chain.getPreviewSampleRate(), 44'100.0, 0.001);
        chain.setSlotEnabled(slotId, true);
        expect(!chain.getTotalLatency().has_value());
        const auto unavailable = chain.preparePreview(44'100.0, 128, 1);
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

    /// Checks that slot ids follow their slots when the chain is reordered, and stop matching once a slot is removed.
    void testSlotIds()
    {
        beginTest("Slot ids follow their slots through moves and removals");
        const TemporaryDirectory temporary;
        juce::ApplicationProperties properties;
        useTemporarySettings(properties, temporary);
        storeUnavailableChain(*properties.getUserSettings(), {"First", "Second", "Third"}, {});
        juce::CriticalSection renderingLock;
        juce::AudioDeviceManager previewDeviceManager;
        PluginLibrary library(properties);
        LiveChain chain(library, properties, renderingLock, previewDeviceManager);

        const auto slotIds = chain.getSlotIds();
        expectEquals(static_cast<int>(slotIds.size()), 3);
        if (slotIds.size() != 3) {
            return;
        }
        expect(slotIds[0] != slotIds[1] && slotIds[1] != slotIds[2] && slotIds[0] != slotIds[2]);

        chain.moveSlot(slotIds[0], 2);
        expect(chain.getSlotIds() == std::vector {slotIds[1], slotIds[2], slotIds[0]});
        expectEquals(chain.getSlotIndex(slotIds[0]).value_or(-1), 2);
        expectEquals(chain.getSlotDescription(slotIds[0]).name, juce::String("First"));

        chain.removeSlot(slotIds[2]);
        expect(!chain.getSlotIndex(slotIds[2]).has_value());
        expect(chain.getSlotDescription(slotIds[2]).name.isEmpty());
        expect(chain.getSlotIds() == std::vector {slotIds[1], slotIds[0]});

        chain.setSlotEnabled(slotIds[0], true);
        expect(chain.isSlotEnabled(slotIds[0]));
        expect(!chain.isSlotEnabled(slotIds[1]));

        // Acting on a removed slot changes nothing.
        chain.moveSlot(slotIds[2], 0);
        chain.removeSlot(slotIds[2]);
        expect(chain.getSlotIds() == std::vector {slotIds[1], slotIds[0]});
    }
};

/// Creates the device-free live preview suite for explicit test registration.
std::unique_ptr<juce::UnitTest> createPreviewPlaybackTests()
{
    return std::make_unique<PreviewPlaybackTests>();
}
}  // namespace audiobatch::tests
