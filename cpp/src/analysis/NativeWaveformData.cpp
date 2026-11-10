/// Validates native JUCE thumbnail geometry without allocating sample arrays.
/// Builds source-version keys for in-memory waveform reuse independently of database persistence.

#include "analysis/NativeWaveformData.h"

#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace audiobatch
{
/// Validates a borrowed payload before copying it or allowing JUCE to allocate native thumbnail arrays.
std::optional<WaveformGeometry> waveformGeometry(const std::span<const std::byte> data)
{
    constexpr std::size_t headerBytes {52};
    constexpr int waveformMagic {0x6d74616a};
    constexpr int minimumDetail {16};
    constexpr int maximumDetail {1024};
    constexpr int maximumWaveformSampleRate {768'000};
    constexpr std::int64_t maximumPoints {4'194'304};
    if (data.size() < headerBytes || data.size() > static_cast<std::size_t>(MAXIMUM_WAVEFORM_BYTES)) {
        return std::nullopt;
    }
    juce::MemoryInputStream input(data.data(), data.size(), false);
    if (input.readInt() != waveformMagic) {
        return std::nullopt;
    }
    const auto detail = input.readInt();
    const auto total = input.readInt64();
    const auto finished = input.readInt64();
    const auto points = input.readInt();
    const auto channels = input.readInt();
    const auto rate = input.readInt();
    if (detail < minimumDetail || detail > maximumDetail || !std::has_single_bit(static_cast<unsigned int>(detail))
        || total <= 0 || finished < 0 || finished > total || total - finished > detail || points <= 0
        || points > maximumPoints || (channels != 1 && channels != 2) || rate <= 0 || rate > maximumWaveformSampleRate
        || total / detail != static_cast<std::int64_t>(points) - 1
        || data.size() - headerBytes != static_cast<std::size_t>(points) * static_cast<std::size_t>(channels) * 2)
    {
        return std::nullopt;
    }
    return WaveformGeometry {.channels = channels, .sampleRate = rate, .lengthInSamples = total};
}

/// Validates the complete mono/stereo thumbnail payload before the JUCE deserializer can allocate.
/// Cached thumbnails use bounded power-of-two detail and complete source geometry, including the final partial point.
bool isValidWaveformData(const juce::MemoryBlock& data)
{
    return waveformGeometry(std::span(static_cast<const std::byte*>(data.getData()), data.getSize())).has_value();
}

/// Keys in-memory thumbnail reuse by the captured source version, detail and waveform format version.
juce::int64 waveformCacheKey(const utils::FileIdentity& identity, const int samplesPerPoint)
{
    const auto [volume, file] = identity.platformId.value_or(std::pair<std::uint64_t, std::uint64_t> {});
    return utils::format(
               "{}:{}:{}:{}:{}:{}:{}:{}:3",
               identity.path,
               identity.fileSize,
               identity.modifiedTimeMs,
               volume,
               file,
               identity.preciseTimes.first,
               identity.preciseTimes.second,
               samplesPerPoint
    )
        .hashCode64();
}
}  // namespace audiobatch
