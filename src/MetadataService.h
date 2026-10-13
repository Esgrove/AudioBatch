/// Audio file metadata handling built on TagLib.
/// MetadataService reads and writes text tag properties and embedded pictures in a format-agnostic way,
/// so normalized output files keep the tags and album art of their sources.

#pragma once

#include <juce_core/juce_core.h>

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

/// Format-agnostic tag properties and embedded pictures of one audio file.
struct AudioMetadata {
    /// Returns true when there are no properties and no pictures to write.
    [[nodiscard]] bool isEmpty() const noexcept
    {
        return properties.empty() && pictures.empty();
    }

    std::vector<MetadataProperty> properties;
    std::vector<MetadataPicture> pictures;
};

/// Reads and writes audio file metadata (tags + embedded pictures) using TagLib.
///
/// Metadata is represented in a format-agnostic way:
/// - `properties` mirrors TagLib's unified `PropertyMap` (uppercase keys, one or more values).
/// - `pictures` mirrors TagLib's generic `PICTURE` complex property
///   and works across ID3v2 (MP3, AIFF, WAV), Vorbis Comments (FLAC, OGG), MP4, APE, etc.
///
/// When writing, AIFF output is stored as an ID3v2.4 tag.
class MetadataService
{
public:
    static bool readMetadata(const juce::File& file, AudioMetadata& outMetadata);

    static bool writeMetadata(const juce::File& file, const AudioMetadata& metadata);

    static bool copyMetadata(const juce::File& sourceFile, const juce::File& destinationFile);
};
}  // namespace audiobatch
