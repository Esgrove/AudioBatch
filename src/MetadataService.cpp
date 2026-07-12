/// Implementation of MetadataService.
/// Converts between JUCE and TagLib string and buffer types,
/// maps tags through TagLib's unified PropertyMap and the generic PICTURE complex property,
/// and saves tags for AIFF and WAV output as ID3v2.4.

#include "MetadataService.h"

#include "utils.h"

#include <aifffile.h>
#include <algorithm>
#include <fileref.h>
#include <id3v2.h>
#include <tbytevector.h>
#include <tfile.h>
#include <tlist.h>
#include <tpropertymap.h>
#include <tstring.h>
#include <tstringlist.h>
#include <tvariant.h>
#include <wavfile.h>

namespace
{
/// Converts a TagLib string to a juce::String, preserving UTF-8 content.
juce::String toJuce(const TagLib::String& tagString)
{
    return juce::String::fromUTF8(tagString.toCString(true));
}

/// Converts a juce::String to a TagLib UTF-8 string.
TagLib::String toTagLib(const juce::String& juceString)
{
    return {juceString.toRawUTF8(), TagLib::String::UTF8};
}

#if JUCE_WINDOWS
/// Builds a TagLib file name from a JUCE path.
/// On Windows TagLib::FileName is a class whose constructor copies the wide-char buffer internally,
/// so the source juce::String only needs to outlive this call.
TagLib::FileName toFileName(const juce::String& fullPath)
{
    return TagLib::FileName(fullPath.toWideCharPointer());
}
#else
/// Builds a TagLib file name from a JUCE path.
/// On non-Windows TagLib::FileName is a typedef for const char*.
/// The returned pointer aliases the buffer inside fullPath,
/// so callers must keep the juce::String alive for as long as the pointer is used.
TagLib::FileName toFileName(const juce::String& fullPath)
{
    return fullPath.toRawUTF8();
}
#endif

/// Returns true when the file extension marks an AIFF family file.
bool isAiffFile(const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();
    return extension == ".aif" || extension == ".aiff" || extension == ".aifc";
}

/// Returns true when the file extension marks a WAV file.
bool isWavFile(const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();
    return extension == ".wav" || extension == ".wave";
}

/// Translates a TagLib PropertyMap into our format-agnostic representation.
std::vector<MetadataService::PropertyEntry> readProperties(const TagLib::PropertyMap& propertyMap)
{
    std::vector<MetadataService::PropertyEntry> entries;
    entries.reserve(propertyMap.size());

    for (const auto& [key, values] : propertyMap) {
        MetadataService::PropertyEntry entry;
        entry.key = toJuce(key);

        for (const auto& value : values) {
            entry.values.add(toJuce(value));
        }

        if (!entry.key.isEmpty()) {
            entries.push_back(std::move(entry));
        }
    }

    return entries;
}

/// Translates our representation back into a TagLib PropertyMap.
TagLib::PropertyMap buildPropertyMap(const std::vector<MetadataService::PropertyEntry>& properties)
{
    TagLib::PropertyMap propertyMap;

    for (const auto& [key, values] : properties) {
        if (key.isEmpty()) {
            continue;
        }

        TagLib::StringList stringList;

        for (const auto& value : values) {
            stringList.append(toTagLib(value));
        }

        propertyMap.insert(toTagLib(key), stringList);
    }

    return propertyMap;
}

/// Reads embedded pictures via TagLib's generic "PICTURE" complex property.
/// Pictures without image data are skipped.
std::vector<MetadataService::Picture> readPictures(const TagLib::File& file)
{
    std::vector<MetadataService::Picture> pictures;

    const auto keys = file.complexPropertyKeys();
    const bool hasPictureKey = std::ranges::any_of(keys, [](const TagLib::String& key) { return key == "PICTURE"; });

    if (!hasPictureKey) {
        return pictures;
    }

    const auto pictureMaps = file.complexProperties("PICTURE");
    pictures.reserve(pictureMaps.size());

    for (const auto& pictureMap : pictureMaps) {
        MetadataService::Picture picture;

        for (const auto& [fieldKey, value] : pictureMap) {
            if (fieldKey == "data") {
                const auto bytes = value.value<TagLib::ByteVector>();

                if (!bytes.isEmpty()) {
                    picture.data.append(bytes.data(), bytes.size());
                }
            } else if (fieldKey == "mimeType") {
                picture.mimeType = toJuce(value.value<TagLib::String>());
            } else if (fieldKey == "description") {
                picture.description = toJuce(value.value<TagLib::String>());
            } else if (fieldKey == "pictureType") {
                picture.pictureType = toJuce(value.value<TagLib::String>());
            }
        }

        if (picture.data.getSize() > 0) {
            pictures.push_back(std::move(picture));
        }
    }

    return pictures;
}

/// Converts pictures into TagLib complex property maps for writing.
/// A missing MIME type defaults to JPEG and a missing picture type to "Front Cover".
TagLib::List<TagLib::VariantMap> buildPictureList(const std::vector<MetadataService::Picture>& pictures)
{
    TagLib::List<TagLib::VariantMap> pictureList;

    for (const auto& [data, mimeType, description, pictureType] : pictures) {
        if (data.getSize() == 0) {
            continue;
        }

        TagLib::VariantMap pictureMap;

        const TagLib::ByteVector bytes(
            static_cast<const char*>(data.getData()), static_cast<unsigned int>(data.getSize())
        );
        pictureMap.insert("data", TagLib::Variant(bytes));

        pictureMap.insert(
            "mimeType", TagLib::Variant(toTagLib(mimeType.isEmpty() ? juce::String("image/jpeg") : mimeType))
        );

        pictureMap.insert(
            "pictureType", TagLib::Variant(toTagLib(pictureType.isEmpty() ? juce::String("Front Cover") : pictureType))
        );

        pictureMap.insert("description", TagLib::Variant(toTagLib(description)));

        pictureList.append(pictureMap);
    }

    return pictureList;
}
}  // namespace

