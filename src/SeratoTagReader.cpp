/// Extracts existing GEOB frames, native FLAC comments and standard properties without saving any file.
/// Bounded Base64 decoding validates documented wrappers before passing payloads to independent tag parsers.

#include "SeratoTagReader.h"

#include "Log.h"
#include "TagLibUtils.h"
#include <string_view>

#include <aifffile.h>
#include <algorithm>
#include <array>
#include <fileref.h>
#include <flacfile.h>
#include <generalencapsulatedobjectframe.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <string>
#include <tpropertymap.h>
#include <wavfile.h>

namespace audiobatch::serato
{
namespace
{
constexpr std::size_t maxWrapperBytes = Overview::maxPayloadBytes + 1024;
constexpr std::size_t maxBase64Bytes = ((maxWrapperBytes + 2) / 3) * 4;
constexpr std::size_t maxEncodedBytes = maxBase64Bytes + 4096;
constexpr std::size_t containerCodeBytes = 4;
constexpr std::size_t containerTypeOffset = 2 * containerCodeBytes;
constexpr std::size_t containerHeaderBytes = containerTypeOffset + containerCodeBytes;

/// Rejects truncated or mislabeled RIFF containers that TagLib can otherwise report as valid empty tags.
bool hasValidContainerHeader(const juce::File& file)
{
    const auto input = file.createInputStream();
    if (input == nullptr) {
        return false;
    }
    if (!file.hasFileExtension("wav;wave;aif;aiff")) {
        return true;
    }
    std::array<char, containerHeaderBytes> header {};
    if (input->read(header.data(), static_cast<int>(header.size())) != static_cast<int>(header.size())) {
        return false;
    }
    const std::string_view bytes(header.data(), header.size());
    const auto container = bytes.substr(0, containerCodeBytes);
    const auto type = bytes.substr(containerTypeOffset, containerCodeBytes);
    return file.hasFileExtension("wav;wave") ? container == "RIFF" && type == "WAVE"
                                             : container == "FORM" && (type == "AIFF" || type == "AIFC");
}

/// Bounds the TagLib object before copying signed library bytes into the unsigned parser input.
std::optional<Overview> parseGeobPayload(const TagLib::ByteVector& bytes)
{
    if (bytes.size() > Overview::maxPayloadBytes) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> payload(bytes.begin(), bytes.end());
    return Overview::parse(payload);
}

/// Normalizes bounded ASCII Base64, permitting whitespace and omitted trailing padding only.
std::optional<Overview> decodeFlacOverview(const TagLib::String& value)
{
    if (value.size() > maxEncodedBytes) {
        return std::nullopt;
    }
    const auto encoded = value.to8Bit(false);
    std::string normalized;
    normalized.reserve(encoded.size());
    for (const char character : encoded) {
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n' || character == '\f'
            || character == '\v')
        {
            continue;
        }
        const bool alphabet = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9') || character == '+' || character == '/';
        if (!alphabet && character != '=') {
            return std::nullopt;
        }
        normalized.push_back(character);
    }
    if (normalized.empty() || normalized.size() > maxBase64Bytes || normalized.size() % 4 == 1) {
        return std::nullopt;
    }
    const auto paddingStart = normalized.find('=');
    if (paddingStart != std::string::npos
        && (normalized.size() % 4 != 0 || normalized.size() - paddingStart > 2
            || normalized.find_first_not_of('=', paddingStart) != std::string::npos))
    {
        return std::nullopt;
    }
    while (normalized.size() % 4 != 0) {
        normalized.push_back('=');
    }
    juce::MemoryOutputStream decoded;
    if (!juce::Base64::convertFromBase64(
            decoded, juce::String::fromUTF8(normalized.data(), static_cast<int>(normalized.size()))
        )
        || decoded.getDataSize() > maxWrapperBytes)
    {
        return std::nullopt;
    }
    const std::span bytes {static_cast<const std::uint8_t*>(decoded.getData()), decoded.getDataSize()};
    std::size_t offset = 0;
    for (const auto expected :
         {std::string_view("application/octet-stream"), std::string_view(), std::string_view("Serato Overview")})
    {
        const auto remaining = bytes.subspan(offset);
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
    return Overview::parse(bytes.subspan(offset));
}

/// Maps stable kinds to documented GEOB descriptions and native FLAC wrapper names.
TagLib::String description(TagKind kind)
{
    switch (kind) {
        case TagKind::overview:
            return "Serato Overview";
        case TagKind::analysis:
            return "Serato Analysis";
        case TagKind::autotags:
            return "Serato Autotags";
        case TagKind::beatGrid:
            return "Serato BeatGrid";
        case TagKind::markers:
            return "Serato Markers2";
        case TagKind::trackInfo:
            break;
    }
    return {};
}

/// Creates an empty outcome while retaining the requested stable kind.
TagReadResult outcome(TagKind kind, ReadStatus status)
{
    TagReadResult result;
    result.kind = kind;
    result.status = status;
    return result;
}

/// Parses one bounded GEOB object, retaining the existing Overview payload decoder.
TagReadResult parseId3Payload(TagKind kind, const TagLib::ByteVector& payload, bool legacy)
{
    auto result = outcome(kind, ReadStatus::invalid);
    if (payload.size() > maxTagPayloadBytes) {
        return result;
    }
    if (kind == TagKind::overview) {
        if (auto overview = parseGeobPayload(payload)) {
            result.overview = std::move(overview);
            result.status = ReadStatus::present;
        }
        return result;
    }
    const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    return parseTagPayload(kind, bytes, legacy);
}

/// Searches one GEOB description, recovering malformed duplicates within the existing record bound.
TagReadResult readId3Frames(
    const TagLib::ID3v2::Tag& tag,
    TagKind kind,
    bool legacy,
    const utils::CancellationCheck& isCancelled
)
{
    auto result = outcome(kind, ReadStatus::absent);
    const auto expected = legacy ? TagLib::String("Serato Markers_") : description(kind);
    std::size_t matchingRecords = 0;
    for (const auto* frame : tag.frameList("GEOB")) {
        if (utils::isCancellationRequested(isCancelled)) {
            return outcome(kind, ReadStatus::cancelled);
        }
        const auto* object = dynamic_cast<const TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frame);
        if (object == nullptr || object->description() != expected) {
            continue;
        }
        result.status = ReadStatus::invalid;
        if (kind != TagKind::overview && ++matchingRecords > maxTagRecords) {
            break;
        }
        auto parsed = parseId3Payload(kind, object->object(), legacy);
        if (parsed.status == ReadStatus::present) {
            return parsed;
        }
    }
    return result;
}

/// Prefers valid Markers2 over legacy fallback regardless of frame order, preserving earlier invalid outcomes.
TagReadResult readId3Tag(const TagLib::ID3v2::Tag* tag, TagKind kind, const utils::CancellationCheck& isCancelled)
{
    if (tag == nullptr) {
        return outcome(kind, ReadStatus::absent);
    }
    auto result = readId3Frames(*tag, kind, false, isCancelled);
    if (kind != TagKind::markers || result.status == ReadStatus::present || result.status == ReadStatus::cancelled) {
        return result;
    }
    const auto legacy = readId3Frames(*tag, kind, true, isCancelled);
    return legacy.status == ReadStatus::absent ? result : legacy;
}

/// Decodes a documented FLAC wrapper with an exact MIME, filename and matching per-kind description.
TagReadResult decodeFlacTag(TagKind kind, const TagLib::String& value, bool legacy)
{
    auto result = outcome(kind, ReadStatus::invalid);
    if (kind == TagKind::overview) {
        if (auto overview = decodeFlacOverview(value)) {
            result.overview = std::move(overview);
            result.status = ReadStatus::present;
        }
        return result;
    }
    if (value.size() > maxEncodedBytes) {
        return result;
    }
    const auto text = value.to8Bit(false);
    const std::vector<std::uint8_t> encoded(text.begin(), text.end());
    const auto decoded = decodeTagBase64(encoded);
    if (!decoded) {
        return result;
    }
    const std::span bytes {static_cast<const std::uint8_t*>(decoded->getData()), decoded->getSize()};
    const auto name = (legacy ? TagLib::String("Serato Markers_") : description(kind)).to8Bit(false);
    std::size_t offset = 0;
    for (const auto expected :
         {std::string_view("application/octet-stream"), std::string_view(), std::string_view(name)})
    {
        const auto remaining = bytes.subspan(offset);
        const auto terminator = std::ranges::find(remaining, std::uint8_t {0});
        if (terminator == remaining.end()) {
            return result;
        }
        const auto length = static_cast<std::size_t>(terminator - remaining.begin());
        if (!std::ranges::equal(remaining.first(length), expected)) {
            return result;
        }
        offset += length + 1;
    }
    return parseTagPayload(kind, bytes.subspan(offset), legacy);
}

/// Maps each kind to its documented native property, independent of optional FLAC ID3 tags.
TagLib::String propertyName(TagKind kind)
{
    switch (kind) {
        case TagKind::overview:
            return "SERATO_OVERVIEW";
        case TagKind::analysis:
            return "SERATO_ANALYSIS";
        case TagKind::autotags:
            return "SERATO_AUTOGAIN";
        case TagKind::beatGrid:
            return "SERATO_BEATGRID";
        case TagKind::markers:
            return "SERATO_MARKERS_V2";
        case TagKind::trackInfo:
            break;
    }
    return {};
}

/// Reads native FLAC values with independent duplicate recovery and optional legacy markers fallback.
TagReadResult readFlacTag(
    const TagLib::PropertyMap& properties,
    TagKind kind,
    const utils::CancellationCheck& isCancelled
)
{
    auto result = outcome(kind, ReadStatus::absent);
    for (const bool legacy : {false, true}) {
        if (legacy && kind != TagKind::markers) {
            break;
        }
        const auto entry = properties.find(legacy ? TagLib::String("SERATO_MARKERS") : propertyName(kind));
        if (entry == properties.end()) {
            continue;
        }
        result.status = ReadStatus::invalid;
        if (kind != TagKind::overview && entry->second.size() > maxTagRecords) {
            continue;
        }
        for (const auto& value : entry->second) {
            if (utils::isCancellationRequested(isCancelled)) {
                return outcome(kind, ReadStatus::cancelled);
            }
            auto parsed = decodeFlacTag(kind, value, legacy);
            if (parsed.status == ReadStatus::present) {
                return parsed;
            }
        }
    }
    return result;
}

/// Validates one bounded standard property and stores the first usable tempo or musical key.
bool readTrackInfoValue(TrackInfo& info, std::string_view name, const TagLib::String& value)
{
    if (value.size() > maxTagTextBytes) {
        return false;
    }
    const auto text = value.to8Bit(true);
    if (text.size() > maxTagTextBytes) {
        return false;
    }
    if (name == "BPM") {
        const auto bpm = parseTagNumber(text);
        if (bpm && *bpm > 0) {
            info.bpm = bpm;
            return true;
        }
    } else if (
        juce::CharPointer_UTF8::isValidString(text.c_str(), static_cast<int>(text.size())) && !text.contains('\0')
    )
    {
        info.key = juce::String::fromUTF8(text.c_str(), static_cast<int>(text.size())).trim();
        return info.key.isNotEmpty();
    }
    return false;
}

/// Reads standard tempo and musical key, allowing malformed BPM to coexist with an independently valid key.
TagReadResult readTrackInfo(const TagLib::PropertyMap& properties, const utils::CancellationCheck& isCancelled)
{
    auto result = outcome(TagKind::trackInfo, ReadStatus::absent);
    TrackInfo info;
    for (const auto* name : {"BPM", "INITIALKEY"}) {
        const auto entry = properties.find(name);
        if (entry == properties.end()) {
            continue;
        }
        result.status = ReadStatus::invalid;
        if (entry->second.size() > maxTagRecords) {
            continue;
        }
        for (const auto& value : entry->second) {
            if (utils::isCancellationRequested(isCancelled)) {
                return outcome(TagKind::trackInfo, ReadStatus::cancelled);
            }
            if (readTrackInfoValue(info, name, value)) {
                break;
            }
        }
    }
    if (info.bpm || info.key.isNotEmpty()) {
        result.status = ReadStatus::present;
        result.trackInfo = std::move(info);
    }
    return result;
}

/// Finds existing ID3 tags on supported non-FLAC containers without creating missing tags.
const TagLib::ID3v2::Tag* findId3Tag(TagLib::File& file)
{
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(&file)) {
        return mpeg->ID3v2Tag(false);
    }
    if (const auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(&file)) {
        return aiff->hasID3v2Tag() ? aiff->tag() : nullptr;
    }
    if (const auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(&file)) {
        return wav->hasID3v2Tag() ? wav->ID3v2Tag() : nullptr;
    }
    return nullptr;
}

