/// Parses documented Serato payloads and validates portable per-kind cache blocks.
/// Bounds apply to encoded input, decoded data, record counts and owned UTF-8 text.
/// Also decodes the Base64 and the wrapper of native FLAC values, and finds the ID3v2 tag of an open TagLib file.

#include "serato/SeratoTags.h"

#include "serato/SeratoBytes.h"

#include <aifffile.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <flacfile.h>
#include <id3v2tag.h>
#include <locale>
#include <mpegfile.h>
#include <sstream>
#include <string>
#include <variant>
#include <wavfile.h>

namespace audiobatch::serato
{
namespace
{
constexpr std::size_t cacheHeaderBytes {16};
constexpr std::size_t bpmOffset {2};
constexpr std::size_t autoGainOffset {9};
constexpr std::size_t gainOffset {16};
constexpr std::size_t fullAutotagsBytes {22};
constexpr std::size_t gridHeaderBytes {6};
constexpr std::size_t gridRecordBytes {8};
constexpr std::size_t cueNameOffset {12};
constexpr std::size_t cueColourOffset {7};
constexpr std::size_t loopNameOffset {20};
constexpr std::size_t loopColourOffset {15};
constexpr std::size_t loopLockOffset {19};
constexpr std::size_t maxRecordTypeBytes {64};
constexpr std::size_t legacyCount {14};
constexpr std::size_t legacyRecordBytes {22};
constexpr std::size_t legacyColourOffset {16};
constexpr std::size_t legacyTypeOffset {20};
constexpr std::size_t legacyLockOffset {21};
constexpr std::size_t legacyEndOffset {6};
constexpr std::uint8_t legacyUnset {0x7f};
constexpr std::uint8_t legacyMinorVersion {5};
constexpr std::uint8_t maxAsciiByte {127};
constexpr std::uint32_t cacheMagic {0x53544147};
constexpr unsigned int seratoGroupBits {7};
constexpr std::uint32_t seratoGroupMask {0x7f};
constexpr std::uint32_t seratoHighMask {0x07};
constexpr std::uint32_t opaqueAlpha {0xff000000};

/// A fail-closed reader for cache fields.
/// Every read returns nothing instead of a default value when its field is truncated or invalid,
/// and a failed read never advances outside the supplied span.
class Cursor
{
public:
    /// Borrows an already bounded block for this synchronous decode.
    explicit Cursor(const std::span<const std::uint8_t> input) : remaining(input) {}

    /// Takes a field of the given size, or nothing when fewer bytes remain.
    std::optional<std::span<const std::uint8_t>> take(const std::size_t count)
    {
        if (count > remaining.size()) {
            return std::nullopt;
        }
        const auto bytes = remaining.first(count);
        remaining = remaining.subspan(count);
        return bytes;
    }

    /// Reads an unsigned byte.
    std::optional<std::uint8_t> byte()
    {
        return take(1).transform([](const std::span<const std::uint8_t> bytes) { return bytes.front(); });
    }

    /// Reads a strict boolean, rejecting byte values other than zero and one.
    std::optional<bool> boolean()
    {
        const auto value = byte();
        if (!value || *value > 1) {
            return std::nullopt;
        }
        return *value != 0;
    }

    /// Reads a portable four-byte unsigned integer.
    std::optional<std::uint32_t> number()
    {
        return take(sizeof(std::uint32_t)).transform([](const std::span<const std::uint8_t> bytes) {
            return readBigEndian<std::uint32_t>(bytes.first<sizeof(std::uint32_t)>());
        });
    }

    /// Reads an IEEE binary64 value, rejecting infinities and NaNs.
    std::optional<double> real()
    {
        const auto bytes = take(sizeof(std::uint64_t));
        if (!bytes) {
            return std::nullopt;
        }
        const auto value = std::bit_cast<double>(readBigEndian<std::uint64_t>(bytes->first<sizeof(std::uint64_t)>()));
        return std::isfinite(value) ? std::optional(value) : std::nullopt;
    }

    std::optional<juce::String> text();

    /// Reports whether every byte of the enclosing block has been consumed.
    [[nodiscard]] bool isExhausted() const
    {
        return remaining.empty();
    }

private:
    /// The bytes not read yet, a view into the block the cursor borrows.
    std::span<const std::uint8_t> remaining;
};

/// Checks padding without accepting unparsed trailing records or text.
bool allZero(std::span<const std::uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](auto byte) { return byte == 0; });
}