bool MetadataService::readMetadata(const juce::File& file, Metadata& outMetadata)
{
    outMetadata = Metadata {};

    if (!file.existsAsFile()) {
        return false;
    }

    const auto fullPath = file.getFullPathName();
    const TagLib::FileRef fileRef(toFileName(fullPath), false);

    if (fileRef.isNull() || fileRef.file() == nullptr) {
        utils::logError("TagLib could not open file for reading: " + file.getFullPathName().quoted());
        return false;
    }

    const auto* taglibFile = fileRef.file();
    outMetadata.properties = readProperties(taglibFile->properties());
    outMetadata.pictures = readPictures(*taglibFile);

    return true;
}

bool MetadataService::writeMetadata(const juce::File& file, const Metadata& metadata)
{
    if (!file.existsAsFile()) {
        utils::logError("Cannot write metadata, target file is missing: " + file.getFullPathName().quoted());
        return false;
    }

    if (metadata.isEmpty()) {
        return true;
    }

    const auto propertyMap = buildPropertyMap(metadata.properties);
    const auto pictureList = buildPictureList(metadata.pictures);

    const auto fullPath = file.getFullPathName();

    if (isAiffFile(file)) {
        // Use the AIFF type directly so we can request ID3v2.4 on save.
        TagLib::RIFF::AIFF::File aiffFile(toFileName(fullPath), false);

        if (!aiffFile.isValid()) {
            utils::logError("TagLib could not open AIFF file for writing: " + file.getFullPathName().quoted());
            return false;
        }

        if (!propertyMap.isEmpty()) {
            aiffFile.setProperties(propertyMap);
        }

        if (!pictureList.isEmpty()) {
            aiffFile.setComplexProperties("PICTURE", pictureList);
        }

        return aiffFile.save(TagLib::ID3v2::v4);
    }

    if (isWavFile(file)) {
        TagLib::RIFF::WAV::File wavFile(toFileName(fullPath), false);

        if (!wavFile.isValid()) {
            utils::logError("TagLib could not open WAV file for writing: " + file.getFullPathName().quoted());
            return false;
        }

        if (!propertyMap.isEmpty()) {
            wavFile.setProperties(propertyMap);
        }

        if (!pictureList.isEmpty()) {
            wavFile.setComplexProperties("PICTURE", pictureList);
        }

        return wavFile.save(TagLib::RIFF::WAV::File::AllTags, TagLib::File::StripOthers, TagLib::ID3v2::v4);
    }

    // Generic fallback for any other writable format.
    TagLib::FileRef fileRef(toFileName(fullPath), false);

    if (fileRef.isNull() || fileRef.file() == nullptr) {
        utils::logError("TagLib could not open file for writing: " + file.getFullPathName().quoted());
        return false;
    }

    if (!propertyMap.isEmpty()) {
        fileRef.file()->setProperties(propertyMap);
    }

    if (!pictureList.isEmpty()) {
        fileRef.file()->setComplexProperties("PICTURE", pictureList);
    }

    return fileRef.save();
}
