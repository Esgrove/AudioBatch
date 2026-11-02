/// Bounded native JUCE thumbnail data shared by waveform producers, consumers, and persistence.
/// Declares the geometry parser, allocation bounds, payload validation, and source-version cache keys.

#pragma once

#include "utils/FileUtils.h"

#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace audiobatch
{
/// Validated native thumbnail geometry, read without allocating preview storage.
struct WaveformGeometry {
    /// Source channel count from the native serializer.
    int channels {0};
    /// Source rate from the native serializer.
    int sampleRate {0};
    /// Complete source length, including the final partial point.
    std::int64_t lengthInSamples {0};
};

/// Largest supported serialized native thumbnail, including its fixed-size header.
inline constexpr int maximumWaveformBytes {52 + 4'194'304 * 2 * 2};

[[nodiscard]] std::optional<WaveformGeometry> waveformGeometry(std::span<const std::byte> data);

[[nodiscard]] juce::int64 waveformCacheKey(const utils::FileIdentity& identity, int samplesPerPoint);
[[nodiscard]] bool isValidWaveformData(const juce::MemoryBlock& data);
}  // namespace audiobatch
