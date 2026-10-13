/// Parses documented Serato payloads and validates portable per-kind cache blocks.
/// Bounds apply to encoded input, decoded data, record counts and owned UTF-8 text.

#include "serato/SeratoTags.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>

namespace audiobatch::serato
{
namespace
{
constexpr std::size_t wrapperAllowance = 1024;
constexpr std::size_t maxDecodedBytes = maxTagPayloadBytes + wrapperAllowance;
constexpr std::size_t maxEncodedBytes = ((maxDecodedBytes + 2) / 3) * 4 + 4096;
constexpr std::size_t cacheHeaderBytes = 16;
constexpr std::size_t bpmOffset = 2;
constexpr std::size_t autoGainOffset = 9;
constexpr std::size_t gainOffset = 16;
constexpr std::size_t fullAutotagsBytes = 22;
constexpr std::size_t gridHeaderBytes = 6;
constexpr std::size_t gridRecordBytes = 8;
constexpr std::size_t cueNameOffset = 12;
constexpr std::size_t cueColourOffset = 7;
constexpr std::size_t loopNameOffset = 20;
constexpr std::size_t loopColourOffset = 15;
constexpr std::size_t loopLockOffset = 19;
constexpr std::size_t maxRecordTypeBytes = 64;
constexpr std::size_t legacyCount = 14;
constexpr std::size_t legacyRecordBytes = 22;
constexpr std::size_t legacyColourOffset = 16;
constexpr std::size_t legacyTypeOffset = 20;
constexpr std::size_t legacyLockOffset = 21;
constexpr std::size_t legacyEndOffset = 6;
constexpr std::uint8_t legacyUnset = 0x7f;
constexpr std::uint8_t legacyMinorVersion = 5;
constexpr std::uint8_t maxAsciiByte = 127;
constexpr std::uint32_t cacheMagic = 0x53544147;
constexpr unsigned int seratoGroupBits = 7;
constexpr std::uint32_t seratoGroupMask = 0x7f;
constexpr std::uint32_t seratoHighMask = 0x07;
constexpr std::uint32_t opaqueAlpha = 0xff000000;

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

/// Decodes a length-checked big-endian integer without alignment assumptions.
std::uint32_t integer(const std::span<const std::uint8_t> bytes)
{
    return juce::ByteOrder::bigEndianInt(bytes.data());
}

/// Decodes opaque-alpha Serato RGB bytes into an opaque display colour.
juce::Colour rgb(std::span<const std::uint8_t> bytes)
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
        markers.colour = rgb(bytes.subspan(1));
    } else if (type == "CUE") {
        if (bytes.size() <= cueNameOffset) {
            return false;
        }
        const auto name = markerName(bytes.subspan(cueNameOffset));
        if (!name) {
            return false;
        }
        markers.cues.push_back({
            .positionMilliseconds = integer(bytes.subspan(2)),
            .index = bytes[1],
            .colour = rgb(bytes.subspan(cueColourOffset)),
            .name = *name,
        });
    } else if (type == "LOOP") {
        if (bytes.size() <= loopNameOffset || bytes[loopLockOffset] > 1) {
            return false;
        }
        const auto name = markerName(bytes.subspan(loopNameOffset));
        const auto start = integer(bytes.subspan(2));
        const auto end = integer(bytes.subspan(gridHeaderBytes));
        if (!name || end < start) {
            return false;
        }
        markers.loops.push_back({
            .startMilliseconds = start,
            .endMilliseconds = end,
            .index = bytes[1],
            .colour = rgb(bytes.subspan(loopColourOffset)),
            .locked = bytes[loopLockOffset] != 0,
            .name = *name,
        });
    }
    return true;
}

