/// Tests for processFileWithPlugins with small in-process test plugins.
/// Covers mono files through stereo-only and mixed-width chains, undithered 16-bit output without a clip guard,
/// latency compensation, rendering and trimming the tail, per-file state, cancellation, and the output length,
/// and plugins that refuse the channel layout of a file.
/// The coordinator of a processing run and the plugin scanning have their own suites.
/// Replaced files are deleted instead of moved to the system trash, so the tests never touch the user's trash.

#include "analysis/AudioAnalysis.h"
#include "gui/LiveChain.h"
#include "metadata/AudioMetadata.h"
#include "plugins/PluginChainPreparation.h"
#include "plugins/PluginPreviewSource.h"
#include "plugins/PluginProcessing.h"
#include "ProcessingTestPlugins.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <generalencapsulatedobjectframe.h>
#include <limits>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Renders the start of the audio through a live preview of one plugin, prepared like the GUI prepares it,
/// and returns one block of the stereo output, past the fade-in.
juce::AudioBuffer<float> renderPreviewBlock(juce::AudioBuffer<float>& audio, juce::AudioPluginInstance& plugin)
{
    constexpr double rate {48'000.0};
    constexpr int blockSize {256};
    plugin.prepareToPlay(rate, blockSize);
    juce::MemoryAudioSource input(audio, false);
    PluginPreviewSource source;
    source.setInput(&input, audio.getNumSamples() / rate, audio.getNumChannels());
    plugin_processing::ChainPreparation preparation;
    preparation.processChannelCount = 2;
    source.setChain({&plugin}, preparation, true, false);
    source.prepareToPlay(blockSize, rate);
    source.start();
    juce::AudioBuffer<float> output(2, blockSize);
    source.getNextAudioBlock({&output, 0, blockSize});
    return output;
}
}  // namespace

/// Checks plugin processing output against the committed fixtures.
class ProcessingTests final : public juce::UnitTest
{
public:
    ProcessingTests() : juce::UnitTest("Plugin processing", "AudioBatch") {}

    /// Runs the processing checks.
    void runTest() override
    {
        const TemporaryDirectory directory;

        beginTest("A plugin whose process call fails fails the file and leaves the source unchanged");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "process_failure.aif");
            const auto sourceSize = source.getSize();
            FailingProcessPlugin plugin;
            const auto result = process(source, {&plugin});
            expect(result.hasError());
            expectEquals(
                outcomeError(result.outcome),
                utils::format(
                    "{}: The plugin failed to process audio (result {})",
                    plugin.getName(),
                    FailingVST3Client::failedResult
                )
            );
            expectEquals(source.getSize(), sourceSize);

