/// Owned Serato Overview indexed raster columns and their bounded binary representations.
/// Parsing preserves raw palette indices without deriving waveform geometry or display colors.

#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace audiobatch::serato
{
/// Independent extraction and numeric serialization version for the Overview kind.
inline constexpr std::uint32_t overviewParserVersion {1};

/// Owns the Serato Overview's coarse color raster for the whole track.
/// Common payloads contain 240 consecutive time slices regardless of track duration,
/// with sixteen vertically arranged palette indices per slice. Other complete slice counts are supported.
/// Same-track screenshot comparisons support a zero-based 6x6x6 RGB cube for foreground indices below 216.
/// These bytes are not sixteen frequency strengths. Their vertical arrangement describes the small overview raster.
/// Indices remain in original tag order without averaging or conversion. Rendering decodes colors separately,
/// skips background cells and leaves the reserved palette unverified.
/// The raster provides neither finer spectral detail on zoom nor separate left and right channel data.
struct Overview {
    /// Number of vertical raster cells, retaining the existing serialized column layout.
    static constexpr std::size_t bandCount {16};
    /// Highest number of horizontal slices accepted when parsing or deserializing.
    static constexpr std::size_t maxSlices {65'536};
    /// Largest valid payload size, including the two byte header.
    static constexpr std::size_t maxPayloadBytes {2 + maxSlices * bandCount};

    [[nodiscard]] static std::optional<Overview> deserialize(std::span<const std::uint8_t> bytes);
    [[nodiscard]] static std::optional<Overview> parse(std::span<const std::uint8_t> bytes);

    [[nodiscard]] juce::MemoryBlock serialize() const;

    /// Palette indices of the raster, one array of bandCount cells per horizontal slice.
    std::vector<std::array<std::uint8_t, bandCount>> slices;
};
}  // namespace audiobatch::serato
