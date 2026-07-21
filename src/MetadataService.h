/// Audio file metadata handling built on TagLib.
/// MetadataService reads and writes text tag properties and embedded pictures in a format-agnostic way,
/// so normalized output files keep the tags and album art of their sources.

#pragma once

#include <juce_core/juce_core.h>

#include <utility>
#include <vector>

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
    struct PropertyEntry {
        juce::String key;
        juce::StringArray values;
    };

    struct Picture {
        juce::MemoryBlock data;
        juce::String mimeType;
        juce::String description;
        /// ID3v2 picture type name, e.g. "Front Cover", "Back Cover", "Band".
        juce::String pictureType;
    };

    struct Metadata {
        std::vector<PropertyEntry> properties;
        std::vector<Picture> pictures;

        /// Returns true when there are no properties and no pictures to write.
        [[nodiscard]] bool isEmpty() const noexcept
        {
            return properties.empty() && pictures.empty();
        }
    };

    /// Reads all metadata (text properties and pictures) from the given file.
    /// Returns true on success. Output is cleared on entry.
    static bool readMetadata(const juce::File& file, Metadata& outMetadata);

    /// Writes the given metadata to the file.
    /// The file must exist and be in a format TagLib can write.
    /// For AIFF and WAV output the tag is saved as ID3v2.4.
    static bool writeMetadata(const juce::File& file, const Metadata& metadata);
};