/// Dispatches one requested kind using properties and ID3 tags borrowed from an already open file.
TagReadResult readOpenedTag(
    TagKind kind,
    const TagLib::PropertyMap& properties,
    const TagLib::ID3v2::Tag* id3,
    bool isFlac,
    const utils::CancellationCheck& isCancelled
)
{
    if (kind == TagKind::trackInfo) {
        return readTrackInfo(properties, isCancelled);
    }
    if (isFlac) {
        return readFlacTag(properties, kind, isCancelled);
    }
    return readId3Tag(id3, kind, isCancelled);
}

/// Replaces supported outcomes after a file-level failure while retaining unsupported requested kinds.
std::vector<TagReadResult> failedReads(std::vector<TagReadResult> results, ReadStatus status)
{
    for (auto& result : results) {
        if (supportsTagKind(result.kind)) {
            result = outcome(result.kind, status);
        }
    }
    return results;
}
}  // namespace

/// Opens a supported file once and returns independent outcomes in requested order, including duplicates.
/// Valid later duplicates recover invalid earlier ones. Markers2 takes precedence over legacy fallback.
/// Requests above maxTagRecords are rejected with an empty vector. Empty requests perform no I/O.
/// Cancellation never turns a transient failure into a stable negative result.
std::vector<TagReadResult> readTags(
    const juce::File& file,
    std::span<const TagKind> kinds,
    const utils::CancellationCheck& isCancelled
)
{
    if (kinds.empty() || kinds.size() > maxTagRecords) {
        return {};
    }
    std::vector<TagReadResult> results;
    results.reserve(kinds.size());
    for (const auto kind : kinds) {
        results.push_back(outcome(kind, supportsTagKind(kind) ? ReadStatus::absent : ReadStatus::unsupported));
    }
    if (utils::isCancellationRequested(isCancelled)) {
        return failedReads(std::move(results), ReadStatus::cancelled);
    }
    if (!file.existsAsFile()) {
        return failedReads(std::move(results), ReadStatus::ioError);
    }
    if (!file.hasFileExtension("mp3;aif;aiff;wav;wave;flac")) {
        return failedReads(std::move(results), ReadStatus::unsupported);
    }
    if (!hasValidContainerHeader(file)) {
        return failedReads(std::move(results), ReadStatus::ioError);
    }
    const auto& fullPath = file.getFullPathName();
    const TagLib::FileRef reference(utils::tagLibFileName(fullPath), false);
    if (utils::isCancellationRequested(isCancelled)) {
        return failedReads(std::move(results), ReadStatus::cancelled);
    }
    auto* tagFile = reference.file();
    if (reference.isNull() || tagFile == nullptr || !tagFile->isOpen() || !tagFile->isValid()) {
        return failedReads(std::move(results), ReadStatus::ioError);
    }
    const auto* id3 = findId3Tag(*tagFile);
    const auto* flac = dynamic_cast<TagLib::FLAC::File*>(tagFile);
    TagLib::PropertyMap properties;
    if (flac != nullptr || std::ranges::find(kinds, TagKind::trackInfo) != kinds.end()) {
        properties = tagFile->properties();
    }
    for (auto& result : results) {
        if (!supportsTagKind(result.kind)) {
            continue;
        }
        if (utils::isCancellationRequested(isCancelled)) {
            result = outcome(result.kind, ReadStatus::cancelled);
            continue;
        }
        result = readOpenedTag(result.kind, properties, id3, flac != nullptr, isCancelled);
        if (utils::isCancellationRequested(isCancelled)) {
            result = outcome(result.kind, ReadStatus::cancelled);
        }
        if (result.status == ReadStatus::invalid) {
            utils::logDebug(
                "Unusable Serato tag kind {} in {}", static_cast<std::uint32_t>(result.kind), fullPath.quoted()
            );
        }
    }
    if (!tagFile->isValid() || !file.existsAsFile()) {
        return failedReads(std::move(results), ReadStatus::ioError);
    }
    return results;
}

/// Reads one requested kind from MPEG, AIFF or WAV ID3v2 GEOB, or native FLAC comments.
/// The first valid duplicate wins. Ogg and other containers are unsupported.
/// Cancellation is checked around TagLib operations, which cannot be interrupted internally.
/// Missing/unreadable files return ioError, never a stable absent result.
TagReadResult readTag(const juce::File& file, TagKind kind, const utils::CancellationCheck& isCancelled)
{
    const std::array kinds {kind};
    return readTags(file, kinds, isCancelled).front();
}
}  // namespace audiobatch::serato