/// Returns a validated UTF-8 string of bounded length without embedded nulls.
std::optional<juce::String> utf8(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > maxTagTextBytes || std::ranges::find(bytes, std::uint8_t {0}) != bytes.end()) {
        return std::nullopt;
    }
    const std::string text(bytes.begin(), bytes.end());
    if (!juce::CharPointer_UTF8::isValidString(text.c_str(), static_cast<int>(text.size()))) {
        return std::nullopt;
    }
    return juce::String::fromUTF8(text.c_str(), static_cast<int>(text.size()));
}

/// Reads length-prefixed UTF-8, bounding the length before allocating the text.
std::optional<juce::String> Cursor::text()
{
    const auto size = number();
    if (!size || *size > maxTagTextBytes) {
        return std::nullopt;
    }
    const auto bytes = take(*size);
    return bytes ? utf8(*bytes) : std::nullopt;
}

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
juce::Colour rgb(const std::span<const std::uint8_t, rgbBytes> bytes)
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
        markers.colour = rgb(bytes.subspan<1, rgbBytes>());
    } else if (type == "CUE") {
        if (bytes.size() <= cueNameOffset) {
            return false;
        }
        const auto name = markerName(bytes.subspan(cueNameOffset));
        if (!name) {
            return false;
        }
        markers.cues.push_back({
            .positionMilliseconds = integer(bytes.subspan<2, sizeof(std::uint32_t)>()),
            .index = bytes[1],
            .colour = rgb(bytes.subspan<cueColourOffset, rgbBytes>()),
            .name = *name,
        });
    } else if (type == "LOOP") {
        if (bytes.size() <= loopNameOffset || bytes[loopLockOffset] > 1) {
            return false;
        }
        const auto name = markerName(bytes.subspan(loopNameOffset));
        const auto start = integer(bytes.subspan<2, sizeof(std::uint32_t)>());
        const auto end = integer(bytes.subspan<gridHeaderBytes, sizeof(std::uint32_t)>());
        if (!name || end < start) {
            return false;
        }
        markers.loops.push_back({
            .startMilliseconds = start,
            .endMilliseconds = end,
            .index = bytes[1],
            .colour = rgb(bytes.subspan<loopColourOffset, rgbBytes>()),
            .locked = bytes[loopLockOffset] != 0,
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
        if (*character > maxAsciiByte || name.size() >= maxRecordTypeBytes) {
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
        const auto type = readMarkerType(input);
        if (!type) {
            return std::nullopt;
        }
        if (type->isEmpty()) {
            return finishMarkerPadding(input) ? std::optional(markers) : std::nullopt;
        }
        if (++records > maxTagRecords) {
            return std::nullopt;
        }
        const auto size = input.number();
        const auto record = size ? input.take(*size) : std::nullopt;
        if (!record || !addMarker(markers, *type, *record)) {
            return std::nullopt;
        }
    }
}

/// Reads counted cache grid anchors while checking ordering and terminal versus intermediate semantics.
std::optional<BeatGrid> readCacheGrid(Cursor& input)
{
    const auto count = input.number();
    if (!count || *count > maxTagRecords) {
        return std::nullopt;
    }
    BeatGrid grid;
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto position = input.real();
        const auto beatsUntilNext = input.number();
        const auto hasBpm = input.boolean();
        if (!position || !beatsUntilNext || !hasBpm) {
            return std::nullopt;
        }
        BeatGridMarker marker {.positionSeconds = *position, .beatsUntilNext = *beatsUntilNext, .bpm = std::nullopt};
        if (*hasBpm) {
            marker.bpm = input.real();
            if (!marker.bpm) {
                return std::nullopt;
            }
        }
        const bool ordered = grid.markers.empty() || marker.positionSeconds >= grid.markers.back().positionSeconds;
        const bool consistent = index + 1 == *count ? marker.bpm && *marker.bpm > 0 && marker.beatsUntilNext == 0
                                                    : !marker.bpm && marker.beatsUntilNext > 0;
        if (!ordered || !consistent) {
            return std::nullopt;
        }
        grid.markers.push_back(marker);
    }
    return grid;
}

/// Writes binary64 bits rather than native floating point layout.
void writeReal(juce::MemoryOutputStream& output, const double value)
{
    output.writeInt64BigEndian(std::bit_cast<std::int64_t>(value));
}

/// Writes a bounded length-prefixed UTF-8 value without the C string terminator.
void writeText(juce::MemoryOutputStream& output, const juce::String& value)
{
    output.writeIntBigEndian(static_cast<int>(value.getNumBytesAsUTF8()));
    output.write(value.toRawUTF8(), value.getNumBytesAsUTF8());
}

/// Verifies public marker sizes before any serialization allocation.
bool boundedMarkers(const Markers& markers)
{
    if (markers.cues.size() > maxTagRecords || markers.loops.size() > maxTagRecords) {
        return false;
    }
    std::size_t total = cacheHeaderBytes;
    for (const auto& cue : markers.cues) {
        const auto size = cue.name.getNumBytesAsUTF8();
        if (size > maxTagTextBytes) {
            return false;
        }
        total += size + cueNameOffset;
    }
    for (const auto& loop : markers.loops) {
        const auto size = loop.name.getNumBytesAsUTF8();
        if (size > maxTagTextBytes) {
            return false;
        }
        total += size + loopNameOffset;
    }
    return total <= maxTagPayloadBytes;
}

/// Serializes counted marker records with explicit flags for optional track metadata.
void writeMarkers(juce::MemoryOutputStream& output, const Markers& markers)
{
    output.writeIntBigEndian(static_cast<int>(markers.cues.size()));
    for (const auto& [positionMilliseconds, index, colour, name] : markers.cues) {
        output.writeIntBigEndian(std::bit_cast<int>(positionMilliseconds));
        output.writeByte(static_cast<char>(index));
        output.writeIntBigEndian(std::bit_cast<int>(colour.getARGB()));
        writeText(output, name);
    }
    output.writeIntBigEndian(static_cast<int>(markers.loops.size()));
    for (const auto& [startMilliseconds, endMilliseconds, index, colour, locked, name] : markers.loops) {
        output.writeIntBigEndian(std::bit_cast<int>(startMilliseconds));
        output.writeIntBigEndian(std::bit_cast<int>(endMilliseconds));
        output.writeByte(static_cast<char>(index));
        output.writeIntBigEndian(std::bit_cast<int>(colour.getARGB()));
        output.writeBool(locked);
        writeText(output, name);
    }
    output.writeBool(markers.colour.has_value());
    if (markers.colour) {
        output.writeIntBigEndian(std::bit_cast<int>(markers.colour->getARGB()));
    }
    output.writeBool(markers.bpmLocked.has_value());
    if (markers.bpmLocked) {
        output.writeBool(*markers.bpmLocked);
    }
}

/// Reads cache markers without trusting counts or allowing reversed loop boundaries.
std::optional<Markers> readMarkers(Cursor& input)
{
    Markers markers;
    const auto cueCount = input.number();
    if (!cueCount || *cueCount > maxTagRecords) {
        return std::nullopt;
    }
    for (std::uint32_t index = 0; index < *cueCount; ++index) {
        const auto position = input.number();
        const auto slot = input.byte();
        const auto colour = input.number();
        const auto name = input.text();
        if (!position || !slot || !colour || !name) {
            return std::nullopt;
        }
        markers.cues.push_back(
            {.positionMilliseconds = *position, .index = *slot, .colour = juce::Colour(*colour), .name = *name}
        );
    }
    const auto loopCount = input.number();
    if (!loopCount || *loopCount > maxTagRecords) {
        return std::nullopt;
    }
    for (std::uint32_t index = 0; index < *loopCount; ++index) {
        const auto start = input.number();
        const auto end = input.number();
        const auto slot = input.byte();
        const auto colour = input.number();
        const auto locked = input.boolean();
        const auto name = input.text();
        if (!start || !end || !slot || !colour || !locked || !name || *end < *start) {
            return std::nullopt;
        }
        markers.loops.push_back({
            .startMilliseconds = *start,
            .endMilliseconds = *end,
            .index = *slot,
            .colour = juce::Colour(*colour),
            .locked = *locked,
            .name = *name,
        });
    }
    const auto hasColour = input.boolean();
    if (!hasColour) {
        return std::nullopt;
    }
    if (*hasColour) {
        const auto colour = input.number();
        if (!colour) {
            return std::nullopt;
        }
        markers.colour = juce::Colour(*colour);
    }
    const auto hasBpmLock = input.boolean();
    if (!hasBpmLock) {
        return std::nullopt;
    }
    if (*hasBpmLock) {
        markers.bpmLocked = input.boolean();
        if (!markers.bpmLocked) {
            return std::nullopt;
        }
    }
    return markers;
}

/// Reads cache track information: an optional positive BPM followed by the key, album, genre, year, and comment texts,
/// requiring at least one field to be set.
std::optional<TrackInfo> readTrackInfo(Cursor& input)
{
    const auto hasBpm = input.boolean();
    if (!hasBpm) {
        return std::nullopt;
    }
    TrackInfo info;
    if (*hasBpm) {
        info.bpm = input.real();
        if (!info.bpm || *info.bpm <= 0) {
            return std::nullopt;
        }
    }
    for (auto* field : {&info.key, &info.album, &info.genre, &info.year, &info.comment}) {
        const auto text = input.text();
        if (!text) {
            return std::nullopt;
        }
        *field = *text;
    }
    if (info.isEmpty()) {
        return std::nullopt;
    }
    return info;
}

/// Recovers the 24-bit legacy value from its four seven-bit groups.
std::uint32_t legacyNumber(const std::span<const std::uint8_t> bytes)
{
    return ((bytes[0] & seratoHighMask) << (3 * seratoGroupBits))
        | ((bytes[1] & seratoGroupMask) << (2 * seratoGroupBits)) | ((bytes[2] & seratoGroupMask) << seratoGroupBits)
        | (bytes[3] & seratoGroupMask);
}

/// Writes a non-Overview cache payload after checking its public collection and text bounds.
bool writeCachePayload(juce::MemoryOutputStream& payload, const TagValue& value)
{
    switch (kindOf(value)) {
        case TagKind::analysis: {
            const auto& analysis = std::get<Analysis>(value);
            payload.writeByte(static_cast<char>(analysis.majorVersion));
            payload.writeByte(static_cast<char>(analysis.minorVersion));
            return true;
        }
        case TagKind::autotags: {
            const auto& autotags = std::get<Autotags>(value);
            writeReal(payload, autotags.bpm);
            writeReal(payload, autotags.autoGain);
            writeReal(payload, autotags.gain);
            return true;
        }
        case TagKind::beatGrid: {
            const auto& grid = std::get<BeatGrid>(value);
            if (grid.markers.size() > maxTagRecords) {
                return false;
            }
            payload.writeIntBigEndian(static_cast<int>(grid.markers.size()));
            for (const auto& [positionSeconds, beatsUntilNext, bpm] : grid.markers) {
                writeReal(payload, positionSeconds);
                payload.writeIntBigEndian(std::bit_cast<int>(beatsUntilNext));
                payload.writeBool(bpm.has_value());
                if (bpm) {
                    writeReal(payload, *bpm);
                }
            }
            return true;
        }
        case TagKind::markers: {
            const auto& markers = std::get<Markers>(value);
            if (!boundedMarkers(markers)) {
                return false;
            }
            writeMarkers(payload, markers);
            return true;
        }
        case TagKind::trackInfo: {
            const auto& info = std::get<TrackInfo>(value);
            const std::array texts {&info.key, &info.album, &info.genre, &info.year, &info.comment};
            if (std::ranges::any_of(
                    texts, [](const auto* text) { return text->getNumBytesAsUTF8() > maxTagTextBytes; }
                ))
            {
                return false;
            }
            payload.writeBool(info.bpm.has_value());
            if (info.bpm) {
                writeReal(payload, *info.bpm);
            }
            for (const auto* text : texts) {
                writeText(payload, *text);
            }
            return true;
        }
        case TagKind::overview:
            return false;
    }
    return false;
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
    if (text.empty() || text.size() > maxTagTextBytes || text.front() == '+'
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
    if (bytes.size() > maxEncodedTagBytes) {
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
        || output.getDataSize() > maxDecodedTagBytes)
    {
        return std::nullopt;
    }
    return output.getMemoryBlock();
}

/// Returns the payload of a decoded native FLAC value after its wrapper,
/// which holds the MIME type "application/octet-stream", an empty file name, and the given description,
/// each terminated by a null byte, or nothing when any of them differs or lacks its terminator.
/// The payload is a view into the decoded bytes.
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
    return decoded.subspan(offset);
}

/// Returns the FLAC comment name and the GEOB description of a tag kind, or of the legacy markers,
/// or nothing for the track information, which is read from the standard properties instead.
std::optional<TagNames> getTagNames(const TagKind kind, const bool legacyMarkers)
{
    switch (kind) {
        case TagKind::overview:
            return overviewNames;
        case TagKind::analysis:
            return analysisNames;
        case TagKind::autotags:
            return autotagsNames;
        case TagKind::beatGrid:
            return beatGridNames;
        case TagKind::markers:
            return legacyMarkers ? legacyMarkersNames : markersNames;
        case TagKind::trackInfo:
            break;
    }
    return std::nullopt;
}

/// Finds the ID3v2 tag of an MPEG, AIFF, WAV, or FLAC file that TagLib has opened.
/// Without create, a file without an ID3v2 tag returns null instead of getting an empty tag,
/// so reading a file never adds one.
/// The tag belongs to the file and stays valid while the file is open.
TagLib::ID3v2::Tag* findId3Tag(TagLib::File& file, const bool create)
{
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(&file)) {
        return mpeg->ID3v2Tag(create);
    }
    if (const auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(&file)) {
        return create || aiff->hasID3v2Tag() ? aiff->tag() : nullptr;
    }
    if (const auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(&file)) {
        return create || wav->hasID3v2Tag() ? wav->ID3v2Tag() : nullptr;
    }
    if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(&file)) {
        return flac->ID3v2Tag(create);
    }
    return nullptr;
}

