/// Parses documented Serato payloads.
/// Bounds apply to encoded input, decoded data, record counts and owned UTF-8 text.
/// Also decodes the Base64 and the wrapper of native FLAC values, and finds the ID3v2 tag of an open TagLib file.

#include "serato/SeratoTags.h"

#include "serato/SeratoBytes.h"
#include "serato/SeratoOverview.h"
#include "serato/SeratoTagFields.h"
#include "serato/SeratoTagTypes.h"
#include "utils/OptionalRef.h"
#include <string_view>

#include <aifffile.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <flacfile.h>
#include <id3v2tag.h>
#include <ios>
#include <locale>
#include <mpegfile.h>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <wavfile.h>

namespace audiobatch::serato
{
namespace
{
constexpr std::size_t BPM_OFFSET {2};
constexpr std::size_t AUTO_GAIN_OFFSET {9};
constexpr std::size_t GAIN_OFFSET {16};
constexpr std::size_t FULL_AUTOTAGS_BYTES {22};
constexpr std::size_t COMPACT_AUTOTAGS_MIN_BYTES {20};
constexpr std::size_t COMPACT_AUTOTAGS_MAX_BYTES {21};
constexpr std::size_t GRID_HEADER_BYTES {6};
constexpr std::array<std::uint8_t, 2> OVERVIEW_PAYLOAD_HEADER {1, 5};
constexpr std::size_t GRID_RECORD_BYTES {8};
constexpr std::size_t CUE_COLOUR_OFFSET {7};
constexpr std::size_t LOOP_COLOUR_OFFSET {15};
constexpr std::size_t LOOP_LOCK_OFFSET {19};
constexpr std::size_t MAX_RECORD_TYPE_BYTES {64};
constexpr std::size_t LEGACY_COUNT {14};
constexpr std::size_t LEGACY_RECORD_BYTES {22};
constexpr std::size_t LEGACY_COLOUR_OFFSET {16};
constexpr std::size_t LEGACY_TYPE_OFFSET {20};
constexpr std::size_t LEGACY_LOCK_OFFSET {21};
constexpr std::size_t LEGACY_END_OFFSET {6};
constexpr std::uint8_t LEGACY_UNSET {0x7f};
constexpr std::uint8_t LEGACY_MAJOR_VERSION {2};
constexpr std::uint8_t LEGACY_MINOR_VERSION {5};
constexpr std::uint8_t LEGACY_CUE_TYPE {1};
constexpr std::uint8_t LEGACY_LOOP_TYPE {3};
constexpr std::uint8_t MAX_ASCII_BYTE {127};
constexpr unsigned int SERATO_GROUP_BITS {7};
constexpr std::uint32_t SERATO_GROUP_MASK {0x7f};
constexpr std::uint32_t SERATO_HIGH_MASK {0x07};
constexpr std::uint32_t OPAQUE_ALPHA {0xff000000};

/// Reads an exact terminated marker label, permitting zero padding after its terminator.
std::optional<juce::String> markerName(std::span<const std::uint8_t> bytes)
{
    const auto end = std::ranges::find(bytes, std::uint8_t {0});
    if (end == bytes.end()) {
        return std::nullopt;
    }
    const auto length = static_cast<std::size_t>(end - bytes.begin());
    return allZero(bytes.subspan(length)) ? utf8(bytes.first(length)) : std::nullopt;
}

/// Decodes a big-endian four-byte integer.
constexpr std::uint32_t integer(const std::span<const std::uint8_t, sizeof(std::uint32_t)> bytes)
{
    return readBigEndian<std::uint32_t>(bytes);
}

/// Decodes opaque-alpha Serato RGB bytes into an opaque display colour.
juce::Colour rgb(const std::span<const std::uint8_t, RGB_BYTES> bytes)
{
    return {bytes[0], bytes[1], bytes[2]};
}

/// Parses an ASCII fixed-width number with null padding and no hidden suffix.
std::optional<double> fixedNumber(std::span<const std::uint8_t> bytes)
{
    const auto end = std::ranges::find(bytes, std::uint8_t {0});
    if (end == bytes.end()) {
        return std::nullopt;
    }
    const auto length = static_cast<std::size_t>(end - bytes.begin());
    if (!allZero(bytes.subspan(length))) {
        return std::nullopt;
    }
    const std::string text(bytes.begin(), end);
    return parseTagNumber(text);
}

/// Adds a supported record, rejecting invalid known records instead of partially displaying them.
bool addMarker(Markers& markers, const juce::String& type, const std::span<const std::uint8_t> bytes)
{
    if (type == "BPMLOCK") {
        if (bytes.size() != 1 || bytes[0] > 1) {
            return false;
        }
        markers.bpmLocked = bytes[0] != 0;
    } else if (type == "COLOR") {
        if (bytes.size() != 4) {
            return false;
        }
        markers.colour = rgb(bytes.subspan<1, RGB_BYTES>());
    } else if (type == "CUE") {
        if (bytes.size() <= CUE_NAME_OFFSET) {
            return false;
        }
        const auto name = markerName(bytes.subspan(CUE_NAME_OFFSET));
        if (!name) {
            return false;
        }
        markers.cues.push_back({
            .positionMilliseconds = integer(bytes.subspan<2, sizeof(std::uint32_t)>()),
            .index = bytes[1],
            .colour = rgb(bytes.subspan<CUE_COLOUR_OFFSET, RGB_BYTES>()),
            .name = *name,
        });
    } else if (type == "LOOP") {
        if (bytes.size() <= LOOP_NAME_OFFSET || bytes[LOOP_LOCK_OFFSET] > 1) {
            return false;
        }
        const auto name = markerName(bytes.subspan(LOOP_NAME_OFFSET));
        const auto start = std::bit_cast<std::int32_t>(integer(bytes.subspan<2, sizeof(std::uint32_t)>()));
        const auto end
            = std::bit_cast<std::int32_t>(integer(bytes.subspan<GRID_HEADER_BYTES, sizeof(std::uint32_t)>()));
        if (!name || end < start) {
            return false;
        }
        markers.loops.push_back({
            .startMilliseconds = start,
            .endMilliseconds = end,
            .index = bytes[1],
            .colour = rgb(bytes.subspan<LOOP_COLOUR_OFFSET, RGB_BYTES>()),
            .locked = bytes[LOOP_LOCK_OFFSET] != 0,
            .name = *name,
        });
    }
    return true;
}

/// Reads a bounded ASCII record type, using an empty type for the terminating padding byte.
std::optional<juce::String> readMarkerType(Cursor& input)
{
    std::vector<std::uint8_t> name;
    for (auto character = input.byte(); character.has_value(); character = input.byte()) {
        if (*character == 0) {
            return utf8(name);
        }
        if (*character > MAX_ASCII_BYTE || name.size() >= MAX_RECORD_TYPE_BYTES) {
            return std::nullopt;
        }
        name.push_back(*character);
    }
    return std::nullopt;
}

/// Consumes only documented zero padding after the final marker record.
bool finishMarkerPadding(Cursor& input)
{
    while (!input.isExhausted()) {
        if (input.byte() != std::uint8_t {0}) {
            return false;
        }
    }
    return true;
}

/// Validates the decoded Markers2 record body independently of its Base64 envelope.
std::optional<Markers> readMarkerBody(const std::span<const std::uint8_t> payload)
{
    Cursor input(payload);
    if (input.byte() != std::uint8_t {1} || input.byte() != std::uint8_t {1}) {
        return std::nullopt;
    }
    Markers markers;
    std::size_t records = 0;
    while (true) {
        if (input.isExhausted()) {
            return records > 0 ? std::optional(markers) : std::nullopt;
        }
        const auto type = readMarkerType(input);
        if (!type) {
            return std::nullopt;
        }
        if (type->isEmpty()) {
            return finishMarkerPadding(input) ? std::optional(markers) : std::nullopt;
        }
        if (++records > MAX_TAG_RECORDS) {
            return std::nullopt;
        }
        const auto size = input.number();
        const auto record = size ? input.take(*size) : std::nullopt;
        if (!record || !addMarker(markers, *type, *record)) {
            return std::nullopt;
        }
    }
}

/// Recovers the 24-bit legacy value from its four seven-bit groups.
std::uint32_t legacyNumber(const std::span<const std::uint8_t> bytes)
{
    return ((bytes[0] & SERATO_HIGH_MASK) << (3 * SERATO_GROUP_BITS))
        | ((bytes[1] & SERATO_GROUP_MASK) << (2 * SERATO_GROUP_BITS))
        | ((bytes[2] & SERATO_GROUP_MASK) << SERATO_GROUP_BITS) | (bytes[3] & SERATO_GROUP_MASK);
}

}  // namespace

/// Strictly parses a finite ASCII number without locale dependence or ignored trailing text.
std::optional<double> parseTagNumber(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.empty() || text.size() > MAX_TAG_TEXT_BYTES || text.front() == '+'
        || text.find_first_not_of("0123456789.eE+-") != std::string_view::npos)
    {
        return std::nullopt;
    }
    std::istringstream input {std::string(text)};
    input.imbue(std::locale::classic());
    double value = 0;
    input >> std::noskipws >> value;
    // libc++ can report a range failure for representable subnormal results.
    if ((input.fail() && std::fpclassify(value) != FP_SUBNORMAL) || !input.eof() || !std::isfinite(value)) {
        return std::nullopt;
    }
    // Stream extraction can silently round an underflowing nonzero significand to zero.
    const auto significand = text.substr(0, text.find_first_of("eE"));
    if (std::fpclassify(value) == FP_ZERO
        && std::ranges::any_of(significand, [](const char character) { return character >= '1' && character <= '9'; }))
    {
        return std::nullopt;
    }
    return value;
}

