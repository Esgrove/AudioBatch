#include "MetadataService.h"

#include "utils.h"

#include <aifffile.h>
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
juce::String toJuce(const TagLib::String& s)
{
    return juce::String::fromUTF8(s.toCString(true));
}

TagLib::String toTagLib(const juce::String& s)
{
    return {s.toRawUTF8(), TagLib::String::UTF8};
}

#if JUCE_WINDOWS
TagLib::FileName toFileName(const juce::File& file)
{
    return TagLib::FileName(file.getFullPathName().toWideCharPointer());
}
#else
TagLib::FileName toFileName(const juce::File& file)
{
    return TagLib::FileName(file.getFullPathName().toRawUTF8());
}
#endif

bool isAiffFile(const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();
    return extension == ".aif" || extension == ".aiff" || extension == ".aifc";
}

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

std::vector<MetadataService::Picture> readPictures(TagLib::File& file)
{
    std::vector<MetadataService::Picture> pictures;

    const auto keys = file.complexPropertyKeys();
    const bool hasPictureKey
        = std::any_of(keys.begin(), keys.end(), [](const TagLib::String& key) { return key == "PICTURE"; });

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

TagLib::List<TagLib::VariantMap> buildPictureList(const std::vector<MetadataService::Picture>& pictures)
{
    TagLib::List<TagLib::VariantMap> pictureList;

    for (const auto& picture : pictures) {
        if (picture.data.getSize() == 0) {
            continue;
        }

        TagLib::VariantMap pictureMap;

        const TagLib::ByteVector bytes(
            static_cast<const char*>(picture.data.getData()), static_cast<unsigned int>(picture.data.getSize())
        );
        pictureMap.insert("data", TagLib::Variant(bytes));

        const auto mimeType = picture.mimeType.isEmpty() ? juce::String("image/jpeg") : picture.mimeType;
        pictureMap.insert("mimeType", TagLib::Variant(toTagLib(mimeType)));

        const auto pictureType = picture.pictureType.isEmpty() ? juce::String("Front Cover") : picture.pictureType;
        pictureMap.insert("pictureType", TagLib::Variant(toTagLib(pictureType)));

        pictureMap.insert("description", TagLib::Variant(toTagLib(picture.description)));

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

    const TagLib::FileRef ref(toFileName(file), false);

    if (ref.isNull() || ref.file() == nullptr) {
        utils::log_error("TagLib could not open file for reading: " + file.getFullPathName());
        return false;
    }

    auto* taglibFile = ref.file();
    outMetadata.properties = readProperties(taglibFile->properties());
    outMetadata.pictures = readPictures(*taglibFile);

    return true;
}

bool MetadataService::writeMetadata(const juce::File& file, const Metadata& metadata)
{
    if (!file.existsAsFile()) {
        utils::log_error("Cannot write metadata, target file is missing: " + file.getFullPathName());
        return false;
    }

    if (metadata.isEmpty()) {
        return true;
    }

    const auto propertyMap = buildPropertyMap(metadata.properties);
    const auto pictureList = buildPictureList(metadata.pictures);

    if (isAiffFile(file)) {
        // Use the AIFF type directly so we can request ID3v2.4 on save.
        TagLib::RIFF::AIFF::File aiffFile(toFileName(file), false);

        if (!aiffFile.isValid()) {
            utils::log_error("TagLib could not open AIFF file for writing: " + file.getFullPathName());
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
        TagLib::RIFF::WAV::File wavFile(toFileName(file), false);

        if (!wavFile.isValid()) {
            utils::log_error("TagLib could not open WAV file for writing: " + file.getFullPathName());
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
    TagLib::FileRef ref(toFileName(file), false);

    if (ref.isNull() || ref.file() == nullptr) {
        utils::log_error("TagLib could not open file for writing: " + file.getFullPathName());
        return false;
    }

    if (!propertyMap.isEmpty()) {
        ref.file()->setProperties(propertyMap);
    }

    if (!pictureList.isEmpty()) {
        ref.file()->setComplexProperties("PICTURE", pictureList);
    }

    return ref.save();
}