            TestPlugin passing({});
            const std::array<juce::AudioPluginInstance*, 2> chain {&passing, &plugin};
            expectEquals(findFailedProcessCall(chain), 1);
            expectEquals(findFailedProcessCall(std::span(chain).first(1)), -1);
        }

        beginTest("A mono file feeds both inputs of a stereo-only plugin and stays mono");
        {
            const auto source = directory.copyFixture("sine_mono_48000_24.aif");
            TestPlugin plugin({.swapChannels = true});
            const ScopedLogCapture capture;
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(outcomeRecord(result.outcome).channels, 1);
            expectEquals(outcomeRecord(result.outcome).bitsPerSample, 16);
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), fixturePeakDecibels, 0.05);
            expect(outcomeNotice(result.outcome).isEmpty(), outcomeNotice(result.outcome));
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

            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(outcomeRecord(result.outcome).channels, 1);
            // Both stereo channels carry the boosted signal, so their average keeps the full 6 dB boost.
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), fixturePeakDecibels + 6.0, 0.05);
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

            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(outcomeRecord(result.outcome).channels, 1);
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), fixturePeakDecibels - 6.0206, 0.05);
        }

        beginTest("A plugin that produces infinity fails the file and leaves the source unchanged");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "unstable.aif");
            const auto sourceSize = source.getSize();
            const auto sourceModified = source.getLastModificationTime();
            TestPlugin plugin({.producesInfinity = true});
            const auto result = process(source, {&plugin});

            expect(result.hasError());
            expect(std::holds_alternative<OperationFailed>(result.outcome));
            expect(outcomeError(result.outcome).contains("invalid samples"), outcomeError(result.outcome));
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

                expect(!result.hasError(), outcomeError(result.outcome));
                expect(std::holds_alternative<OperationSucceeded>(result.outcome));
                expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), expectedPeak, 0.01);
                expectEquals(plugin.getPreparedBlockSize(), plugin_processing::processingBlockSize);
                expectWithinAbsoluteError(plugin.getLastSampleRate(), 44'100.0, 0.001);
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

            plugin_processing::restoreStateAndPrepare(plugin, savedState, 48'000.0, 512);

            expectEquals(plugin.getPreparedBlockSize(), 512);
            expectWithinAbsoluteError(plugin.getLastSampleRate(), 48'000.0, 0.001);
            expectEquals(plugin.getLifecycle().joinIntoString(","), juce::String("prepare,restore-initialized"));
            expectPreparedGain(plugin, 0.25f);
            plugin_processing::releasePluginResources(chainOf(plugin));
        }

        beginTest("An AU with empty saved state is prepared once and reset without restoring");
        {
            PreparationTestPlugin plugin(true);
            const auto preparation
                = plugin_processing::prepareChain(chainOf(plugin), makeOptions({&plugin}), 2, 44'100.0);

            expect(preparation.errorMessage.isEmpty(), preparation.errorMessage);
            expectEquals(plugin.getLifecycle().joinIntoString(","), juce::String("prepare,reset"));
            expectEquals(plugin.getPreparedBlockSize(), plugin_processing::processingBlockSize);
            expectPreparedGain(plugin, 1.0f);
            plugin_processing::releasePluginResources(chainOf(plugin));
        }

        beginTest("The editor preparation helper restores non-AU state before activation");
        {
            PreparationTestPlugin plugin(false);
            juce::MemoryBlock savedState;
            plugin.getStateInformation(savedState);

            for (int preparationIndex = 0; preparationIndex < 2; ++preparationIndex) {
                plugin.clearLifecycle();
                plugin_processing::restoreStateAndPrepare(plugin, savedState, 48'000.0, 512);

                expectEquals(plugin.getLifecycle().joinIntoString(","), juce::String("restore-inactive,prepare"));
                expectEquals(plugin.getPreparedBlockSize(), 512);
                expectWithinAbsoluteError(plugin.getLastSampleRate(), 48'000.0, 0.001);
                expectPreparedGain(plugin, 0.25f);
                plugin_processing::releasePluginResources(chainOf(plugin));
            }
        }

        beginTest("A prepared chain releases its plugins when it goes out of scope");
        {
            PreparationTestPlugin plugin(true);
            const auto chain = chainOf(plugin);
            const utils::CancellationCheck notCancelled = [] { return false; };
            {
                plugin_processing::PreparedChain preparedChain(chain, notCancelled);
                const auto preparation = preparedChain.prepare(makeOptions({&plugin}), 2, 44'100.0);

                expect(preparation.has_value());
                expect(preparation.has_value() && preparation->errorMessage.isEmpty());
                expectEquals(plugin.getLifecycle().joinIntoString(","), juce::String("prepare,reset"));
            }
            expectEquals(plugin.getLifecycle().joinIntoString(","), juce::String("prepare,reset,release"));
        }

        beginTest("A live slot keeps its processor when moved and releases it before destroying it");
        {
            juce::StringArray events;
            CountingProcessorListener listener;
            std::vector<LiveSlot> slots(2);
            slots.front().attachInstance(std::make_unique<TeardownRecordingPlugin>(events), listener);
            slots.front().prepared = true;
            std::swap(slots.front(), slots.back());

            expect(slots.front().instance == nullptr);
            expect(slots.front().listener == nullptr);
            expect(!slots.front().prepared);
            expect(slots.back().instance != nullptr);
            expect(slots.back().prepared);
            expect(events.isEmpty(), events.joinIntoString(","));

            slots.back().instance->updateHostDisplay();
            expectEquals(listener.changeCount, 1);

            slots.pop_back();
            expectEquals(events.joinIntoString(","), juce::String("release,destroy"));
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

            expect(!firstResult.hasError(), outcomeError(firstResult.outcome));
            expect(!secondResult.hasError(), outcomeError(secondResult.outcome));
            // Without the restore, the second file would start from the gain the first file adapted to.
            expectWithinAbsoluteError(
                peakDecibels(outcomeRecord(secondResult.outcome)),
                peakDecibels(outcomeRecord(firstResult.outcome)),
                0.01
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

            const auto result = processFileWithPlugins(analyzeFile(source), options, chainOf(plugin));

            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(disposedNames.joinIntoString(","), juce::String("in_place.aif"));
            expectEquals(disposedSizes[0], sourceSize);
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), fixturePeakDecibels - 6.0206, 0.05);
        }

        beginTest("A source with more than two channels is refused and left unchanged");
        {
            const auto source = directory.copyFixture("sine_3ch_44100_16.aif", "three_channels.aif");
            const auto sourceSize = source.getSize();
            TestPlugin plugin({.gain = 0.5f});
            auto options = makeOptions({&plugin});
            int disposedCount = 0;
            options.disposeFile = [&disposedCount](const juce::File& file) {
                ++disposedCount;
                return file.deleteFile();
            };

            const auto result = processFileWithPlugins(analyzeFile(source), options, chainOf(plugin));

            expect(result.hasError());
            expect(std::holds_alternative<OperationFailed>(result.outcome));
            expect(outcomeError(result.outcome).contains("3 channels"), outcomeError(result.outcome));
            expectEquals(disposedCount, 0);
            expectEquals(source.getSize(), sourceSize);
        }

        beginTest("A converted source is disposed of once its .aif output is in place");
        {
            const auto source = directory.copyFixture("sine_stereo_48000_24.wav", "converted.wav");
            TestPlugin plugin({});
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), outcomeError(result.outcome));
            expect(std::holds_alternative<OperationSucceeded>(result.outcome));
            expect(!source.exists());
            expectEquals(outcomeRecord(result.outcome).file.getFileName(), juce::String("converted.aif"));
            expectEquals(outcomeRecord(result.outcome).bitsPerSample, 16);
            expectEquals(outcomeRecord(result.outcome).sampleRate, 48'000);
        }

        beginTest("Plugin latency is removed from the start of the output");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "latency.aif");
            const auto sourceLength = analyzeFile(source).lengthInSamples;
            TestPlugin plugin({.latencySamples = 1000});
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(outcomeRecord(result.outcome).lengthInSamples, sourceLength);
            expectWithinAbsoluteError(
                maximumDifference(fixtureFile("sine_stereo_44100_16.aif"), outcomeRecord(result.outcome).file, 512),
                0.0f,
                0.001f
            );
        }

        beginTest("Processing keeps the start of the audio in place through plugin latency");
        {
            for (const auto* fixtureName : {"click_mono_44100_16.wav", "click_mono_44100_320k.mp3"}) {
                const auto source = directory.copyFixture(fixtureName);
                TestPlugin plugin({.latencySamples = 1000});
                const auto result = process(source, {&plugin});

                expect(!result.hasError(), outcomeError(result.outcome));
                expectEquals(outcomeRecord(result.outcome).lengthInSamples, clickFixtureLength);
                expectEquals(peakSamplePosition(outcomeRecord(result.outcome).file), clickFixturePosition);
            }
        }

        beginTest("The tail of a plugin is rendered past the end of the source");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "echo.aif");
            const auto sourceLength = analyzeFile(source).lengthInSamples;
            constexpr int echoSamples = 4410;
            TestPlugin plugin({.echoSamples = echoSamples});
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), outcomeError(result.outcome));
            expect(outcomeRecord(result.outcome).lengthInSamples > sourceLength + echoSamples - 20);
            expect(outcomeRecord(result.outcome).lengthInSamples <= sourceLength + echoSamples);
        }

        beginTest("A silent tail allowance is trimmed back to the source length");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "silent_tail.aif");
            const auto sourceLength = analyzeFile(source).lengthInSamples;
            TestPlugin plugin({.reportedTailSeconds = 0.5});
            const auto result = process(source, {&plugin});

            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(outcomeRecord(result.outcome).lengthInSamples, sourceLength);
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
            expect(!result.hasError(), outcomeError(result.outcome));
            AudioMetadata written;
            expect(readMetadata(outcomeRecord(result.outcome).file, written));
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
                = processFileWithPlugins(analyzeFile(source), makeOptions({&plugin}), chainOf(plugin), [] {
                      return true;
                  });

            expect(result.hasError());
            expect(outcomeError(result.outcome).contains("cancelled"), outcomeError(result.outcome));
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
                = processFileWithPlugins(staleRecord, makeOptions({&plugin}), chainOf(plugin), [] { return true; });

            expect(result.hasError());
            expectEquals(outcomeError(result.outcome), cancelledAnalysis.errorMessage);
            expect(std::holds_alternative<OperationFailed>(result.outcome));
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

            const auto result = processFileWithPlugins(analyzeFile(source), options, chainOf(plugin));

            expect(!result.hasError(), outcomeError(result.outcome));
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), -3.0, 0.05);
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

            const auto result = processFileWithPlugins(analyzeFile(source), options, chainOf(plugin));

            expect(!result.hasError(), outcomeError(result.outcome));
            expectWithinAbsoluteError(outcomeRecord(result.outcome).integratedLufs, -20.0, 0.1);
        }

        beginTest("A custom gain wins over normalization before the chain");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "pre_custom.aif");
            TestPlugin plugin({});
            auto options = makeOptions({&plugin});
            options.normalizeBeforePlugin = AudioNormalizationOptions {};
            auto record = analyzeFile(source);
            record.customGain = Decibels {-6.0f};

            const auto result = processFileWithPlugins(record, options, chainOf(plugin));

            expect(!result.hasError(), outcomeError(result.outcome));
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), fixturePeakDecibels - 6.0, 0.05);
        }

        beginTest("The preview runs a mono-only plugin on a stereo file by folding both channels into it");
        {
            TestPlugin plugin({.acceptsMono = true, .refusesStereo = true});
            expect(!plugin_processing::configurePluginChannels(plugin, 2));
            expect(plugin_processing::configurePreviewChannels(plugin));
            expectEquals(plugin.getMainBusNumInputChannels(), 1);

            juce::AudioBuffer<float> audio(2, 1024);
            juce::FloatVectorOperations::fill(audio.getWritePointer(0), 0.5f, audio.getNumSamples());
            juce::FloatVectorOperations::fill(audio.getWritePointer(1), 0.25f, audio.getNumSamples());
            const auto output = renderPreviewBlock(audio, plugin);
            expectWithinAbsoluteError(output.getSample(0, 255), 0.375f, 0.00001f);
            expectWithinAbsoluteError(output.getSample(1, 255), 0.375f, 0.00001f);
        }

        beginTest("The preview of a mono file is folded back to mono after a stereo plugin, like the written file");
        {
            TestPlugin plugin({.rightGain = 0.0f});
            expect(plugin_processing::configurePreviewChannels(plugin));

            juce::AudioBuffer<float> audio(1, 1024);
            juce::FloatVectorOperations::fill(audio.getWritePointer(0), 0.5f, audio.getNumSamples());
            const auto output = renderPreviewBlock(audio, plugin);
            expectWithinAbsoluteError(output.getSample(0, 255), 0.25f, 0.00001f);
            expectWithinAbsoluteError(output.getSample(1, 255), 0.25f, 0.00001f);
        }

        beginTest("A file the normalization refuses is processed at unity gain with a notice");
        {
            const auto source = directory.copyFixture("silence_stereo_44100_16.wav", "pre_refused.wav");
            TestPlugin plugin({});
            auto options = makeOptions({&plugin});
            options.normalizeBeforePlugin = AudioNormalizationOptions {};
            const auto record = analyzeFile(source);

            const auto result = processFileWithPlugins(record, options, chainOf(plugin));

            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(
                outcomeNotice(result.outcome),
                juce::String("Not normalized before the plugins: File contains no signal that can be normalized")
            );
            expectEquals(outcomeRecord(result.outcome).lengthInSamples, record.lengthInSamples);
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
                expect(!result.hasError(), outcomeError(result.outcome));
                expectEquals(outcomeRecord(result.outcome).bitsPerSample, 16);
                expectEquals(outcomeRecord(result.outcome).sampleRate, original.sampleRate);
                expectEquals(outcomeRecord(result.outcome).channels, original.channels);
                expectEquals(outcomeRecord(result.outcome).lengthInSamples, original.lengthInSamples);
                const auto reader = utils::createAudioReader(outcomeRecord(result.outcome).file);
                expect(reader != nullptr);
                if (reader != nullptr) {
                    expectEquals<juce::uint64>(reader->bitsPerSample, 16);
                    expect(!reader->usesFloatingPointData);
                }
            }
        }

        beginTest("Digital silence stays exactly silent without AudioBatch dithering");
        {
            const auto source = directory.copyFixture("silence_stereo_44100_16.wav");
            TestPlugin plugin({});
            const auto result = process(source, {&plugin});
            expect(!result.hasError(), outcomeError(result.outcome));
            const auto reader = utils::createAudioReader(outcomeRecord(result.outcome).file);
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
            expect(!result.hasError(), outcomeError(result.outcome));
            expect(outcomeNotice(result.outcome).contains("Output clipping"), outcomeNotice(result.outcome));
            const auto reader = utils::createAudioReader(outcomeRecord(result.outcome).file);
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

        beginTest("Output between the largest step and full scale rounds down without reporting clipping");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "half_step.aif");
            TestPlugin plugin({.producesHalfStepBelowFullScale = true});
            const auto result = process(source, {&plugin});
            expect(!result.hasError(), outcomeError(result.outcome));
            expect(outcomeNotice(result.outcome).isEmpty(), outcomeNotice(result.outcome));
            expectWithinAbsoluteError(
                utils::peakMagnitude(outcomeRecord(result.outcome).overallPeak), 32'767.0f / 32'768.0f, 0.0f
            );
        }

        beginTest("Float overs between plugins do not report clipping when the final output is in range");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "float_overs.aif");
            TestPlugin boost({.gain = 4.0f});
            TestPlugin attenuate({.gain = 0.25f});
            const ScopedLogCapture capture;
            const auto result = process(source, {&boost, &attenuate});

            expect(!result.hasError(), outcomeError(result.outcome));
            expect(outcomeNotice(result.outcome).isEmpty(), outcomeNotice(result.outcome));
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), fixturePeakDecibels, 0.05);
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

            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(outcomeRecord(result.outcome).channels, 2);
            expectEquals(outcomeRecord(result.outcome).bitsPerSample, 16);
            expectEquals(outcomeRecord(result.outcome).lengthInSamples, sourceLength);
            expect(
                peakDecibels(outcomeRecord(result.outcome)) <= 0.0,
                juce::String(peakDecibels(outcomeRecord(result.outcome)))
            );
            expect(
                peakDecibels(outcomeRecord(result.outcome)) > -0.1,
                juce::String(peakDecibels(outcomeRecord(result.outcome)))
            );
            expect(outcomeNotice(result.outcome).contains("Output clipping"), outcomeNotice(result.outcome));
            expect(std::ranges::any_of(capture.getMessages(), [&](const auto& message) {
                return message.endsWith(
                    utils::format(
                        "[WARN]: Plugin processing warning: {}, {}",
                        outcomeRecord(result.outcome).file.getFullPathName().quoted(),
                        outcomeNotice(result.outcome)
                    )
                );
            }));
        }

        testChannelLayoutRefusal(directory);
        testLateCancellation();
    }

