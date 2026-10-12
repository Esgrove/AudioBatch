/// Extracts existing GEOB frames and native FLAC Overview comments without saving any file.
/// Bounded Base64 decoding validates the documented wrapper before passing its payload to Overview.

#include "SeratoTagReader.h"

#include "Log.h"
#include "TagLibUtils.h"
#include <string_view>

#include <aifffile.h>
#include <algorithm>
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
    std::array<char, 12> header {};
    if (input->read(header.data(), static_cast<int>(header.size())) != static_cast<int>(header.size())) {
        return false;
    }
    const std::string_view container(header.data(), 4);
    const std::string_view type(header.data() + 8, 4);
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

/// Searches existing GEOB frames, allowing a later valid frame to recover from an invalid duplicate.
TagReadResult readId3Overview(const TagLib::ID3v2::Tag* tag, const utils::CancellationCheck& isCancelled)
{
    TagReadResult result;
    if (tag == nullptr) {
        return result;
    }
    for (const auto* frame : tag->frameList("GEOB")) {
        if (utils::isCancellationRequested(isCancelled)) {
            return {.kind = TagKind::overview, .status = ReadStatus::cancelled, .overview = std::nullopt};
        }
        const auto* object = dynamic_cast<const TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frame);
        if (object == nullptr || object->description() != "Serato Overview") {
            continue;
        }
        result.status = ReadStatus::invalid;
        const auto payload = object->object();
        if (auto overview = parseGeobPayload(payload)) {
            return {.kind = TagKind::overview, .status = ReadStatus::present, .overview = std::move(overview)};
        }
    }
    return result;
}

/// Reads native FLAC comments, without falling back to FLAC's optional ID3 tags.
TagReadResult readFlacOverview(const TagLib::FLAC::File& file, const utils::CancellationCheck& isCancelled)
{
    TagReadResult result;
    const auto properties = file.properties();
    const auto entry = properties.find("SERATO_OVERVIEW");
    if (entry == properties.end()) {
        return result;
    }
    result.status = ReadStatus::invalid;
    for (const auto& value : entry->second) {
        if (utils::isCancellationRequested(isCancelled)) {
            return {.kind = TagKind::overview, .status = ReadStatus::cancelled, .overview = std::nullopt};
        }
        if (auto overview = decodeFlacOverview(value)) {
            return {.kind = TagKind::overview, .status = ReadStatus::present, .overview = std::move(overview)};
        }
    }
    return result;
}
}  // namespace

TagReadResult readTag(const juce::File& file, TagKind kind, const utils::CancellationCheck& isCancelled)
{
    if (utils::isCancellationRequested(isCancelled)) {
        return {.kind = kind, .status = ReadStatus::cancelled, .overview = std::nullopt};
    }
    if (kind != TagKind::overview) {
        return {.kind = kind, .status = ReadStatus::unsupported, .overview = std::nullopt};
    }
    if (!file.existsAsFile()) {
        return {.kind = kind, .status = ReadStatus::ioError, .overview = std::nullopt};
    }
    if (!file.hasFileExtension("mp3;aif;aiff;wav;wave;flac")) {
        return {.kind = kind, .status = ReadStatus::unsupported, .overview = std::nullopt};
    }
    if (!hasValidContainerHeader(file)) {
        return {.kind = kind, .status = ReadStatus::ioError, .overview = std::nullopt};
    }
    const auto& fullPath = file.getFullPathName();
    const TagLib::FileRef reference(utils::tagLibFileName(fullPath), false);
    if (utils::isCancellationRequested(isCancelled)) {
        return {.kind = kind, .status = ReadStatus::cancelled, .overview = std::nullopt};
    }
    auto* tagFile = reference.file();
    if (reference.isNull() || tagFile == nullptr || !tagFile->isOpen() || !tagFile->isValid()) {
        return {.kind = kind, .status = ReadStatus::ioError, .overview = std::nullopt};
    }
    TagReadResult result {.kind = kind, .status = ReadStatus::ioError, .overview = std::nullopt};
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(tagFile)) {
        result = readId3Overview(mpeg->ID3v2Tag(false), isCancelled);
    } else if (const auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(tagFile)) {
        result = readId3Overview(aiff->hasID3v2Tag() ? aiff->tag() : nullptr, isCancelled);
    } else if (const auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(tagFile)) {
        result = readId3Overview(wav->hasID3v2Tag() ? wav->ID3v2Tag() : nullptr, isCancelled);
    } else if (const auto* flac = dynamic_cast<TagLib::FLAC::File*>(tagFile)) {
        result = readFlacOverview(*flac, isCancelled);
    }
    if (utils::isCancellationRequested(isCancelled)) {
        return {.kind = kind, .status = ReadStatus::cancelled, .overview = std::nullopt};
    }
    if (!tagFile->isValid() || !file.existsAsFile()) {
        return {.kind = kind, .status = ReadStatus::ioError, .overview = std::nullopt};
    }
    if (result.status == ReadStatus::invalid) {
        utils::logDebug("Unusable Serato Overview in {}", fullPath.quoted());
    }
    return result;
}
}  // namespace audiobatch::serato