/// Implementation of the source bit depth detection.
/// Reads the bit depth from the reader for lossless formats, and for MP3 files searches the ID3v2 tag text
/// for a bit depth noted by the encoder, since the decoder output depth says nothing about the source.

#include "BitDepthDetection.h"

#include "FileUtils.h"
#include "StringFormat.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace audiobatch::analysis
{
constexpr int defaultMp3BitsPerSample = 16;
/// Number of payload bits in each byte of an ID3v2 synchsafe integer.
/// The most significant bit of every byte is always zero.
constexpr std::uint32_t synchsafeBitsPerByte = 7U;

/// Returns true when the reader reports the decoder output bit depth instead of the source bit depth.
/// JUCE's MP3 reader is the known case, since decoding hides the original encoding depth.
static bool reportsDecodedBitDepth(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (utils::isMp3File(file)) {
        return true;
    }

    return reader.getFormatName().containsIgnoreCase("mp3");
}

/// Reads exactly the requested number of bytes from the stream.
/// Returns false on a short read, so callers can treat truncated data as missing.
static bool readExactly(juce::InputStream& input, void* destination, const int bytesToRead)
{
    return bytesToRead >= 0 && input.read(destination, bytesToRead) == bytesToRead;
}

/// Decodes a 28-bit synchsafe integer, the encoding ID3v2 uses for tag and frame sizes.
static std::uint32_t readSynchsafeUint32(const std::span<const std::uint8_t, 4> bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) << (synchsafeBitsPerByte * 3U)
        | static_cast<std::uint32_t>(bytes[1]) << (synchsafeBitsPerByte * 2U)
        | static_cast<std::uint32_t>(bytes[2]) << synchsafeBitsPerByte | static_cast<std::uint32_t>(bytes[3]);
}

/// Flattens raw ID3 tag bytes into a lowercase text blob suitable for substring searches.
/// Alphanumeric characters and a few common separators are kept,
/// and every other run of bytes collapses into a single space.
static juce::String buildSearchableMetadataText(const juce::MemoryBlock& metadata)
{
    juce::String text;
    const std::span bytes(static_cast<const std::uint8_t*>(metadata.getData()), metadata.getSize());
    bool previousWasSeparator = true;

    for (const auto byte : bytes) {
        if ((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
            text += juce::String::charToString(byte);
            previousWasSeparator = false;
            continue;
        }

        if (byte == '-' || byte == '_' || byte == '/' || byte == '.') {
            text += juce::String::charToString(byte);
            previousWasSeparator = false;
            continue;
        }

        if (!previousWasSeparator) {
            text += ' ';
            previousWasSeparator = true;
        }
    }

    return text.toLowerCase();
}

/// Searches flattened metadata text for a plausible source bit depth mention.
/// Returns the matched depth, or 0 when no known pattern is present.
static int findBitDepthInText(const juce::String& text)
{
    constexpr std::array candidateBitDepths {8, 12, 16, 20, 24, 32};

    for (const auto candidate : candidateBitDepths) {
        if (const auto value = juce::String(candidate); text.contains(utils::format("{}-bit", value))
            || text.contains(utils::format("{} bit", value)) || text.contains(utils::format("{} bits", value)))
        {
            return candidate;
        }
    }

    for (const auto candidate : candidateBitDepths) {
        if (const auto value = juce::String(candidate); text.contains(utils::format("bit depth {}", value))
            || text.contains(utils::format("bitdepth {}", value))
            || text.contains(utils::format("bits per sample {}", value))
            || text.contains(utils::format("source bit depth {}", value))
            || text.contains(utils::format("source bits per sample {}", value)))
        {
            return candidate;
        }
    }

    return 0;
}

/// Scans the ID3v2 tag of an MP3 file for a mention of the original source bit depth.
/// Returns 0 when the file has no tag, the tag is malformed, or no bit depth is mentioned.
static int extractTaggedMp3BitDepth(const juce::File& file)
{
    const auto input = file.createInputStream();

    if (input == nullptr) {
        return 0;
    }

    constexpr juce::int64 id3HeaderSize = 10;
    std::array<std::uint8_t, id3HeaderSize> header {};

    if (!readExactly(*input, header.data(), static_cast<int>(header.size()))) {
        return 0;
    }

    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        return 0;
    }

    constexpr std::size_t sizeFieldOffset = 6;
    constexpr std::size_t sizeFieldLength = 4;
    const auto sizeField = std::span(header).subspan<sizeFieldOffset, sizeFieldLength>();
    if (std::ranges::any_of(sizeField, [](const std::uint8_t byte) { return (byte & 0x80U) != 0U; })) {
        return 0;
    }

    const auto payloadSize = readSynchsafeUint32(std::span(header).subspan<sizeFieldOffset, sizeFieldLength>());
    const auto hasFooter = (header[5] & 0x10U) != 0;
    const auto totalSize
        = id3HeaderSize + static_cast<juce::int64>(payloadSize) + (hasFooter ? id3HeaderSize : juce::int64 {0});

    if (totalSize <= id3HeaderSize || totalSize > file.getSize()) {
        return 0;
    }

    juce::MemoryBlock metadata;
    metadata.setSize(static_cast<size_t>(totalSize), false);
    input->setPosition(0);

    if (!readExactly(*input, metadata.getData(), static_cast<int>(totalSize))) {
        return 0;
    }

    return findBitDepthInText(buildSearchableMetadataText(metadata));
}

int resolveSourceBitsPerSample(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (reportsDecodedBitDepth(reader, file)) {
        if (const auto taggedBitDepth = extractTaggedMp3BitDepth(file); taggedBitDepth > 0) {
            return taggedBitDepth;
        }

        return defaultMp3BitsPerSample;
    }

    return static_cast<int>(reader.bitsPerSample);
}
}  // namespace audiobatch::analysis