private:
    /// Checks that a plugin that refuses the channel layout of a file fails it before any audio is processed,
    /// and that the plugins prepared before it in the chain are released again.
    void testChannelLayoutRefusal(const TemporaryDirectory& directory)
    {
        beginTest("A mono-only plugin refuses a stereo file, which is left unchanged without an output");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.wav", "mono_only.wav");
            juce::MemoryBlock originalBytes;
            expect(source.loadFileAsData(originalBytes));
            TestPlugin monoOnly({.acceptsMono = true, .refusesStereo = true});
            const auto result = process(source, {&monoOnly});

            expect(result.hasError());
            expectEquals(
                outcomeError(result.outcome),
                utils::format(
                    "Plugin {} does not support the file's channel layout (2 channels)", monoOnly.getName().quoted()
                )
            );
            expectEquals(monoOnly.getWidestBufferChannels(), 0);
            juce::MemoryBlock remainingBytes;
            expect(source.loadFileAsData(remainingBytes));
            expect(remainingBytes == originalBytes);
            expect(!getProcessingOutputFile(source).exists());
        }

        beginTest("A refusal later in the chain releases the plugins already prepared for the file");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "refused_later.aif");
            const auto sourceSize = source.getSize();
            PreparationTestPlugin prepared(false);
            TestPlugin monoOnly({.acceptsMono = true, .refusesStereo = true});
            const auto result = process(source, {&prepared, &monoOnly});

            expect(result.hasError());
            expect(outcomeError(result.outcome).contains("channel layout"), outcomeError(result.outcome));
            expectEquals(prepared.getLifecycle().joinIntoString(","), juce::String("prepare,reset,release"));
            expectEquals(prepared.getWidestBufferChannels(), 0);
            expectEquals(source.getSize(), sourceSize);
        }
    }

    /// Counts the cancellation checks of a complete run of a file through a plugin,
    /// then cancels the same run at its first, a middle, and its last check.
    /// The last check comes after the output was rendered and checked, just before it would replace the source,
    /// so it covers the cleanup of a finished temporary output.
    void testLateCancellation()
    {
        beginTest("Cancelling processing at any later check leaves the source unchanged and removes temporary files");
        std::vector<int> allowedCheckCounts;
        {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "complete.aif");
            TestPlugin plugin({.gain = 0.5f});
            const auto checks = std::make_shared<std::atomic<int>>(0);
            const auto result = processFileWithPlugins(
                analyzeFile(source),
                makeOptions({&plugin}),
                chainOf(plugin),
                cancelAfterChecks(std::numeric_limits<int>::max(), checks)
            );
            expect(!result.hasError(), outcomeError(result.outcome));
            expect(checks->load() > 1, "processing must check for cancellation more than once");
            allowedCheckCounts = {0, checks->load() / 2, checks->load() - 1};
        }

        for (const auto allowedChecks : allowedCheckCounts) {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "late_cancel.aif");
            juce::MemoryBlock originalBytes;
            expect(source.loadFileAsData(originalBytes));
            TestPlugin plugin({.gain = 0.5f});
            auto options = makeOptions({&plugin});
            int disposedCount = 0;
            options.disposeFile = [&disposedCount](const juce::File& file) {
                ++disposedCount;
                return file.deleteFile();
            };

            const auto checks = std::make_shared<std::atomic<int>>(0);
            const auto result = processFileWithPlugins(
                analyzeFile(source), options, chainOf(plugin), cancelAfterChecks(allowedChecks, checks)
            );
            const auto context = utils::format("cancelled after {} checks", allowedChecks);
            expect(result.hasError(), context);
            expect(outcomeError(result.outcome).contains("cancelled"), outcomeError(result.outcome));
            // Releasing the chain may look at the check again, so only the first cancelling call is certain.
            expect(checks->load() > allowedChecks, context);
            expectEquals(disposedCount, 0, context);
            juce::MemoryBlock remainingBytes;
            expect(source.loadFileAsData(remainingBytes));
            expect(remainingBytes == originalBytes, context);
            expectEquals(source.getParentDirectory().findChildFiles(juce::File::findFiles, false).size(), 1, context);
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
        return makeTestPluginOptions(chain, states);
    }
};

std::unique_ptr<juce::UnitTest> createProcessingTests()
{
    return std::make_unique<ProcessingTests>();
}
}  // namespace audiobatch::tests