/// Parses the two-byte Serato analysis version, rejecting truncated or extended payloads.
std::optional<Analysis> parseAnalysis(const std::span<const std::uint8_t> bytes)
{
    return bytes.size() == 2 ? std::optional(Analysis {.majorVersion = bytes[0], .minorVersion = bytes[1]})
                             : std::nullopt;
}

/// Parses fixed-width ASCII tempo and gain fields with a supported version header.
std::optional<Autotags> parseAutotags(const std::span<const std::uint8_t> bytes)
{
    if ((bytes.size() != gainOffset && bytes.size() != fullAutotagsBytes) || bytes[0] != 1 || bytes[1] != 1) {
        return std::nullopt;
    }
    const auto bpm = fixedNumber(bytes.subspan(bpmOffset, autoGainOffset - bpmOffset));
    const auto autoGain = fixedNumber(bytes.subspan(autoGainOffset, gainOffset - autoGainOffset));
    const auto gain = bytes.size() == fullAutotagsBytes ? fixedNumber(bytes.subspan(gainOffset)) : std::optional(0.0);
    if (!bpm || *bpm < 0 || !autoGain || !gain) {
        return std::nullopt;
    }
    return Autotags {.bpm = *bpm, .autoGain = *autoGain, .gain = *gain};
}

/// Parses counted big-endian grid anchors and the opaque footer. Only the final anchor carries BPM.
std::optional<BeatGrid> parseBeatGrid(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < gridHeaderBytes || bytes.size() > maxTagPayloadBytes || bytes[0] != 1 || bytes[1] != 0) {
        return std::nullopt;
    }
    const auto count = integer(bytes.subspan<2, sizeof(std::uint32_t)>());
    const auto expected = gridHeaderBytes + static_cast<std::size_t>(count) * gridRecordBytes;
    if (count > maxTagRecords || (bytes.size() != expected + 1 && (count != 0 || bytes.size() != expected))) {
        return std::nullopt;
    }
    BeatGrid grid;
    grid.markers.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto record = bytes.subspan(gridHeaderBytes + static_cast<std::size_t>(index) * gridRecordBytes)
                                .first<gridRecordBytes>();
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
    if (bytes.size() < 3 || bytes.size() > maxTagPayloadBytes || bytes[0] != 1 || bytes[1] != 1) {
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
    constexpr auto expected = gridHeaderBytes + legacyCount * legacyRecordBytes + sizeof(std::uint32_t);
    if (bytes.size() != expected || bytes[0] != 2 || bytes[1] != legacyMinorVersion
        || integer(bytes.subspan<2, sizeof(std::uint32_t)>()) != legacyCount)
    {
        return std::nullopt;
    }
    Markers markers;
    std::uint8_t cueIndex = 0;
    std::uint8_t loopIndex = 0;
    for (std::size_t index = 0; index < legacyCount; ++index) {
        const auto record = bytes.subspan(gridHeaderBytes + index * legacyRecordBytes, legacyRecordBytes);
        const auto type = record[legacyTypeOffset];
        if ((record[0] != 0 && record[0] != legacyUnset) || record[legacyLockOffset] > 1) {
            return std::nullopt;
        }
        const auto slot = type == 1 ? cueIndex++ : loopIndex++;
        if (record[0] == legacyUnset) {
            continue;
        }
        const auto position = legacyNumber(record.subspan(1));
        const juce::Colour colour(opaqueAlpha | legacyNumber(record.subspan(legacyColourOffset)));
        if (type == 1) {
            markers.cues.push_back({.positionMilliseconds = position, .index = slot, .colour = colour, .name = {}});
        } else if (type == 3) {
            if (record[legacyEndOffset - 1] != 0) {
                return std::nullopt;
            }
            const auto end = legacyNumber(record.subspan(legacyEndOffset));
            if (end < position) {
                return std::nullopt;
            }
            markers.loops.push_back({
                .startMilliseconds = position,
                .endMilliseconds = end,
                .index = slot,
                .colour = colour,
                .locked = record[legacyLockOffset] != 0,
                .name = {},
            });
        }
    }
    markers.colour = juce::Colour(opaqueAlpha | legacyNumber(bytes.last(4)));
    return markers;
}

