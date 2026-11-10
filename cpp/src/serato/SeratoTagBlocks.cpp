/// Encodes and decodes the portable per-kind cache blocks of the Serato tags.
/// A block holds a magic number, the tag kind, the parser version and the payload length,
/// followed by a bounded payload that decoding validates field by field.
/// A block that fails any check is a cache miss instead of partial data.

#include "serato/SeratoOverview.h"
#include "serato/SeratoTagFields.h"
#include "serato/SeratoTags.h"
#include "serato/SeratoTagTypes.h"

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>

namespace audiobatch::serato
{
namespace
{
constexpr std::size_t CACHE_HEADER_BYTES {16};
constexpr std::uint32_t CACHE_MAGIC {0x53544147};

/// Reads counted cache grid anchors while checking ordering and terminal versus intermediate semantics.
std::optional<BeatGrid> readCacheGrid(Cursor& input)
{
    const auto count = input.number();
    if (!count || *count > MAX_TAG_RECORDS) {
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
    if (markers.cues.size() > MAX_TAG_RECORDS || markers.loops.size() > MAX_TAG_RECORDS) {
        return false;
    }
    std::size_t total = CACHE_HEADER_BYTES;
    for (const auto& cue : markers.cues) {
        const auto size = cue.name.getNumBytesAsUTF8();
        if (size > MAX_TAG_TEXT_BYTES) {
            return false;
        }
        total += size + CUE_NAME_OFFSET;
    }
    for (const auto& loop : markers.loops) {
        const auto size = loop.name.getNumBytesAsUTF8();
        if (size > MAX_TAG_TEXT_BYTES) {
            return false;
        }
        total += size + LOOP_NAME_OFFSET;
    }
    return total <= MAX_TAG_PAYLOAD_BYTES;
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
    if (!cueCount || *cueCount > MAX_TAG_RECORDS) {
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
    if (!loopCount || *loopCount > MAX_TAG_RECORDS) {
        return std::nullopt;
    }
    for (std::uint32_t index = 0; index < *loopCount; ++index) {
        const auto start = input.number().transform([](auto value) { return std::bit_cast<std::int32_t>(value); });
        const auto end = input.number().transform([](auto value) { return std::bit_cast<std::int32_t>(value); });
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
            if (grid.markers.size() > MAX_TAG_RECORDS) {
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
                    texts, [](const auto* text) { return text->getNumBytesAsUTF8() > MAX_TAG_TEXT_BYTES; }
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
    if (payload.getDataSize() > MAX_TAG_PAYLOAD_BYTES) {
        return {};
    }
    juce::MemoryOutputStream output;
    output.writeIntBigEndian(static_cast<int>(CACHE_MAGIC));
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
    if (!supportsTagKind(kind) || bytes.size() < CACHE_HEADER_BYTES
        || bytes.size() > CACHE_HEADER_BYTES + MAX_TAG_PAYLOAD_BYTES)
    {
        return std::nullopt;
    }
    Cursor input(bytes);
    if (input.number() != CACHE_MAGIC || input.number() != std::to_underlying(kind)
        || input.number() != parserVersion(kind) || input.number() != bytes.size() - CACHE_HEADER_BYTES)
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