/// A fail-closed cursor for cache fields. Failed reads never advance outside the supplied span.
struct Cursor {
    /// Borrows an already bounded block for this synchronous decode.
    explicit Cursor(const std::span<const std::uint8_t> input) : remaining(input) {}
    /// Takes a bounded field, remembering truncation for the final validity check.
    std::span<const std::uint8_t> take(const std::size_t count)
    {
        if (count > remaining.size()) {
            valid = false;
            return {};
        }
        const auto bytes = remaining.first(count);
        remaining = remaining.subspan(count);
        return bytes;
    }
    /// Reads an unsigned byte with a safe default after truncation.
    std::uint8_t byte()
    {
        const auto bytes = take(1);
        return bytes.empty() ? 0 : bytes[0];
    }
    /// Reads a strict boolean, rejecting other byte values.
    bool boolean()
    {
        const auto value = byte();
        valid = valid && value <= 1;
        return value != 0;
    }
    /// Reads a portable four-byte unsigned integer.
    std::uint32_t number()
    {
        const auto bytes = take(sizeof(std::uint32_t));
        return bytes.empty() ? 0 : integer(bytes);
    }
    /// Reads an IEEE binary64 value and rejects infinities and NaNs.
    double real()
    {
        const auto bytes = take(sizeof(std::uint64_t));
        const auto value = bytes.empty() ? 0.0 : std::bit_cast<double>(juce::ByteOrder::bigEndianInt64(bytes.data()));
        valid = valid && std::isfinite(value);
        return value;
    }
    /// Reads length-prefixed UTF-8 while bounding text before allocation.
    juce::String text()
    {
        const auto size = number();
        if (size > maxTagTextBytes) {
            valid = false;
            return {};
        }
        const auto value = utf8(take(size));
        valid = valid && value.has_value();
        return value.value_or(juce::String());
    }
    /// Reports whether every field and the exact enclosing length were consumed successfully.
    [[nodiscard]] bool finished() const
    {
        return valid && remaining.empty();
    }

    bool valid = true;
    std::span<const std::uint8_t> remaining;
};

/// Reads a bounded ASCII record type, using an empty type for the terminating padding byte.
std::optional<juce::String> readMarkerType(Cursor& input)
{
    std::vector<std::uint8_t> name;
    auto character = input.byte();
    while (character != 0 && input.valid && name.size() < maxRecordTypeBytes) {
        if (character > maxAsciiByte) {
            return std::nullopt;
        }
        name.push_back(character);
        character = input.byte();
    }
    return input.valid && character == 0 ? utf8(name) : std::nullopt;
}

/// Consumes only documented zero padding after the final marker record.
bool finishMarkerPadding(Cursor& input)
{
    while (!input.finished() && input.valid) {
        if (input.byte() != 0) {
            return false;
        }
    }
    return input.finished();
}

