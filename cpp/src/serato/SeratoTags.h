/// Bounded Serato metadata payload parsers and independent numeric cache encodings.
/// Also holds the helpers that the tag reader and the metadata copy share:
/// the names of each tag in ID3 and FLAC files, the bounded Base64 decoding and the wrapper of a native FLAC value,
/// and the lookup of the ID3 tag of a file TagLib has opened.
/// These functions own their results and never open files or infer musical key from analysis versions.

#pragma once

#include "serato/SeratoTagTypes.h"
#include <string_view>

#include <array>

namespace TagLib
{
class File;

namespace ID3v2
{
class Tag;
}  // namespace ID3v2
}  // namespace TagLib

namespace audiobatch::serato
{
/// The names of one Serato tag in the two containers that hold it.
struct TagNames {
    /// Name of the native FLAC comment, such as "SERATO_OVERVIEW".
    std::string_view property;
    /// Description of the ID3 GEOB frame, which the wrapper of the FLAC comment repeats, such as "Serato Overview".
    std::string_view description;
};

inline constexpr std::size_t maxTagPayloadBytes {Overview::maxPayloadBytes};
inline constexpr std::size_t maxTagRecords {4096};
inline constexpr std::size_t maxTagTextBytes {32'768};
/// Largest decoded native FLAC value, the payload with room for its wrapper.
inline constexpr std::size_t maxDecodedTagBytes {maxTagPayloadBytes + 1024};
/// Largest Base64 text of a native FLAC value, with room for whitespace.
inline constexpr std::size_t maxEncodedTagBytes {((maxDecodedTagBytes + 2) / 3) * 4 + 4096};

/// Names of the tags of each kind.
inline constexpr TagNames overviewNames {.property = "SERATO_OVERVIEW", .description = "Serato Overview"};
inline constexpr TagNames analysisNames {.property = "SERATO_ANALYSIS", .description = "Serato Analysis"};
inline constexpr TagNames autotagsNames {.property = "SERATO_AUTOGAIN", .description = "Serato Autotags"};
inline constexpr TagNames beatGridNames {.property = "SERATO_BEATGRID", .description = "Serato BeatGrid"};
inline constexpr TagNames markersNames {.property = "SERATO_MARKERS_V2", .description = "Serato Markers2"};
/// Names of the legacy Markers_ tag, which is only read when a file has no usable Markers2 tag.
inline constexpr TagNames legacyMarkersNames {.property = "SERATO_MARKERS", .description = "Serato Markers_"};

/// Names of every Serato tag that a FLAC file keeps as a native comment, including the legacy markers.
inline constexpr std::array allTagNames {
    overviewNames,
    analysisNames,
    autotagsNames,
    beatGridNames,
    markersNames,
    legacyMarkersNames,
};

[[nodiscard]] juce::MemoryBlock serializeTag(const TagReadResult& result);
[[nodiscard]] std::optional<Analysis> parseAnalysis(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<Autotags> parseAutotags(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<BeatGrid> parseBeatGrid(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<Markers> parseLegacyMarkers(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<Markers> parseMarkers2(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<TagReadResult> deserializeTag(TagKind kind, const juce::MemoryBlock& block);
[[nodiscard]] std::optional<double> parseTagNumber(std::string_view text);
[[nodiscard]] std::optional<TagNames> getTagNames(TagKind kind, bool legacyMarkers = false);

[[nodiscard]] std::optional<juce::MemoryBlock> decodeTagBase64(
    std::span<const std::uint8_t> bytes,
    bool markersWorkaround = false
);
[[nodiscard]] std::optional<std::span<const std::uint8_t>> parseTagWrapper(
    std::span<const std::uint8_t> decoded,
    std::string_view description
);

[[nodiscard]] TagReadResult parseTagPayload(
    TagKind kind,
    std::span<const std::uint8_t> bytes,
    bool legacyMarkers = false
);

[[nodiscard]] TagLib::ID3v2::Tag* findId3Tag(TagLib::File& file, bool create = false);
}  // namespace audiobatch::serato