/// Decodes bounded ASCII Base64, allowing whitespace and omitted padding.
/// The optional Serato workaround accepts the documented one-character remainder only for Markers2.
std::optional<juce::MemoryBlock> decodeTagBase64(
    const std::span<const std::uint8_t> bytes,
    const bool markersWorkaround
)
{
    if (bytes.size() > MAX_ENCODED_TAG_BYTES) {
        return std::nullopt;
    }
    std::string text;
    text.reserve(bytes.size());
    for (const auto byte : bytes) {
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' || byte == '\f' || byte == '\v') {
            continue;
        }
        const bool alphabet = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
            || (byte >= '0' && byte <= '9') || byte == '+' || byte == '/' || byte == '=';
        if (!alphabet) {
            return std::nullopt;
        }
        text.push_back(static_cast<char>(byte));
    }
    if (text.empty()) {
        return std::nullopt;
    }
    const auto padding = text.find('=');
    if (padding != std::string::npos
        && (text.size() % 4 != 0 || text.size() - padding > 2
            || text.find_first_not_of('=', padding) != std::string::npos))
    {
        return std::nullopt;
    }
    if (text.size() % 4 == 1) {
        if (!markersWorkaround) {
            return std::nullopt;
        }
        text.push_back('A');
    }
    while (text.size() % 4 != 0) {
        text.push_back('=');
    }
    juce::MemoryOutputStream output;
    if (!juce::Base64::convertFromBase64(output, juce::String::fromUTF8(text.data(), static_cast<int>(text.size())))
        || output.getDataSize() > MAX_DECODED_TAG_BYTES)
    {
        return std::nullopt;
    }
    return output.getMemoryBlock();
}