/// Validates the decoded Markers2 record body independently of its Base64 envelope.
std::optional<Markers> readMarkerBody(const std::span<const std::uint8_t> payload)
{
    Cursor input(payload);
    if (input.byte() != 1 || input.byte() != 1 || !input.valid) {
        return std::nullopt;
    }
    Markers markers;
    std::size_t records = 0;
    while (input.valid) {
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
        const auto record = input.take(size);
        if (!input.valid || !addMarker(markers, *type, record)) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

/// Reads counted cache grid anchors while checking ordering and terminal versus intermediate semantics.
BeatGrid readCacheGrid(Cursor& input)
{
    BeatGrid grid;
    const auto count = input.number();
    if (count > maxTagRecords) {
        input.valid = false;
        return grid;
    }
    for (std::uint32_t index = 0; index < count && input.valid; ++index) {
        BeatGridMarker marker {.positionSeconds = input.real(), .beatsUntilNext = input.number(), .bpm = std::nullopt};
        if (input.boolean()) {
            marker.bpm = input.real();
        }
        input.valid = input.valid
            && (grid.markers.empty() || marker.positionSeconds >= grid.markers.back().positionSeconds)
            && (index + 1 == count ? marker.bpm && *marker.bpm > 0 && marker.beatsUntilNext == 0
                                   : !marker.bpm && marker.beatsUntilNext > 0);
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
Markers readMarkers(Cursor& input)
{
    Markers markers;
    const auto cueCount = input.number();
    if (cueCount > maxTagRecords) {
        input.valid = false;
        return markers;
    }
    for (std::uint32_t index = 0; index < cueCount && input.valid; ++index) {
        const auto position = input.number();
        const auto slot = input.byte();
        const juce::Colour colour(input.number());
        markers.cues.push_back(
            {.positionMilliseconds = position, .index = slot, .colour = colour, .name = input.text()}
        );
    }
    const auto loopCount = input.number();
    if (loopCount > maxTagRecords) {
        input.valid = false;
        return markers;
    }
    for (std::uint32_t index = 0; index < loopCount && input.valid; ++index) {
        const auto start = input.number();
        const auto end = input.number();
        const auto slot = input.byte();
        const juce::Colour colour(input.number());
        const auto locked = input.boolean();
        markers.loops.push_back({
            .startMilliseconds = start,
            .endMilliseconds = end,
            .index = slot,
            .colour = colour,
            .locked = locked,
            .name = input.text(),
        });
        input.valid = input.valid && end >= start;
    }
    if (input.boolean()) {
        markers.colour = juce::Colour(input.number());
    }
    if (input.boolean()) {
        markers.bpmLocked = input.boolean();
    }
    return markers;
}

/// Recovers the 24-bit legacy value from its four seven-bit groups.
std::uint32_t legacyNumber(const std::span<const std::uint8_t> bytes)
{
    return ((bytes[0] & seratoHighMask) << (3 * seratoGroupBits))
        | ((bytes[1] & seratoGroupMask) << (2 * seratoGroupBits)) | ((bytes[2] & seratoGroupMask) << seratoGroupBits)
        | (bytes[3] & seratoGroupMask);
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
    if (bytes.size() > maxEncodedBytes) {
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
        || output.getDataSize() > maxDecodedBytes)
    {
        return std::nullopt;
    }
    return output.getMemoryBlock();
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
    const auto count = integer(bytes.subspan(2));
    const auto expected = gridHeaderBytes + static_cast<std::size_t>(count) * gridRecordBytes;
    if (count > maxTagRecords || (bytes.size() != expected + 1 && (count != 0 || bytes.size() != expected))) {
        return std::nullopt;
    }
    BeatGrid grid;
    grid.markers.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto record
            = bytes.subspan(gridHeaderBytes + static_cast<std::size_t>(index) * gridRecordBytes, gridRecordBytes);
        const double position = static_cast<double>(std::bit_cast<float>(integer(record)));
        if (!std::isfinite(position) || (!grid.markers.empty() && position < grid.markers.back().positionSeconds)) {
            return std::nullopt;
        }
        if (index + 1 == count) {
            const double bpm = static_cast<double>(std::bit_cast<float>(integer(record.subspan(4))));
            if (!std::isfinite(bpm) || bpm <= 0) {
                return std::nullopt;
            }
            grid.markers.push_back({.positionSeconds = position, .beatsUntilNext = 0, .bpm = bpm});
        } else {
            const auto beats = integer(record.subspan(4));
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
        || integer(bytes.subspan(2)) != legacyCount)
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
    TagReadResult result;
    result.kind = kind;
    result.status = ReadStatus::invalid;
    if (bytes.size() > maxTagPayloadBytes) {
        return result;
    }
    switch (kind) {
        case TagKind::overview:
            result.overview = Overview::parse(bytes);
            break;
        case TagKind::analysis:
            result.analysis = parseAnalysis(bytes);
            break;
        case TagKind::autotags:
            result.autotags = parseAutotags(bytes);
            break;
        case TagKind::beatGrid:
            result.beatGrid = parseBeatGrid(bytes);
            break;
        case TagKind::markers:
            result.markers = legacyMarkers ? parseLegacyMarkers(bytes) : parseMarkers2(bytes);
            break;
        case TagKind::trackInfo:
            result.status = ReadStatus::unsupported;
            return result;
    }
    if (!supportsTagKind(kind)) {
        result.status = ReadStatus::unsupported;
    } else if (result.overview || result.analysis || result.autotags || result.beatGrid || result.markers) {
        result.status = ReadStatus::present;
    }
    return result;
}

namespace
{
/// Writes a matching non-Overview cache payload after checking its public collection and text bounds.
bool writeCachePayload(juce::MemoryOutputStream& payload, const TagReadResult& result)
{
    switch (result.kind) {
        case TagKind::analysis:
            if (!result.analysis) {
                return false;
            }
            payload.writeByte(static_cast<char>(result.analysis->majorVersion));
            payload.writeByte(static_cast<char>(result.analysis->minorVersion));
            return true;
        case TagKind::autotags:
            if (!result.autotags) {
                return false;
            }
            writeReal(payload, result.autotags->bpm);
            writeReal(payload, result.autotags->autoGain);
            writeReal(payload, result.autotags->gain);
            return true;
        case TagKind::beatGrid:
            if (!result.beatGrid || result.beatGrid->markers.size() > maxTagRecords) {
                return false;
            }
            payload.writeIntBigEndian(static_cast<int>(result.beatGrid->markers.size()));
            for (const auto& [positionSeconds, beatsUntilNext, bpm] : result.beatGrid->markers) {
                writeReal(payload, positionSeconds);
                payload.writeIntBigEndian(std::bit_cast<int>(beatsUntilNext));
                payload.writeBool(bpm.has_value());
                if (bpm) {
                    writeReal(payload, *bpm);
                }
            }
            return true;
        case TagKind::markers:
            if (!result.markers || !boundedMarkers(*result.markers)) {
                return false;
            }
            writeMarkers(payload, *result.markers);
            return true;
        case TagKind::trackInfo:
            if (!result.trackInfo || result.trackInfo->key.getNumBytesAsUTF8() > maxTagTextBytes) {
                return false;
            }
            payload.writeBool(result.trackInfo->bpm.has_value());
            if (result.trackInfo->bpm) {
                writeReal(payload, *result.trackInfo->bpm);
            }
            writeText(payload, result.trackInfo->key);
            return true;
        case TagKind::overview:
            return false;
    }
    return false;
}
}  // namespace

/// Encodes a present result using a bounded, versioned numeric representation.
/// Overview delegates to its existing serializer. Invalid or mismatched public values return an empty block.
juce::MemoryBlock serializeTag(const TagReadResult& result)
{
    const auto values = static_cast<unsigned int>(result.overview.has_value())
        + static_cast<unsigned int>(result.analysis.has_value())
        + static_cast<unsigned int>(result.autotags.has_value())
        + static_cast<unsigned int>(result.beatGrid.has_value()) + static_cast<unsigned int>(result.markers.has_value())
        + static_cast<unsigned int>(result.trackInfo.has_value());
    if (result.status != ReadStatus::present || values != 1 || !supportsTagKind(result.kind)) {
        return {};
    }
    if (result.kind == TagKind::overview) {
        return result.overview ? result.overview->serialize() : juce::MemoryBlock();
    }
    juce::MemoryOutputStream payload;
    if (!writeCachePayload(payload, result)) {
        return {};
    }
    if (payload.getDataSize() > maxTagPayloadBytes) {
        return {};
    }
    juce::MemoryOutputStream output;
    output.writeIntBigEndian(static_cast<int>(cacheMagic));
    output.writeIntBigEndian(static_cast<int>(result.kind));
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
    TagReadResult result;
    result.kind = kind;
    result.status = ReadStatus::present;
    if (kind == TagKind::overview) {
        result.overview = Overview::deserialize(bytes);
        return result.overview ? std::optional(result) : std::nullopt;
    }
    if (!supportsTagKind(kind) || bytes.size() < cacheHeaderBytes
        || bytes.size() > cacheHeaderBytes + maxTagPayloadBytes)
    {
        return std::nullopt;
    }
    Cursor input(bytes);
    if (input.number() != cacheMagic || input.number() != static_cast<std::uint32_t>(kind)
        || input.number() != parserVersion(kind) || input.number() != bytes.size() - cacheHeaderBytes)
    {
        return std::nullopt;
    }
    switch (kind) {
        case TagKind::analysis:
            result.analysis = Analysis {.majorVersion = input.byte(), .minorVersion = input.byte()};
            break;
        case TagKind::autotags:
            result.autotags = Autotags {.bpm = input.real(), .autoGain = input.real(), .gain = input.real()};
            input.valid = input.valid && result.autotags->bpm >= 0;
            break;
        case TagKind::beatGrid:
            result.beatGrid = readCacheGrid(input);
            break;
        case TagKind::markers:
            result.markers = readMarkers(input);
            break;
        case TagKind::trackInfo:
            result.trackInfo.emplace();
            if (input.boolean()) {
                result.trackInfo->bpm = input.real();
            }
            result.trackInfo->key = input.text();
            input.valid = input.valid && (!result.trackInfo->bpm || *result.trackInfo->bpm > 0)
                && (result.trackInfo->bpm || result.trackInfo->key.isNotEmpty());
            break;
        case TagKind::overview:
            return std::nullopt;
    }
    return input.finished() ? std::optional(result) : std::nullopt;
}
}  // namespace audiobatch::serato
