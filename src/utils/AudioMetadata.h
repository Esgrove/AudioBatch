/// Audio file metadata handling built on TagLib.
/// The metadata functions read and write text tag properties
/// and embedded pictures in a format-agnostic way, retaining opaque ID3v2 frames too.
/// Processing copies discard the Serato overview while keeping the remaining source tags.

#pragma once

#include <juce_core/juce_core.h>

#include <tfile.h>
#include <vector>

namespace audiobatch
{
/// One text tag property, with one or more values.
struct MetadataProperty {
    juce::String key;
    juce::StringArray values;
};

/// One embedded picture, such as album art.
struct MetadataPicture {
    /// Encoded image bytes.
    juce::MemoryBlock data;
    /// Image type such as "image/jpeg" or "image/png".
    juce::String mimeType;
    /// Free text description stored with the picture.
    juce::String description;
    /// ID3v2 picture type name, e.g. "Front Cover", "Back Cover", "Band".
    juce::String pictureType;
};

/// Format-agnostic tag properties and embedded pictures of one audio file, read
/// and written through TagLib.
/// - `properties` mirrors TagLib's unified `PropertyMap` (uppercase keys, one or more values).
/// - `pictures` mirrors TagLib's generic `PICTURE` complex property
///   and works across ID3v2 (MP3, AIFF, WAV), Vorbis Comments (FLAC, OGG), MP4,
///   APE, etc.
///
/// When writing, AIFF output is stored as an ID3v2.4 tag.
struct AudioMetadata {
    /// Returns true when there are no properties, pictures or ID3v2 frames to write.
    [[nodiscard]] bool isEmpty() const noexcept
    {
        return properties.empty() && pictures.empty() && id3v2Frames.empty() && !hasUnsupportedId3Frames;
    }

    std::vector<MetadataProperty> properties;
    std::vector<MetadataPicture> pictures;
    /// Owned ID3v2.4 frame bytes, including custom binary frames outside TagLib's PropertyMap.
    std::vector<juce::MemoryBlock> id3v2Frames;
    /// True when TagLib cannot safely reserialize a known frame, so processing must keep the original file.
    bool hasUnsupportedId3Frames {false};
};

[[nodiscard]] bool copyMetadata(const juce::File& sourceFile, const juce::File& destinationFile);
bool readMetadata(const juce::File& file, AudioMetadata& outMetadata);
bool writeMetadata(const juce::File& file, const AudioMetadata& metadata);

namespace utils
{
[[nodiscard]] TagLib::FileName tagLibFileName(const juce::String& fullPath);
}  // namespace utils
}  // namespace audiobatch