/// Decodes native FLAC envelopes, dropping only their final incomplete zero sextet.
/// Generic Base64 callers still reject every impossible one-character remainder.
std::optional<juce::MemoryBlock> decodeNativeTagBase64(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > MAX_ENCODED_TAG_BYTES) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> encoded;
    encoded.reserve(bytes.size());
    for (const auto byte : bytes) {
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n' && byte != '\f' && byte != '\v') {
            encoded.push_back(byte);
        }
    }
    if (encoded.size() % 4 == 1 && encoded.back() == 'A'
        && std::ranges::find(encoded, std::uint8_t {'='}) == encoded.end())
    {
        encoded.pop_back();
    }
    return decodeTagBase64(encoded);
}

/// Returns the payload of a decoded native FLAC value after its wrapper,
/// which holds the MIME type "application/octet-stream", an empty file name, and the given description,
/// each terminated by a null byte, or nothing when any of them differs or lacks its terminator.
/// The payload is a view into the decoded bytes, omitting exactly one native zero suffix when its size identifies it.
std::optional<std::span<const std::uint8_t>> parseTagWrapper(
    const std::span<const std::uint8_t> decoded,
    const std::string_view description
)
{
    std::size_t offset = 0;
    for (const auto expected : {std::string_view("application/octet-stream"), std::string_view(), description}) {
        const auto remaining = decoded.subspan(offset);
        const auto terminator = std::ranges::find(remaining, std::uint8_t {0});
        if (terminator == remaining.end()) {
            return std::nullopt;
        }
        const auto length = static_cast<std::size_t>(terminator - remaining.begin());
        if (!std::ranges::equal(remaining.first(length), expected)) {
            return std::nullopt;
        }
        offset += length + 1;
    }
    auto payload = decoded.subspan(offset);
    if (!payload.empty() && payload.back() == 0) {
        bool nativePadding = description == OVERVIEW_NAMES.description
            && payload.size() > OVERVIEW_PAYLOAD_HEADER.size() + Overview::BAND_COUNT
            && std::ranges::equal(payload.first(OVERVIEW_PAYLOAD_HEADER.size()), OVERVIEW_PAYLOAD_HEADER)
            && (payload.size() - OVERVIEW_PAYLOAD_HEADER.size()) % Overview::BAND_COUNT == 1;
        nativePadding = nativePadding || (description == ANALYSIS_NAMES.description && payload.size() == 3);
        if (description == BEAT_GRID_NAMES.description && payload.size() >= GRID_HEADER_BYTES) {
            const auto count = integer(payload.subspan<2, sizeof(std::uint32_t)>());
            nativePadding = count <= MAX_TAG_RECORDS
                && payload.size() == GRID_HEADER_BYTES + 2 + static_cast<std::size_t>(count) * GRID_RECORD_BYTES;
        }
        if (nativePadding) {
            payload = payload.first(payload.size() - 1);
        }
    }
    return payload;
}

