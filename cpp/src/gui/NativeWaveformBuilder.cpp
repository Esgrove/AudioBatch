/// Serializes JUCE-native thumbnail peaks from bounded PCM blocks on the admitted enrichment worker.
/// Only complete thumbnail buckets are encoded, matching JUCE's source-backed final-bucket behavior.
/// No loudness meter, true-peak processing, source-backed thumbnail or auxiliary decoder thread is created.

#include "gui/NativeWaveformBuilder.h"

#include "analysis/DecodeFailureTracker.h"
#include "analysis/NativeWaveformData.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/ThreadUtils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>

namespace audiobatch
{
namespace
{
constexpr juce::int64 thumbnailTargetPoints {16'384};
constexpr int minimumDetail {16};
constexpr int maximumDetail {1024};
constexpr juce::int64 maximumPoints {4'194'304};
constexpr int decodeBlockSize {16'384};
constexpr int maximumQuantizedPeak {127};
constexpr int minimumQuantizedPeak {-128};
constexpr float quantizationScale {127.0f};
constexpr double maximumSampleRate {768'000.0};

/// Writes JUCE's quantized minimum and maximum, expanding a flat range by one step.
void writeRange(juce::OutputStream& output, const juce::Range<float> range)
{
    const auto quantize = [](const float value) {
        if (value <= -1.0f) {
            return value * quantizationScale <= static_cast<float>(minimumQuantizedPeak)
                ? minimumQuantizedPeak
                : juce::roundToInt(value * quantizationScale);
        }
        return value >= 1.0f ? maximumQuantizedPeak : juce::roundToInt(value * quantizationScale);
    };
    auto minimum = quantize(range.getStart());
    auto maximum = quantize(range.getEnd());
    if (minimum == maximum) {
        if (maximum == maximumQuantizedPeak) {
            --minimum;
        } else {
            ++maximum;
        }
    }
    output.writeByte(static_cast<char>(minimum));
    output.writeByte(static_cast<char>(maximum));
}
}  // namespace

/// Chooses the same power-of-two detail as the preview, bounded for both short and long files.
int samplesPerThumbnailPoint(const juce::int64 lengthInSamples)
{
    if (lengthInSamples <= 0) {
        return maximumDetail;
    }
    auto detail = minimumDetail;
    while (detail < maximumDetail && lengthInSamples / detail > thumbnailTargetPoints) {
        detail *= 2;
    }
    return detail;
}

/// Reads corrected native PCM in bounded blocks and serializes the exact source-backed thumbnail geometry.
/// The extra trailing point stays zero, and unfinished final buckets remain empty, as in JUCE's native reader path.
/// Decode failures use the same tolerance as analysis, but repeated failures never publish a partial waveform.
std::optional<juce::MemoryBlock> buildNativeWaveform(
    juce::AudioFormatReader& reader,
    const utils::CancellationCheck& isCancelled
)
{
    const auto detail = samplesPerThumbnailPoint(reader.lengthInSamples);
    const auto completePoints = reader.lengthInSamples / detail;
    if (reader.lengthInSamples <= 0 || (reader.numChannels != 1 && reader.numChannels != 2)
        || !std::isfinite(reader.sampleRate) || reader.sampleRate <= 0.0 || reader.sampleRate > maximumSampleRate
        || completePoints >= maximumPoints || utils::isCancellationRequested(isCancelled))
    {
        return std::nullopt;
    }
    const auto channels = static_cast<int>(reader.numChannels);
    juce::MemoryBlock data;
    juce::MemoryOutputStream output(data, false);
    output.write("jatm", 4);
    output.writeInt(detail);
    output.writeInt64(reader.lengthInSamples);
    output.writeInt64(completePoints * detail);
    output.writeInt(static_cast<int>(completePoints + 1));
    output.writeInt(channels);
    output.writeInt(static_cast<int>(reader.sampleRate));
    output.writeInt64(0);
    output.writeInt64(0);

    juce::AudioBuffer<float> buffer(channels, decodeBlockSize);
    DecodeFailureTracker failures(reader.lengthInSamples);
    for (juce::int64 position = 0; position < reader.lengthInSamples; position += decodeBlockSize) {
        if (utils::isCancellationRequested(isCancelled)) {
            return std::nullopt;
        }
        const auto count = static_cast<int>(std::min<juce::int64>(decodeBlockSize, reader.lengthInSamples - position));
        buffer.clear();
        if (!failures.recordBlock(
                utils::readFloatSamples(reader, utils::writePointers(buffer), position, count), position, count
            ))
        {
            return std::nullopt;
        }
        for (int offset = 0; offset + detail <= count; offset += detail) {
            for (int channel = 0; channel < channels; ++channel) {
                const auto* samples = buffer.getReadPointer(channel, offset);
                if (!std::ranges::all_of(
                        std::span(samples, static_cast<std::size_t>(detail)),
                        [](const float value) { return std::isfinite(value); }
                    ))
                {
                    return std::nullopt;
                }
                writeRange(output, juce::FloatVectorOperations::findMinAndMax(samples, detail));
            }
        }
    }
    for (int channel = 0; channel < channels; ++channel) {
        output.writeShort(0);
    }
    output.flush();
    if (utils::isCancellationRequested(isCancelled) || !isValidWaveformData(data)) {
        return std::nullopt;
    }
    return data;
}

/// Captures no new identity during publication, rejecting replacement before and after waveform-only decoding.
std::optional<juce::MemoryBlock> buildNativeWaveform(
    const utils::FileIdentity& identity,
    const utils::CancellationCheck& isCancelled
)
{
    const juce::File file(identity.path);
    if (!identity.matches(file) || utils::isCancellationRequested(isCancelled)) {
        return std::nullopt;
    }
    const auto reader = utils::createAudioReader(file);
    if (!reader || !identity.matches(file)) {
        return std::nullopt;
    }
    auto data = buildNativeWaveform(*reader, isCancelled);
    return identity.matches(file) ? std::move(data) : std::nullopt;
}
}  // namespace audiobatch
