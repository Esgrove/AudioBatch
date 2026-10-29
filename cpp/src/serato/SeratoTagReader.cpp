/// Extracts existing GEOB frames, native FLAC comments and standard properties without saving any file.
/// Bounded Base64 decoding validates documented wrappers before passing payloads to independent tag parsers.

#include "serato/SeratoTagReader.h"

#include "serato/SeratoTags.h"
#include "serato/SeratoTagTypes.h"
#include "utils/AudioFormatUtils.h"
#include "utils/Log.h"
#include "utils/ThreadUtils.h"
#include <string_view>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fileref.h>
#include <flacfile.h>
#include <generalencapsulatedobjectframe.h>
#include <id3v2tag.h>
#include <ranges>
#include <span>
#include <string>
#include <tpropertymap.h>
#include <utility>
#include <vector>

namespace audiobatch::serato
{
namespace
{
constexpr std::size_t containerCodeBytes {4};
constexpr std::size_t containerTypeOffset {2 * containerCodeBytes};
constexpr std::size_t containerHeaderBytes {containerTypeOffset + containerCodeBytes};

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

/// Parses one bounded GEOB object, copying the signed TagLib bytes into the unsigned parser input.
TagReadResult parseId3Payload(const TagKind kind, const TagLib::ByteVector& payload, const bool legacy)
{
    if (payload.size() > maxTagPayloadBytes) {
        return TagReadResult::missing(kind, ReadStatus::invalid);
    }
    const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    return parseTagPayload(kind, bytes, legacy);
}

/// Searches one GEOB description, recovering malformed duplicates within the existing record bound.
TagReadResult readId3Frames(
    const TagLib::ID3v2::Tag& tag,
    const TagKind kind,
    const bool legacy,
    const utils::CancellationCheck& isCancelled
)
{
    auto result = TagReadResult::missing(kind, ReadStatus::absent);
    const auto names = getTagNames(kind, legacy);
    if (!names.has_value()) {
        return result;
    }
    const TagLib::String expected(std::string(names->description));
    std::size_t matchingRecords = 0;
    for (const auto* frame : tag.frameList("GEOB")) {
        if (utils::isCancellationRequested(isCancelled)) {
            return TagReadResult::missing(kind, ReadStatus::cancelled);
        }
        const auto* object = dynamic_cast<const TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frame);
        if (object == nullptr || object->description() != expected) {
            continue;
        }
        result = TagReadResult::missing(kind, ReadStatus::invalid);
        if (kind != TagKind::overview && ++matchingRecords > maxTagRecords) {
            break;
        }
        auto parsed = parseId3Payload(kind, object->object(), legacy);
        if (parsed.value.has_value()) {
            return parsed;
        }
    }
    return result;
}

/// Prefers valid Markers2 over legacy fallback regardless of frame order,
/// preserving earlier invalid outcomes.
TagReadResult readId3Tag(const TagLib::ID3v2::Tag* tag, const TagKind kind, const utils::CancellationCheck& isCancelled)
{
    if (tag == nullptr) {
        return TagReadResult::missing(kind, ReadStatus::absent);
    }
    auto result = readId3Frames(*tag, kind, false, isCancelled);
    if (kind != TagKind::markers || result.status() == ReadStatus::present || result.status() == ReadStatus::cancelled)
    {
        return result;
    }
    const auto legacy = readId3Frames(*tag, kind, true, isCancelled);
    return legacy.status() == ReadStatus::absent ? result : legacy;
}

/// Decodes a documented FLAC wrapper with an exact MIME, filename and matching per-kind description.
TagReadResult decodeFlacTag(const TagKind kind, const TagLib::String& value, const bool legacy)
{
    const auto invalid = [kind] { return TagReadResult::missing(kind, ReadStatus::invalid); };
    // Base64 is ASCII, and to8Bit keeps only the low byte of every other character,
    // so for example U+0141 would pass as the Base64 letter 'A'.
    if (!value.isAscii() || value.size() > maxEncodedTagBytes) {
        return invalid();
    }
    const auto names = getTagNames(kind, legacy);
    const auto text = value.to8Bit(false);
    const std::vector<std::uint8_t> encoded(text.begin(), text.end());
    const auto decoded = decodeTagBase64(encoded);
    if (!names.has_value() || !decoded.has_value()) {
        return invalid();
    }
    const std::span bytes {static_cast<const std::uint8_t*>(decoded->getData()), decoded->getSize()};
    const auto payload = parseTagWrapper(bytes, names->description);
    return payload.has_value() ? parseTagPayload(kind, *payload, legacy) : invalid();
}

/// Reads native FLAC values with independent duplicate recovery and optional legacy markers fallback.
TagReadResult readFlacTag(
    const TagLib::PropertyMap& properties,
    const TagKind kind,
    const utils::CancellationCheck& isCancelled
)
{
    auto result = TagReadResult::missing(kind, ReadStatus::absent);
    for (const bool legacy : {false, true}) {
        if (legacy && kind != TagKind::markers) {
            break;
        }
        const auto names = getTagNames(kind, legacy);
        if (!names.has_value()) {
            break;
        }
        const auto entry = properties.find(TagLib::String(std::string(names->property)));
        if (entry == properties.end()) {
            continue;
        }
        result = TagReadResult::missing(kind, ReadStatus::invalid);
        if (kind != TagKind::overview && entry->second.size() > maxTagRecords) {
            continue;
        }
        for (const auto& value : entry->second) {
            if (utils::isCancellationRequested(isCancelled)) {
                return TagReadResult::missing(kind, ReadStatus::cancelled);
            }
            auto parsed = decodeFlacTag(kind, value, legacy);
            if (parsed.value.has_value()) {
                return parsed;
            }
        }
    }
    return result;
}

/// Returns the trimmed text of a bounded tag value, or an empty string when it is not valid UTF-8.
juce::String readTagText(const std::string& text)
{
    if (!juce::CharPointer_UTF8::isValidString(text.c_str(), static_cast<int>(text.size())) || text.contains('\0')) {
        return {};
    }
    return juce::String::fromUTF8(text.c_str(), static_cast<int>(text.size())).trim();
}

/// Returns the year a DATE value starts with, or the whole value when it does not start with four digits.
juce::String readYear(const juce::String& date)
{
    const auto year = date.substring(0, 4);
    return year.length() == 4 && year.containsOnly("0123456789") ? year : date;
}

/// Validates one bounded standard property and stores it as the first usable value of its field.
bool readTrackInfoValue(TrackInfo& info, const std::string_view name, const TagLib::String& value)
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
        return false;
    }
    const auto string = readTagText(text);
    if (string.isEmpty()) {
        return false;
    }
    if (name == "INITIALKEY") {
        info.key = string;
    } else if (name == "ALBUM") {
        info.album = string;
    } else if (name == "GENRE") {
        info.genre = string;
    } else if (name == "DATE") {
        info.year = readYear(string);
    } else if (name == "COMMENT" || name == "DESCRIPTION") {
        // Vorbis comments may hold the comment as DESCRIPTION, as ffmpeg writes it, so that is the fallback.
        if (info.comment.isEmpty()) {
            info.comment = string;
        }
    } else {
        return false;
    }
    return true;
}