/// Returns the FLAC comment name and the GEOB description of a tag kind, or of the legacy markers,
/// or nothing for the track information, which is read from the standard properties instead.
std::optional<TagNames> getTagNames(const TagKind kind, const bool legacyMarkers)
{
    switch (kind) {
        case TagKind::overview:
            return OVERVIEW_NAMES;
        case TagKind::analysis:
            return ANALYSIS_NAMES;
        case TagKind::autotags:
            return AUTOTAGS_NAMES;
        case TagKind::beatGrid:
            return BEAT_GRID_NAMES;
        case TagKind::markers:
            return legacyMarkers ? LEGACY_MARKERS_NAMES : MARKERS_NAMES;
        case TagKind::trackInfo:
            break;
    }
    return std::nullopt;
}

/// Borrows the ID3v2 tag of an MPEG, AIFF, WAV, or FLAC file that TagLib has opened.
/// Without create, the lookup requests no new tag and never saves or modifies the file.
/// TagLib eagerly creates an empty in-memory MPEG tag on open, so an engaged result need not imply a disk tag.
/// The tag belongs to the file and stays valid while the file is open.
utils::OptionalRef<TagLib::ID3v2::Tag> findId3Tag(TagLib::File& file, const bool create)
{
    // TagLib owns this nullable tag, which is borrowed only while the file remains open.
    TagLib::ID3v2::Tag* tag {};
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(&file)) {
        tag = mpeg->ID3v2Tag(create);
    } else if (const auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(&file)) {
        tag = create || aiff->hasID3v2Tag() ? aiff->tag() : nullptr;
    } else if (const auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(&file)) {
        tag = create || wav->hasID3v2Tag() ? wav->ID3v2Tag() : nullptr;
    } else if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(&file)) {
        tag = flac->ID3v2Tag(create);
    }
    return tag != nullptr ? utils::OptionalRef<TagLib::ID3v2::Tag>(*tag) : std::nullopt;
}

/// Parses the two-byte Serato analysis version, rejecting truncated or extended payloads.
std::optional<Analysis> parseAnalysis(const std::span<const std::uint8_t> bytes)
{
    return bytes.size() == 2 ? std::optional(Analysis {.majorVersion = bytes[0], .minorVersion = bytes[1]})
                             : std::nullopt;
}

