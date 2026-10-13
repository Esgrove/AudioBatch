/// Owned Serato Overview indexed raster columns and their bounded binary representations.
/// Parsing preserves raw palette indices without deriving waveform geometry or display colors.

#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace audiobatch::serato
{
/// Independent extraction and numeric serialization version for the Overview kind.
inline constexpr std::uint32_t overviewParserVersion = 1;

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
    static constexpr std::size_t bandCount = 16;
    static constexpr std::size_t maxSlices = 65536;
    static constexpr std::size_t maxPayloadBytes = 2 + maxSlices * bandCount;

    std::vector<std::array<std::uint8_t, bandCount>> slices;

    /// Parses the binary payload after the enclosing GEOB or FLAC wrapper has been removed.
    /// Offsets and lengths below are hexadecimal. The final row illustrates a 240-slice payload.
    ///
    /// | Offset | Length | Raw value     | Type         | Description                           |
    /// | ------ | ------ | ------------- | ------------ | ------------------------------------- |
    /// | 0x0000 | 0x0002 | 01 05         | two bytes    | Supported header, meaning unspecified |
    /// | 0x0002 | 0x0010 | sixteen bytes | 16 * uint8_t | Palette indices for time slice 0      |
    /// | 0x0012 | 0x0010 | sixteen bytes | 16 * uint8_t | Palette indices for time slice 1      |
    /// | ...    | ...    | ...           | ...          | Subsequent consecutive time slices    |
    /// | 0x0ef2 | 0x0010 | sixteen bytes | 16 * uint8_t | Palette indices for time slice 239    |
    ///
    /// A payload with N slices occupies 2 + 16 * N bytes, or 3842 bytes for 240 slices.
    /// Accepts header 01 05 and one to maxSlices complete slices, rejecting tails and unknown headers.
    /// The returned slices own their data independently of the input buffer.
    [[nodiscard]] static std::optional<Overview> parse(std::span<const std::uint8_t> bytes);

    /// Encodes magic, kind, parser version and count as big-endian fields followed by raw palette indices.
    /// Returns an empty block when the public slice vector is empty or exceeds maxSlices.
    [[nodiscard]] juce::MemoryBlock serialize() const;

    /// Validates the complete numeric cache representation before allocating owned slices.
    /// Unknown magic, kind, version, count or an inexact length is a cache miss.
    [[nodiscard]] static std::optional<Overview> deserialize(std::span<const std::uint8_t> bytes);
};
}  // namespace audiobatch::serato
