/// Bounded Serato metadata payload parsers and independent numeric cache encodings.
/// Also holds the helpers that the tag reader and the metadata copy share:
/// the names of each tag in ID3 and FLAC files, the bounded Base64 decoding and the wrapper of a native FLAC value,
/// and the lookup of the ID3 tag of a file TagLib has opened.
/// Parsers own their results, while the tag lookup borrows from the caller's open file.
/// These functions never open files or infer musical key from analysis versions.

#pragma once

#include "serato/SeratoTagTypes.h"
#include "utils/OptionalRef.h"
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
    // Keep the native property before the ID3 description, matching aggregate tag-name tables.
    /// Name of the native FLAC comment, such as "SERATO_OVERVIEW".
    std::string_view property;
    /// Description of the ID3 GEOB frame, which the wrapper of the FLAC comment repeats, such as "Serato Overview".
    std::string_view description;
};

inline constexpr std::size_t MAX_TAG_PAYLOAD_BYTES {Overview::MAX_PAYLOAD_BYTES};
inline constexpr std::size_t MAX_TAG_RECORDS {4096};
inline constexpr std::size_t MAX_TAG_TEXT_BYTES {32'768};
/// Largest decoded native FLAC value, the payload with room for its wrapper.
inline constexpr std::size_t MAX_DECODED_TAG_BYTES {MAX_TAG_PAYLOAD_BYTES + 1024};
/// Largest Base64 text of a native FLAC value, with room for whitespace.
inline constexpr std::size_t MAX_ENCODED_TAG_BYTES {((MAX_DECODED_TAG_BYTES + 2) / 3) * 4 + 4096};

/// Names of the tags of each kind.
inline constexpr TagNames OVERVIEW_NAMES {.property = "SERATO_OVERVIEW", .description = "Serato Overview"};
inline constexpr TagNames ANALYSIS_NAMES {.property = "SERATO_ANALYSIS", .description = "Serato Analysis"};
inline constexpr TagNames AUTOTAGS_NAMES {.property = "SERATO_AUTOGAIN", .description = "Serato Autotags"};
inline constexpr TagNames BEAT_GRID_NAMES {.property = "SERATO_BEATGRID", .description = "Serato BeatGrid"};
inline constexpr TagNames MARKERS_NAMES {.property = "SERATO_MARKERS_V2", .description = "Serato Markers2"};
/// Names of the legacy Markers_ tag, which is only read when a file has no usable Markers2 tag.
inline constexpr TagNames LEGACY_MARKERS_NAMES {.property = "SERATO_MARKERS", .description = "Serato Markers_"};

/// Names of every Serato tag that a FLAC file keeps as a native comment, including the legacy markers.
inline constexpr std::array ALL_TAG_NAMES {
    OVERVIEW_NAMES,
    ANALYSIS_NAMES,
    AUTOTAGS_NAMES,
    BEAT_GRID_NAMES,
    MARKERS_NAMES,
    LEGACY_MARKERS_NAMES,
};

[[nodiscard]] TagReadResult parseTagPayload(
    TagKind kind,
    std::span<const std::uint8_t> bytes,
    bool legacyMarkers = false
);
[[nodiscard]] utils::OptionalRef<TagLib::ID3v2::Tag> findId3Tag(TagLib::File& file, bool create = false);
[[nodiscard]] juce::MemoryBlock serializeTag(const TagReadResult& result);

[[nodiscard]] std::optional<Analysis> parseAnalysis(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<Autotags> parseAutotags(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<BeatGrid> parseBeatGrid(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<Markers> parseLegacyMarkers(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<Markers> parseMarkers2(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<TagNames> getTagNames(TagKind kind, bool legacyMarkers = false);
[[nodiscard]] std::optional<TagReadResult> deserializeTag(TagKind kind, const juce::MemoryBlock& block);

[[nodiscard]] std::optional<double> parseTagNumber(std::string_view text);

[[nodiscard]] std::optional<juce::MemoryBlock> decodeNativeTagBase64(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<juce::MemoryBlock> decodeTagBase64(
    std::span<const std::uint8_t> bytes,
    bool markersWorkaround = false
);
[[nodiscard]] std::optional<std::span<const std::uint8_t>> parseTagWrapper(
    std::span<const std::uint8_t> decoded,
    std::string_view description
);
}  // namespace audiobatch::serato