/// Parses fixed-width or compact terminated ASCII tempo and gain fields with a supported version header.
std::optional<Autotags> parseAutotags(const std::span<const std::uint8_t> bytes)
{
    if ((bytes.size() == COMPACT_AUTOTAGS_MIN_BYTES || bytes.size() == COMPACT_AUTOTAGS_MAX_BYTES) && bytes[0] == 1
        && bytes[1] == 1)
    {
        auto remaining = bytes.subspan(2);
        std::array<double, 3> fields {};
        for (auto& field : fields) {
            const auto end = std::ranges::find(remaining, std::uint8_t {0});
            if (end == remaining.end()) {
                return std::nullopt;
            }
            const std::string text(remaining.begin(), end);
            const auto parsed = parseTagNumber(text);
            if (!parsed) {
                return std::nullopt;
            }
            field = *parsed;
            remaining = remaining.subspan(static_cast<std::size_t>(end - remaining.begin()) + 1);
        }
        if (fields[0] < 0 || remaining.size() > 1 || !allZero(remaining)) {
            return std::nullopt;
        }
        return Autotags {.bpm = fields[0], .autoGain = fields[1], .gain = fields[2]};
    }
    if ((bytes.size() != GAIN_OFFSET && bytes.size() != FULL_AUTOTAGS_BYTES) || bytes[0] != 1 || bytes[1] != 1) {
        return std::nullopt;
    }
    const auto bpm = fixedNumber(bytes.subspan(BPM_OFFSET, AUTO_GAIN_OFFSET - BPM_OFFSET));
    const auto autoGain = fixedNumber(bytes.subspan(AUTO_GAIN_OFFSET, GAIN_OFFSET - AUTO_GAIN_OFFSET));
    const auto gain
        = bytes.size() == FULL_AUTOTAGS_BYTES ? fixedNumber(bytes.subspan(GAIN_OFFSET)) : std::optional(0.0);
    if (!bpm || *bpm < 0 || !autoGain || !gain) {
        return std::nullopt;
    }
    return Autotags {.bpm = *bpm, .autoGain = *autoGain, .gain = *gain};
}

/// Parses counted big-endian grid anchors and the opaque footer. Only the final anchor carries BPM.
std::optional<BeatGrid> parseBeatGrid(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < GRID_HEADER_BYTES || bytes.size() > MAX_TAG_PAYLOAD_BYTES || bytes[0] != 1 || bytes[1] != 0) {
        return std::nullopt;
    }
    const auto count = integer(bytes.subspan<2, sizeof(std::uint32_t)>());
    const auto expected = GRID_HEADER_BYTES + static_cast<std::size_t>(count) * GRID_RECORD_BYTES;
    if (count > MAX_TAG_RECORDS || (bytes.size() != expected + 1 && (count != 0 || bytes.size() != expected))) {
        return std::nullopt;
    }
    BeatGrid grid;
    grid.markers.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto record = bytes.subspan(GRID_HEADER_BYTES + static_cast<std::size_t>(index) * GRID_RECORD_BYTES)
                                .first<GRID_RECORD_BYTES>();
        const double position
            = static_cast<double>(std::bit_cast<float>(integer(record.first<sizeof(std::uint32_t)>())));
        if (!std::isfinite(position) || (!grid.markers.empty() && position < grid.markers.back().positionSeconds)) {
            return std::nullopt;
        }
        if (index + 1 == count) {
            const double bpm = static_cast<double>(std::bit_cast<float>(integer(record.last<sizeof(std::uint32_t)>())));
            if (!std::isfinite(bpm) || bpm <= 0) {
                return std::nullopt;
            }
            grid.markers.push_back({.positionSeconds = position, .beatsUntilNext = 0, .bpm = bpm});
        } else {
            const auto beats = integer(record.last<sizeof(std::uint32_t)>());
            if (beats == 0) {
                return std::nullopt;
            }
            grid.markers.push_back({.positionSeconds = position, .beatsUntilNext = beats, .bpm = std::nullopt});
        }
    }
    return grid;
}

