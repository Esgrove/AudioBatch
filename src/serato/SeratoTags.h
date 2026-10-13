/// Bounded Serato metadata payload parsers and independent numeric cache encodings.
/// These functions own their results and never access files or infer musical key from analysis versions.

#pragma once

#include "serato/SeratoTagTypes.h"
#include <string_view>

namespace audiobatch::serato
{
inline constexpr std::size_t maxTagPayloadBytes = Overview::maxPayloadBytes;
inline constexpr std::size_t maxTagRecords = 4096;
inline constexpr std::size_t maxTagTextBytes = 32768;

[[nodiscard]] std::optional<Analysis> parseAnalysis(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<Autotags> parseAutotags(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<BeatGrid> parseBeatGrid(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<Markers> parseMarkers2(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<Markers> parseLegacyMarkers(std::span<const std::uint8_t> bytes);
[[nodiscard]] TagReadResult parseTagPayload(
    TagKind kind,
    std::span<const std::uint8_t> bytes,
    bool legacyMarkers = false
);
[[nodiscard]] std::optional<double> parseTagNumber(std::string_view text);
[[nodiscard]] std::optional<juce::MemoryBlock> decodeTagBase64(
    std::span<const std::uint8_t> bytes,
    bool markersWorkaround = false
);
[[nodiscard]] juce::MemoryBlock serializeTag(const TagReadResult& result);
[[nodiscard]] std::optional<TagReadResult> deserializeTag(TagKind kind, const juce::MemoryBlock& block);
}  // namespace audiobatch::serato