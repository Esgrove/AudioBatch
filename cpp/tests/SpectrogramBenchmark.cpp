/// Measures source-rate spectral construction and optional-artifact backfill without audio hardware.
/// Repeated bounded PCM blocks avoid a full-track allocation.
/// Cache work uses only copied fixtures and scratch SQLite.
/// Each measurement has one untimed warmup and five timed runs, with exact artifact checks outside timing.

#include "SpectrogramBenchmark.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/SpectrogramBuilder.h"
#include "analysis/SpectrogramData.h"
#include "gui/PreviewArtifactDecoder.h"
#include "TestHelpers.h"
#include "utils/ConsolePrint.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"
#include <fmt/base.h>
#include <string_view>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>

namespace audiobatch::tests
{
namespace
{
constexpr int SAMPLE_RATE {48'000};
constexpr int CHANNELS {2};
constexpr int SECONDS {30};
constexpr int TOTAL_FRAMES {SAMPLE_RATE * SECONDS};
constexpr int BLOCK_FRAMES {4096};
constexpr int ITERATIONS {5};
constexpr std::size_t CODEC_HEADER_BYTES {72};

/// Fails correctness checks independently of the benchmark's deliberately absent timing thresholds.
void require(const bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/// Precomputes a periodic stereo signal once, outside every timed region.
juce::AudioBuffer<float> makeSourceBlock()
{
    juce::AudioBuffer<float> block(CHANNELS, BLOCK_FRAMES);
    for (int frame = 0; frame < BLOCK_FRAMES; ++frame) {
        const auto phase = 2.0 * std::numbers::pi * frame / BLOCK_FRAMES;
        block.setSample(0, frame, static_cast<float>(0.4 * std::sin(17.0 * phase) + 0.1 * std::sin(131.0 * phase)));
        block.setSample(1, frame, static_cast<float>(0.3 * std::sin(43.0 * phase) - 0.2 * std::sin(317.0 * phase)));
    }
    return block;
}

/// Presents a realistic track length and arbitrary absolute reads backed by one preallocated periodic block.
class RepeatedBlockReader final : public juce::AudioFormatReader
{
public:
    /// Owns only the small source block, never the simulated 30-second track.
    RepeatedBlockReader() : juce::AudioFormatReader(nullptr, "Spectrogram benchmark"), block(makeSourceBlock())
    {
        sampleRate = SAMPLE_RATE;
        bitsPerSample = 32;
        lengthInSamples = TOTAL_FRAMES;
        numChannels = CHANNELS;
        usesFloatingPointData = true;
    }

    /// Fills the JUCE float destination, including random offsets and silence outside the source range.
    bool readSamples(
        int* const* destinations,
        const int destinationChannels,
        const int destinationOffset,
        const juce::int64 sourceStart,
        const int frames
    ) override
    {
        if (destinations == nullptr || destinationChannels < 0 || destinationOffset < 0 || frames < 0) {
            return false;
        }
        const auto channels = std::span(destinations, static_cast<std::size_t>(destinationChannels));
        for (std::size_t channel = 0; channel < channels.size(); ++channel) {
            if (channels[channel] == nullptr) {
                continue;
            }
            // JUCE's float reader interface transfers binary32 representations through int channel storage.
            const auto destination
                = std::span(
                      channels[channel], static_cast<std::size_t>(destinationOffset) + static_cast<std::size_t>(frames)
                )
                      .subspan(static_cast<std::size_t>(destinationOffset));
            for (std::size_t frame = 0; frame < destination.size(); ++frame) {
                const auto position = sourceStart + static_cast<juce::int64>(frame);
                const auto sample = channel < CHANNELS && position >= 0 && position < lengthInSamples
                    ? block.getSample(static_cast<int>(channel), static_cast<int>(position % BLOCK_FRAMES))
                    : 0.0f;
                destination[frame] = std::bit_cast<int>(sample);
            }
        }
        return true;
    }

private:
    juce::AudioBuffer<float> block;
};

/// Includes builder allocation, immutable table setup, streaming, and EOF completion in the returned workload.
std::shared_ptr<const SpectrogramData> buildStandalone(const juce::AudioBuffer<float>& block)
{
    SpectrogramBuilder builder(CHANNELS, SAMPLE_RATE);
    for (int position = 0; position < TOTAL_FRAMES; position += BLOCK_FRAMES) {
        builder.push(block, 0, std::min(BLOCK_FRAMES, TOTAL_FRAMES - position));
    }
    return builder.finish();
}

/// Pins every geometry field and intensity byte rather than relying on a lossy checksum or timing limit.
void verifySnapshot(const std::shared_ptr<const SpectrogramData>& actual, const SpectrogramData& expected)
{
    require(actual != nullptr, "Missing spectrogram artifact");
    require(isValidSpectrogramData(*actual), "Invalid spectrogram geometry");
    require(
        actual->sampleRate == expected.sampleRate && actual->channelCount == expected.channelCount
            && actual->lengthInSamples == expected.lengthInSamples
            && actual->samplesPerColumn == expected.samplesPerColumn && actual->intensities == expected.intensities,
        "Spectrogram snapshot differs between equivalent workloads"
    );
}

/// Times only the operation, retaining its result until exact verification has finished outside the timed region.
/// The warmup exercises allocation and prepared statements, so cache results describe warm selected-file access.
template<typename Operation, typename Verify>
void measure(
    const std::string_view name,
    const std::string_view rateUnit,
    const double workUnits,
    const std::size_t payloadBytes,
    Operation operation,
    Verify verify
)
{
    double totalSeconds = 0.0;
    double maximumSeconds = 0.0;
    for (int iteration = 0; iteration <= ITERATIONS; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = operation();
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        verify(result);
        if (iteration > 0) {
            totalSeconds += seconds;
            maximumSeconds = std::max(maximumSeconds, seconds);
        }
    }
    const auto meanSeconds = totalSeconds / ITERATIONS;
    fmt::print(
        "  {}: n={} mean_ms={:.3f} max_ms={:.3f} payload_bytes={} {}={:.0f}{}",
        name,
        ITERATIONS,
        meanSeconds * 1000.0,
        maximumSeconds * 1000.0,
        payloadBytes,
        rateUnit,
        meanSeconds > 0.0 ? workUnits / meanSeconds : 0.0,
        juce::NewLine::getDefault()
    );
}

/// Compares CPU-only spectral work with the single-pass artifact backfill used by selected-file enrichment.
void benchmarkMemory()
{
    const auto block = makeSourceBlock();
    const auto reference = buildStandalone(block);
    require(reference != nullptr && isValidSpectrogramData(*reference), "Could not build reference spectrogram");
    const auto expectedColumns
        = static_cast<std::size_t>((TOTAL_FRAMES + SpectrogramData::HOP_SIZE - 1) / SpectrogramData::HOP_SIZE);
    require(
        reference->sampleRate == SAMPLE_RATE && reference->channelCount == CHANNELS
            && reference->lengthInSamples == TOTAL_FRAMES && reference->samplesPerColumn == SpectrogramData::HOP_SIZE
            && reference->columnCount() == expectedColumns
            && reference->intensities.size() == expectedColumns * SpectrogramData::FREQUENCY_ROWS,
        "Unexpected deterministic source geometry"
    );
    const auto blob = serializeSpectrogram(*reference);
    require(blob.getSize() == CODEC_HEADER_BYTES + reference->intensities.size(), "Unexpected codec byte geometry");
    const auto verify = [&](const auto& result) { verifySnapshot(result, *reference); };
    fmt::print(
        "memory: duration_seconds={} sample_rate={} channels={} frames={} source_block_bytes={} columns={} rows={} "
        "memory_scope=source_block_only{}",
        SECONDS,
        SAMPLE_RATE,
        CHANNELS,
        TOTAL_FRAMES,
        static_cast<std::size_t>(BLOCK_FRAMES) * CHANNELS * sizeof(float),
        reference->columnCount(),
        SpectrogramData::FREQUENCY_ROWS,
        juce::NewLine::getDefault()
    );
    measure(
        "standalone_setup_stream_finish",
        "frames_per_second",
        TOTAL_FRAMES,
        reference->intensities.size(),
        [&] { return buildStandalone(block); },
        verify
    );
    RepeatedBlockReader reader;
    const auto combined = buildPreviewAudioArtifacts(reader, true, true);
    const auto waveform = combined.waveform.value_or(juce::MemoryBlock {});
    require(waveform.getSize() > 0, "Combined backfill has no waveform");
    verifySnapshot(combined.spectrogram, *reference);
    for (const bool needWaveform : {false, true}) {
        measure(
            needWaveform ? "backfill_waveform_and_spectrum_no_loudness" : "backfill_spectrum_only_no_loudness",
            "frames_per_second",
            TOTAL_FRAMES,
            reference->intensities.size() + (needWaveform ? waveform.getSize() : 0),
            [&] { return buildPreviewAudioArtifacts(reader, needWaveform, true); },
            [&](const PreviewAudioArtifacts& artifacts) {
                verifySnapshot(artifacts.spectrogram, *reference);
                require(artifacts.waveform.has_value() == needWaveform, "Unexpected waveform availability");
                if (needWaveform) {
                    require(
                        artifacts.waveform.value_or(juce::MemoryBlock {}) == waveform,
                        "Waveform snapshot differs across repeats"
                    );
                }
            }
        );
    }
    measure(
        "codec_serialize",
        "bytes_per_second",
        static_cast<double>(blob.getSize()),
        blob.getSize(),
        [&] { return serializeSpectrogram(*reference); },
        [&](const juce::MemoryBlock& result) { require(result == blob, "Serialized snapshot differs"); }
    );
    const auto bytes = std::span(static_cast<const std::byte*>(blob.getData()), blob.getSize());
    measure(
        "codec_deserialize",
        "bytes_per_second",
        static_cast<double>(blob.getSize()),
        blob.getSize(),
        [&] { return deserializeSpectrogram(bytes); },
        verify
    );
}

/// Measures real identity-checked SQLite publication and selected-cache loading using one copied fixture.
/// File analysis and database schema creation happen once before the cache operations are timed.
void benchmarkCache(const juce::File& source)
{
    const TemporaryDirectory temporary;
    const auto copy = temporary.getChildFile(source.getFileName());
    require(source.existsAsFile() && source.copyFileTo(copy), "Could not copy cache benchmark fixture");
    AnalysisCache cache(temporary.getChildFile("spectrogram-benchmark.db"));
    require(cache.open(), "Could not open scratch benchmark cache");
    const auto analysis = analyzeFile(copy);
    require(analysis.isReady() && cache.storeAnalysis(analysis), "Could not seed benchmark measurements");
    const utils::FileIdentity identity(copy);
    const auto artifacts = buildPreviewAudioArtifacts(identity, true, true);
    const auto waveform = artifacts.waveform.value_or(juce::MemoryBlock {});
    require(artifacts.spectrogram != nullptr && waveform.getSize() > 0, "Could not build fixture artifacts");
    require(cache.storeWaveformData(identity, waveform), "Could not seed fixture waveform");
    const auto blob = serializeSpectrogram(*artifacts.spectrogram);
    fmt::print(
        "cache: fixture={} frames={} sample_rate={} channels={} state=warm isolated=true{}",
        source.getFileName(),
        analysis.lengthInSamples,
        analysis.sampleRate,
        analysis.channels,
        juce::NewLine::getDefault()
    );
    measure(
        "cache_store_spectrum",
        "operations_per_second",
        1.0,
        blob.getSize(),
        [&] { return cache.storeSpectrogramData(identity, *artifacts.spectrogram); },
        [&](const bool stored) {
            require(stored, "Could not store fixture spectrogram");
            verifySnapshot(cache.getSpectrogramData(copy), *artifacts.spectrogram);
        }
    );
    measure(
        "selected_cache_load_spectrum",
        "operations_per_second",
        1.0,
        blob.getSize(),
        [&] { return cache.getSpectrogramData(copy); },
        [&](const auto& result) { verifySnapshot(result, *artifacts.spectrogram); }
    );
    measure(
        "selected_cache_load_waveform_and_spectrum",
        "operations_per_second",
        1.0,
        blob.getSize() + waveform.getSize(),
        [&] {
            return PreviewAudioArtifacts {
                .waveform = cache.getWaveformData(copy),
                .spectrogram = cache.getSpectrogramData(copy),
            };
        },
        [&](const PreviewAudioArtifacts& result) {
            verifySnapshot(result.spectrogram, *artifacts.spectrogram);
            require(result.waveform.value_or(juce::MemoryBlock {}) == waveform, "Cached waveform snapshot differs");
        }
    );
}
}  // namespace

/// Returns -1 when not requested, otherwise runs only the explicit benchmark and reports correctness failures.
/// An optional absolute fixture path affects only the isolated cache workloads, not the deterministic CPU workload.
int runSpectrogramBenchmark(const juce::StringArray& arguments)
{
    if (!arguments.contains("--benchmark-spectrogram")) {
        return -1;
    }
    try {
        require(
            arguments.size() >= 1 && arguments.size() <= 2 && arguments[0] == "--benchmark-spectrogram",
            "Usage: AudioBatchTests --benchmark-spectrogram [ABSOLUTE_FIXTURE_PATH]"
        );
        require(
            arguments.size() != 2 || juce::File::isAbsolutePath(arguments[1]),
            "The optional cache fixture path must be absolute"
        );
        const auto source = arguments.size() == 2 ? juce::File(arguments[1]) : fixtureFile("sine_stereo_48000_24.wav");
        fmt::print(
            "Spectrogram benchmark (steady_clock, one warmup, no timing thresholds){}", juce::NewLine::getDefault()
        );
        benchmarkMemory();
        benchmarkCache(source);
        fmt::print(
            "Correctness: byte geometry and repeated, backfill, codec, and cache snapshots match.{}",
            juce::NewLine::getDefault()
        );
        return 0;
    } catch (const std::exception& error) {
        console::printError(utils::format("Spectrogram benchmark failed: {}", error.what()));
        return 1;
    }
}
}  // namespace audiobatch::tests