/// Parses the Markers2 envelope and known records, skipping bounded unknown records by their lengths.
std::optional<Markers> parseMarkers2(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 3 || bytes.size() > MAX_TAG_PAYLOAD_BYTES || bytes[0] != 1 || bytes[1] != 1) {
        return std::nullopt;
    }
    const auto envelope = bytes.subspan(2);
    const auto terminator = std::ranges::find(envelope, std::uint8_t {0});
    const auto length = static_cast<std::size_t>(terminator - envelope.begin());
    if (!allZero(envelope.subspan(length))) {
        return std::nullopt;
    }
    const auto decoded = decodeTagBase64(envelope.first(length), true);
    if (!decoded) {
        return std::nullopt;
    }
    const std::span payload {static_cast<const std::uint8_t*>(decoded->getData()), decoded->getSize()};
    return readMarkerBody(payload);
}

/// Parses the fixed legacy fourteen-slot layout as a fallback, without inventing cue names or BPM lock.
std::optional<Markers> parseLegacyMarkers(const std::span<const std::uint8_t> bytes)
{
    constexpr auto expected = GRID_HEADER_BYTES + LEGACY_COUNT * LEGACY_RECORD_BYTES + sizeof(std::uint32_t);
    if (bytes.size() != expected || bytes[0] != LEGACY_MAJOR_VERSION || bytes[1] != LEGACY_MINOR_VERSION
        || integer(bytes.subspan<2, sizeof(std::uint32_t)>()) != LEGACY_COUNT)
    {
        return std::nullopt;
    }
    Markers markers;
    std::uint8_t cueIndex = 0;
    std::uint8_t loopIndex = 0;
    for (std::size_t index = 0; index < LEGACY_COUNT; ++index) {
        const auto record = bytes.subspan(GRID_HEADER_BYTES + index * LEGACY_RECORD_BYTES, LEGACY_RECORD_BYTES);
        const auto type = record[LEGACY_TYPE_OFFSET];
        if ((record[0] != 0 && record[0] != LEGACY_UNSET) || record[LEGACY_LOCK_OFFSET] > 1) {
            return std::nullopt;
        }
        const auto slot = type == LEGACY_CUE_TYPE ? cueIndex++ : loopIndex++;
        if (record[0] == LEGACY_UNSET) {
            continue;
        }
        const auto position = legacyNumber(record.subspan(1));
        const juce::Colour colour(OPAQUE_ALPHA | legacyNumber(record.subspan(LEGACY_COLOUR_OFFSET)));
        if (type == LEGACY_CUE_TYPE) {
            markers.cues.push_back({.positionMilliseconds = position, .index = slot, .colour = colour, .name = {}});
        } else if (type == LEGACY_LOOP_TYPE) {
            if (record[LEGACY_END_OFFSET - 1] != 0) {
                return std::nullopt;
            }
            const auto start = std::bit_cast<std::int32_t>(position);
            const auto end = std::bit_cast<std::int32_t>(legacyNumber(record.subspan(LEGACY_END_OFFSET)));
            if (end < start) {
                return std::nullopt;
            }
            markers.loops.push_back({
                .startMilliseconds = start,
                .endMilliseconds = end,
                .index = slot,
                .colour = colour,
                .locked = record[LEGACY_LOCK_OFFSET] != 0,
                .name = {},
            });
        }
    }
    markers.colour = juce::Colour(OPAQUE_ALPHA | legacyNumber(bytes.last(4)));
    return markers;
}

/// Dispatches a stripped GEOB payload. Invalid data affects only the requested kind.
TagReadResult parseTagPayload(const TagKind kind, const std::span<const std::uint8_t> bytes, const bool legacyMarkers)
{
    if (bytes.size() > MAX_TAG_PAYLOAD_BYTES) {
        return TagReadResult::missing(kind, ReadStatus::invalid);
    }
    const auto present = [kind](auto parsed) {
        return parsed ? TagReadResult::present(std::move(*parsed)) : TagReadResult::missing(kind, ReadStatus::invalid);
    };
    switch (kind) {
        case TagKind::overview:
            return present(Overview::parse(bytes));
        case TagKind::analysis:
            return present(parseAnalysis(bytes));
        case TagKind::autotags:
            return present(parseAutotags(bytes));
        case TagKind::beatGrid:
            return present(parseBeatGrid(bytes));
        case TagKind::markers:
            return present(legacyMarkers ? parseLegacyMarkers(bytes) : parseMarkers2(bytes));
        case TagKind::trackInfo:
            break;
    }
    return TagReadResult::missing(kind, ReadStatus::unsupported);
}

}  // namespace audiobatch::serato
