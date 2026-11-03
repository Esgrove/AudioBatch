/// Accumulates native thumbnail buckets across arbitrary PCM chunk boundaries without retaining PCM.
/// Preserves JUCE quantization, channel ordering, complete-bucket geometry and the empty trailing point.

#include "analysis/NativeWaveformBuilder.h"

#include "analysis/NativeWaveformData.h"

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
constexpr juce::int64 THUMBNAIL_TARGET_POINTS {16'384};
constexpr int MINIMUM_DETAIL {16};
constexpr int MAXIMUM_DETAIL {1024};
constexpr juce::int64 MAXIMUM_POINTS {4'194'304};
constexpr int MAXIMUM_SAMPLE_RATE {768'000};
constexpr std::size_t HEADER_BYTES {52};
constexpr int MAXIMUM_QUANTIZED_PEAK {127};
constexpr int MINIMUM_QUANTIZED_PEAK {-128};
constexpr float QUANTIZATION_SCALE {127.0f};

/// Uses JUCE's native peak quantization without converting unbounded floats to integers.
int quantize(const float value)
{
    if (value <= -1.0f) {
        return value * QUANTIZATION_SCALE <= static_cast<float>(MINIMUM_QUANTIZED_PEAK)
            ? MINIMUM_QUANTIZED_PEAK
            : juce::roundToInt(value * QUANTIZATION_SCALE);
    }
    return value >= 1.0f ? MAXIMUM_QUANTIZED_PEAK : juce::roundToInt(value * QUANTIZATION_SCALE);
}

/// Writes one native range, expanding a flat range by one quantization step.
void writeRange(juce::OutputStream& output, const float minimumValue, const float maximumValue)
{
    auto minimum = quantize(minimumValue);
    auto maximum = quantize(maximumValue);
    if (minimum == maximum) {
        if (maximum == MAXIMUM_QUANTIZED_PEAK) {
            --minimum;
        } else {
            ++maximum;
        }
    }
    output.writeByte(static_cast<char>(minimum));
    output.writeByte(static_cast<char>(maximum));
}
}  // namespace

/// Chooses the existing power-of-two thumbnail detail, bounded for short and long sources.
int samplesPerThumbnailPoint(const juce::int64 lengthInSamples)
{
    if (lengthInSamples <= 0) {
        return MAXIMUM_DETAIL;
    }
    auto detail = MINIMUM_DETAIL;
    while (detail < MAXIMUM_DETAIL && lengthInSamples / detail > THUMBNAIL_TARGET_POINTS) {
        detail *= 2;
    }
    return detail;
}

/// Allocates bounded zero-filled native geometry, leaving invalid source geometry inert.
/// Allocation exceptions belong to this optional artifact and may be isolated by its caller.
NativeWaveformBuilder::NativeWaveformBuilder(const int sourceChannels, const int sampleRate, const juce::int64 length) :
    totalLength(length),
    channels(sourceChannels),
    detail(samplesPerThumbnailPoint(length))
{
    const auto completePoints = length / detail;
    if ((channels != 1 && channels != 2) || sampleRate <= 0 || sampleRate > MAXIMUM_SAMPLE_RATE || length <= 0
        || completePoints >= MAXIMUM_POINTS)
    {
        return;
    }
    const auto bytes
        = HEADER_BYTES + static_cast<std::size_t>(completePoints + 1) * static_cast<std::size_t>(channels) * 2;
    if (bytes > static_cast<std::size_t>(maximumWaveformBytes)) {
        return;
    }
    data.setSize(bytes, true);
    juce::MemoryOutputStream output(data, true);
    output.setPosition(0);
    output.write("jatm", 4);
    output.writeInt(detail);
    output.writeInt64(length);
    output.writeInt64(completePoints * detail);
    output.writeInt(static_cast<int>(completePoints + 1));
    output.writeInt(channels);
    output.writeInt(sampleRate);
    output.writeInt64(0);
    output.writeInt64(0);
    output.flush();
    valid = true;
}

/// Returns cacheable native bytes only after exact source completion, consuming this builder.
/// A final partial bucket remains zero, as in JUCE's source-backed serialization.
std::optional<juce::MemoryBlock> NativeWaveformBuilder::finish()
{
    if (!valid || received != totalLength || !isValidWaveformData(data)) {
        return std::nullopt;
    }
    valid = false;
    return std::move(data);
}

/// Copies full source geometry on explicit request, leaving partial and unread buckets zero-filled.
/// This display-only snapshot MUST NEVER be persisted or cached, even though strict validation accepts it.
/// Its header claims total completion solely for the native loader. Use framesReceived() for real progress.
std::optional<juce::MemoryBlock> NativeWaveformBuilder::snapshot() const
{
    if (!valid) {
        return std::nullopt;
    }
    auto provisional = data;
    {
        juce::MemoryOutputStream output(provisional, true);
        output.setPosition(16);
        output.writeInt64(totalLength);
        output.flush();
    }
    return provisional;
}

/// Reports source frames consumed independently of the provisional serialized completion marker.
juce::int64 NativeWaveformBuilder::framesReceived() const
{
    return received;
}

/// Appends a borrowed PCM slice, carrying its partial bucket into the next push.
/// Invalid slices, excess frames or any nonfinite sample invalidate only this optional artifact.
void NativeWaveformBuilder::push(const juce::AudioBuffer<float>& buffer, const int first, const int frames)
{
    if (!valid) {
        return;
    }
    if (first < 0 || frames < 0 || first > buffer.getNumSamples() || frames > buffer.getNumSamples() - first
        || buffer.getNumChannels() != channels || static_cast<juce::int64>(frames) > totalLength - received)
    {
        valid = false;
        return;
    }
    juce::MemoryOutputStream output(data, true);
    output.setPosition(static_cast<juce::int64>(HEADER_BYTES) + (received / detail) * channels * 2);
    for (int offset = 0; offset < frames;) {
        const auto count = std::min(detail - partialFrames, frames - offset);
        for (int channel = 0; channel < channels; ++channel) {
            const auto index = static_cast<std::size_t>(channel);
            const auto samples
                = std::span(buffer.getReadPointer(channel, first + offset), static_cast<std::size_t>(count));
            for (int sample = 0; sample < count; ++sample) {
                const auto value = samples[static_cast<std::size_t>(sample)];
                if (!std::isfinite(value)) {
                    valid = false;
                    return;
                }
                if (partialFrames == 0 && sample == 0) {
                    minimum[index] = maximum[index] = value;
                } else {
                    minimum[index] = std::min(minimum[index], value);
                    maximum[index] = std::max(maximum[index], value);
                }
            }
        }
        partialFrames += count;
        received += count;
        offset += count;
        if (partialFrames == detail) {
            for (int channel = 0; channel < channels; ++channel) {
                const auto index = static_cast<std::size_t>(channel);
                writeRange(output, minimum[index], maximum[index]);
            }
            partialFrames = 0;
        }
    }
    output.flush();
}
}  // namespace audiobatch
