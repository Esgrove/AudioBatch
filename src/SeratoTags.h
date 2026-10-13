/// Bounded Serato metadata payload parsers and independent numeric cache encodings.
/// These functions own their results and never access files or infer musical key from analysis versions.

#pragma once

#include "SeratoTagTypes.h"
#include <string_view>

namespace audiobatch::serato
{
inline constexpr std::size_t maxTagPayloadBytes = Overview::maxPayloadBytes;
inline constexpr std::size_t maxTagRecords = 4096;
inline constexpr std::size_t maxTagTextBytes = 32768;

/// Parses the two-byte Serato analysis version, rejecting truncated or extended payloads.
[[nodiscard]] std::optional<Analysis> parseAnalysis(std::span<const std::uint8_t> bytes);
/// Parses fixed-width ASCII tempo and gain fields with a supported version header.
[[nodiscard]] std::optional<Autotags> parseAutotags(std::span<const std::uint8_t> bytes);
/// Parses counted big-endian grid anchors and the opaque footer. Only the final anchor carries BPM.
[[nodiscard]] std::optional<BeatGrid> parseBeatGrid(std::span<const std::uint8_t> bytes);
/// Parses the Markers2 envelope and known records, skipping bounded unknown records by their lengths.
[[nodiscard]] std::optional<Markers> parseMarkers2(std::span<const std::uint8_t> bytes);
/// Parses the fixed legacy fourteen-slot layout as a fallback, without inventing cue names or BPM lock.
[[nodiscard]] std::optional<Markers> parseLegacyMarkers(std::span<const std::uint8_t> bytes);
/// Dispatches a stripped GEOB payload. Invalid data affects only the requested kind.
[[nodiscard]] TagReadResult parseTagPayload(
    TagKind kind,
    std::span<const std::uint8_t> bytes,
    bool legacyMarkers = false
);
/// Strictly parses a finite ASCII number without locale dependence or ignored trailing text.
[[nodiscard]] std::optional<double> parseTagNumber(std::string_view text);
/// Decodes bounded ASCII Base64, allowing whitespace and omitted padding.
/// The optional Serato workaround accepts the documented one-character remainder only for Markers2.
[[nodiscard]] std::optional<juce::MemoryBlock> decodeTagBase64(
    std::span<const std::uint8_t> bytes,
    bool markersWorkaround = false
);
/// Encodes a present result using a bounded, versioned numeric representation.
/// Overview delegates to its existing serializer. Invalid or mismatched public values return an empty block.
[[nodiscard]] juce::MemoryBlock serializeTag(const TagReadResult& result);
/// Validates kind, version, exact length, counts, UTF-8 and finite numeric values before returning owned data.
/// Overview delegates to its existing deserializer. Corrupt data is a cache miss.
[[nodiscard]] std::optional<TagReadResult> deserializeTag(TagKind kind, const juce::MemoryBlock& block);
}  // namespace audiobatch::serato