/// Reads the standard tempo, musical key, album, genre, date, and comment,
/// allowing a malformed value to coexist with independently valid ones.
TagReadResult readTrackInfo(const TagLib::PropertyMap& properties, const utils::CancellationCheck& isCancelled)
{
    auto result = TagReadResult::missing(TagKind::trackInfo, ReadStatus::absent);
    TrackInfo info;
    for (const auto* name : {"BPM", "INITIALKEY", "ALBUM", "GENRE", "DATE", "COMMENT", "DESCRIPTION"}) {
        const auto entry = properties.find(name);
        if (entry == properties.end()) {
            continue;
        }
        result = TagReadResult::missing(TagKind::trackInfo, ReadStatus::invalid);
        if (entry->second.size() > maxTagRecords) {
            continue;
        }
        for (const auto& value : entry->second) {
            if (utils::isCancellationRequested(isCancelled)) {
                return TagReadResult::missing(TagKind::trackInfo, ReadStatus::cancelled);
            }
            if (readTrackInfoValue(info, name, value)) {
                break;
            }
        }
    }
    if (!info.isEmpty()) {
        return TagReadResult::present(std::move(info));
    }
    return result;
}

/// Dispatches one requested kind using properties and ID3 tags borrowed from an already open file.
TagReadResult readOpenedTag(
    const TagKind kind,
    const TagLib::PropertyMap& properties,
    const TagLib::ID3v2::Tag* id3,
    const bool isFlac,
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
std::vector<TagReadResult> failedReads(std::vector<TagReadResult> results, const ReadStatus status)
{
    for (auto& result : results) {
        if (supportsTagKind(result.kind)) {
            result = TagReadResult::missing(result.kind, status);
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
    auto results
        = kinds | std::views::transform([](const TagKind kind) {
              return TagReadResult::missing(kind, supportsTagKind(kind) ? ReadStatus::absent : ReadStatus::unsupported);
          })
        | std::ranges::to<std::vector>();
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
            result = TagReadResult::missing(result.kind, ReadStatus::cancelled);
            continue;
        }
        result = readOpenedTag(result.kind, properties, id3, flac != nullptr, isCancelled);
        if (utils::isCancellationRequested(isCancelled)) {
            result = TagReadResult::missing(result.kind, ReadStatus::cancelled);
        }
        if (result.status() == ReadStatus::invalid) {
            utils::logDebug("Unusable Serato tag kind {} in {}", std::to_underlying(result.kind), fullPath.quoted());
        }
    }
    if (!tagFile->isValid() || !file.existsAsFile()) {
        return failedReads(std::move(results), ReadStatus::ioError);
    }
    return results;
}

/// Reads one requested kind from MPEG, AIFF or WAV ID3v2 GEOB, or native FLAC comments. The first valid duplicate wins.
/// Ogg and other containers are unsupported.
/// Cancellation is checked around TagLib operations, which cannot be interrupted internally.
/// Missing/unreadable files return ioError, never a stable absent result.
TagReadResult readTag(const juce::File& file, const TagKind kind, const utils::CancellationCheck& isCancelled)
{
    const std::array kinds {kind};
    return readTags(file, kinds, isCancelled).front();
}
}  // namespace audiobatch::serato