/// Dispatches a stripped GEOB payload. Invalid data affects only the requested kind.
TagReadResult parseTagPayload(const TagKind kind, const std::span<const std::uint8_t> bytes, const bool legacyMarkers)
{
    if (bytes.size() > maxTagPayloadBytes) {
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

/// Encodes a present result using a bounded, versioned numeric representation.
/// Overview delegates to its existing serializer. Missing, mismatched, or invalid values return an empty block.
juce::MemoryBlock serializeTag(const TagReadResult& result)
{
    if (!result.value || !supportsTagKind(result.kind) || kindOf(*result.value) != result.kind) {
        return {};
    }
    if (const auto* overview = std::get_if<Overview>(&*result.value)) {
        return overview->serialize();
    }
    juce::MemoryOutputStream payload;
    if (!writeCachePayload(payload, *result.value)) {
        return {};
    }
    if (payload.getDataSize() > maxTagPayloadBytes) {
        return {};
    }
    juce::MemoryOutputStream output;
    output.writeIntBigEndian(static_cast<int>(cacheMagic));
    output.writeIntBigEndian(std::to_underlying(result.kind));
    output.writeIntBigEndian(static_cast<int>(parserVersion(result.kind)));
    output.writeIntBigEndian(static_cast<int>(payload.getDataSize()));
    output.write(payload.getData(), payload.getDataSize());
    const auto block = output.getMemoryBlock();
    return deserializeTag(result.kind, block) ? block : juce::MemoryBlock();
}

/// Validates kind, version, exact length, counts, UTF-8 and finite numeric values before returning owned data.
/// Overview delegates to its existing deserializer. Corrupt data is a cache miss.
std::optional<TagReadResult> deserializeTag(TagKind kind, const juce::MemoryBlock& block)
{
    const std::span bytes {static_cast<const std::uint8_t*>(block.getData()), block.getSize()};
    if (kind == TagKind::overview) {
        auto overview = Overview::deserialize(bytes);
        return overview ? std::optional(TagReadResult::present(std::move(*overview))) : std::nullopt;
    }
    if (!supportsTagKind(kind) || bytes.size() < cacheHeaderBytes
        || bytes.size() > cacheHeaderBytes + maxTagPayloadBytes)
    {
        return std::nullopt;
    }
    Cursor input(bytes);
    if (input.number() != cacheMagic || input.number() != std::to_underlying(kind)
        || input.number() != parserVersion(kind) || input.number() != bytes.size() - cacheHeaderBytes)
    {
        return std::nullopt;
    }
    std::optional<TagValue> value;
    switch (kind) {
        case TagKind::analysis: {
            const auto majorVersion = input.byte();
            const auto minorVersion = input.byte();
            if (majorVersion && minorVersion) {
                value = Analysis {.majorVersion = *majorVersion, .minorVersion = *minorVersion};
            }
            break;
        }
        case TagKind::autotags: {
            const auto bpm = input.real();
            const auto autoGain = input.real();
            const auto gain = input.real();
            if (bpm && autoGain && gain && *bpm >= 0) {
                value = Autotags {.bpm = *bpm, .autoGain = *autoGain, .gain = *gain};
            }
            break;
        }
        case TagKind::beatGrid:
            value = readCacheGrid(input);
            break;
        case TagKind::markers:
            value = readMarkers(input);
            break;
        case TagKind::trackInfo:
            value = readTrackInfo(input);
            break;
        case TagKind::overview:
            return std::nullopt;
    }
    return input.isExhausted() && value ? std::optional(TagReadResult::present(std::move(*value))) : std::nullopt;
}
}  // namespace audiobatch::serato
