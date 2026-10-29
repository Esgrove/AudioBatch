/// Implementation of the metadata functions.
/// Converts between JUCE and TagLib string and buffer types, maps tags through TagLib's unified PropertyMap
/// and the generic PICTURE complex property, saves tags for AIFF and WAV output as ID3v2.4,
/// retains opaque ID3v2 frames, and drops stale Serato overviews when copying processed audio.
/// Vorbis track and disc totals and the encoder field are mapped onto the ID3v2 frames meant for them.

#include "metadata/AudioMetadata.h"

#include "serato/SeratoTags.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include <initializer_list>
#include <string_view>

#include <aifffile.h>
#include <algorithm>
#include <array>
#include <attachedpictureframe.h>
#include <cstddef>
#include <cstdint>
#include <fileref.h>
#include <flacfile.h>
#include <generalencapsulatedobjectframe.h>
#include <id3v2.h>
#include <id3v2framefactory.h>
#include <id3v2header.h>
#include <id3v2tag.h>
#include <memory>
#include <string>
#include <tbytevector.h>
#include <textidentificationframe.h>
#include <tfile.h>
#include <tlist.h>
#include <tpropertymap.h>
#include <tstring.h>
#include <tstringlist.h>
#include <tvariant.h>
#include <unknownframe.h>
#include <utility>
#include <vector>
#include <wavfile.h>

namespace audiobatch
{
namespace
{
/// ID3v2.3 frames that ID3v2.4 drops.
/// TagLib folds TDAT and TIME into TDRC when reading and discards all of them when saving as ID3v2.4.
constexpr std::array obsoleteId3v23Frames {"EQUA", "RVAD", "TDAT", "TIME", "TRDA", "TSIZ"};

/// Returns a Serato tag name as a JUCE string.
juce::String toJuce(const std::string_view name)
{
    return {name.data(), name.size()};
}

/// Recreates a serialized ID3v2.4 frame with ownership held by the caller until the destination takes it.
std::unique_ptr<TagLib::ID3v2::Frame> restoreId3Frame(const juce::MemoryBlock& bytes)
{
    const TagLib::ByteVector data(
        static_cast<const char*>(bytes.getData()), static_cast<unsigned int>(bytes.getSize())
    );
    TagLib::ID3v2::Header header;
    header.setMajorVersion(4);
    return std::unique_ptr<TagLib::ID3v2::Frame>(TagLib::ID3v2::FrameFactory::instance()->createFrame(data, &header));
}

/// Serializes frames independently of the source file, preserving unknown custom frame IDs too.
/// Unsupported known frames fail safely if serialization would reinterpret an opaque payload as decoded fields.
std::vector<juce::MemoryBlock> readId3Frames(TagLib::File& file, bool& hasUnsupportedFrames)
{
    const auto* tag = serato::findId3Tag(file);
    if (tag == nullptr) {
        return {};
    }
    const auto isObsolete = [tag](const TagLib::ID3v2::Frame& frame) {
        return tag->header()->majorVersion() < 4
            && std::ranges::any_of(
                   obsoleteId3v23Frames, [&frame](const char* frameId) { return frame.frameID() == frameId; }
            );
    };
    std::vector<juce::MemoryBlock> frames;
    frames.reserve(tag->frameList().size());
    for (const auto* frame : tag->frameList()) {
        if (isObsolete(*frame)) {
            continue;
        }
        // A frame read from an ID3v2.3 tag renders in the v2.3 layout until its header says otherwise,
        // and restoreId3Frame parses the bytes as ID3v2.4.
        frame->header()->setVersion(4);
        const auto bytes = frame->render();
        frames.emplace_back(bytes.data(), bytes.size());
        if (dynamic_cast<const TagLib::ID3v2::UnknownFrame*>(frame) != nullptr) {
            const auto restored = restoreId3Frame(frames.back());
            if (dynamic_cast<const TagLib::ID3v2::UnknownFrame*>(restored.get()) == nullptr) {
                hasUnsupportedFrames = true;
            }
        }
    }
    return frames;
}

/// Returns true for text information frames, including TXXX, whose IDs start with T.
bool isTextFrame(const TagLib::ID3v2::Frame& frame)
{
    return frame.frameID().startsWith("T");
}

/// Matches equivalent generated frames without replacing unrelated native FLAC custom fields or artwork.
/// Text frames with different IDs match when they hold the same property,
/// such as a copied TXXX:MOOD frame and the TMOO frame TagLib generates for its MOOD property.
bool equivalentId3Frames(const TagLib::ID3v2::Frame& first, const TagLib::ID3v2::Frame& second)
{
    if (first.frameID() != second.frameID() && !(isTextFrame(first) && isTextFrame(second))) {
        return false;
    }
    const auto firstBytes = first.render();
    const auto secondBytes = second.render();
    if (firstBytes == secondBytes) {
        return true;
    }
    const auto* firstPicture = dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame*>(&first);
    const auto* secondPicture = dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame*>(&second);
    if (firstPicture != nullptr && secondPicture != nullptr) {
        return firstPicture->picture() == secondPicture->picture() && firstPicture->type() == secondPicture->type()
            && firstPicture->description() == secondPicture->description();
    }
    // TagLib's addFrame dereferences the frame, so a payload that cannot be parsed back is never passed to it.
    auto firstRestored = restoreId3Frame(juce::MemoryBlock(firstBytes.data(), firstBytes.size()));
    auto secondRestored = restoreId3Frame(juce::MemoryBlock(secondBytes.data(), secondBytes.size()));
    if (firstRestored == nullptr || secondRestored == nullptr) {
        return false;
    }
    TagLib::ID3v2::Tag firstTag;
    TagLib::ID3v2::Tag secondTag;
    firstTag.addFrame(firstRestored.release());
    secondTag.addFrame(secondRestored.release());
    const auto properties = firstTag.properties();
    return !properties.isEmpty() && properties == secondTag.properties();
}

/// Replaces equivalent mapped frames with their original forms, retaining duplicates and opaque payloads.
/// Invalid serialized frames fail the write rather than silently discarding custom data.
bool writeId3Frames(TagLib::ID3v2::Tag& tag, const std::vector<juce::MemoryBlock>& bytes)
{
    std::vector<std::unique_ptr<TagLib::ID3v2::Frame>> frames;
    frames.reserve(bytes.size());
    for (const auto& data : bytes) {
        auto frame = restoreId3Frame(data);
        if (frame == nullptr) {
            return false;
        }
        frames.push_back(std::move(frame));
    }
    for (const auto& frame : frames) {
        // A copy of the tag's own frame list would share its ownership and delete the frames when destroyed,
        // so the candidates are collected as plain pointers.
        const auto& list = isTextFrame(*frame) ? tag.frameList() : tag.frameList(frame->frameID());
        const std::vector<TagLib::ID3v2::Frame*> candidates(list.begin(), list.end());
        for (auto* candidate : candidates) {
            if (equivalentId3Frames(*frame, *candidate)) {
                tag.removeFrame(candidate);
            }
        }
    }
    for (auto& frame : frames) {
        tag.addFrame(frame.release());
    }
    return true;
}

/// Discards only the overview whose raster no longer represents the processed audio.
/// Other Serato fields and unrelated custom frames are retained even when their payloads are unrecognized.
void removeSeratoOverview(AudioMetadata& metadata)
{
    const auto property = toJuce(serato::overviewNames.property);
    std::erase_if(metadata.properties, [&property](const MetadataProperty& metadataProperty) {
        return metadataProperty.key.equalsIgnoreCase(property);
    });
    const TagLib::String frameDescription(std::string(serato::overviewNames.description));
    const TagLib::String textDescription(std::string(serato::overviewNames.property));
    std::erase_if(metadata.id3v2Frames, [&frameDescription, &textDescription](const juce::MemoryBlock& bytes) {
        const auto frame = restoreId3Frame(bytes);
        const auto* object = dynamic_cast<const TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frame.get());
        const auto* text = dynamic_cast<const TagLib::ID3v2::UserTextIdentificationFrame*>(frame.get());
        return frame != nullptr
            && ((object != nullptr && object->description() == frameDescription)
                || (text != nullptr && text->description().upper() == textDescription));
    });
}

/// Appends a GEOB frame for a valid native Serato value unless an equivalent frame is already present.
/// Malformed values, unexpected wrapper fields, and empty payloads are ignored without modifying the original property.
void appendSeratoFrame(AudioMetadata& metadata, const juce::String& value, const std::string_view expectedDescription)
{
    const auto encoded = value.toStdString();
    const std::vector<std::uint8_t> bytes(encoded.begin(), encoded.end());
    const auto decoded = serato::decodeTagBase64(bytes);
    if (!decoded) {
        return;
    }
    const std::span decodedBytes {static_cast<const std::uint8_t*>(decoded->getData()), decoded->getSize()};
    const auto wrappedPayload = serato::parseTagWrapper(decodedBytes, expectedDescription);
    if (!wrappedPayload.has_value() || wrappedPayload->empty()) {
        return;
    }
    TagLib::ID3v2::GeneralEncapsulatedObjectFrame frame;
    frame.setMimeType("application/octet-stream");
    frame.setDescription(TagLib::String(std::string(expectedDescription)));
    const auto payloadOffset = static_cast<std::size_t>(wrappedPayload->data() - decodedBytes.data());
    const auto payload
        = std::span(static_cast<const char*>(decoded->getData()), decoded->getSize()).subspan(payloadOffset);
    frame.setObject(TagLib::ByteVector(payload.data(), static_cast<unsigned int>(payload.size())));
    const auto rendered = frame.render();
    const juce::MemoryBlock frameBytes(rendered.data(), rendered.size());
    const bool alreadyPresent = std::ranges::any_of(metadata.id3v2Frames, [&frame](const auto& existing) {
        const auto restored = restoreId3Frame(existing);
        const auto* object = dynamic_cast<const TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(restored.get());
        return object != nullptr && object->description() == frame.description() && object->object() == frame.object()
            && object->mimeType() == frame.mimeType() && object->fileName() == frame.fileName();
    });
    if (!alreadyPresent) {
        metadata.id3v2Frames.push_back(frameBytes);
    }
}

/// Finds a property by its case-insensitive key.
std::vector<MetadataProperty>::iterator findProperty(std::vector<MetadataProperty>& properties, const char* key)
{
    return std::ranges::find_if(properties, [key](const MetadataProperty& property) {
        return property.key.equalsIgnoreCase(key);
    });
}

/// Joins a number property with the first of its total properties into the "number/total" form
/// that ID3v2 stores in a single frame, and drops the separate total.
/// A total without a number stays as it is.
void joinTotal(
    std::vector<MetadataProperty>& properties,
    const char* numberKey,
    const std::initializer_list<const char*> totalKeys
)
{
    for (const auto* totalKey : totalKeys) {
        const auto total = findProperty(properties, totalKey);
        const auto number = findProperty(properties, numberKey);
        if (total == properties.end() || number == properties.end() || total->values.isEmpty()
            || number->values.size() != 1 || number->values[0].contains("/"))
        {
            continue;
        }
        number->values.set(0, number->values[0] + "/" + total->values[0]);
        properties.erase(total);
        return;
    }
}

/// Maps the Vorbis comment conventions of a non-ID3v2 source onto the frames ID3v2 has for them:
/// the track and disc totals join their numbers in TRCK and TPOS,
/// and the encoder becomes TSSE through TagLib's ENCODING property.
void mapVorbisConventions(AudioMetadata& metadata)
{
    if (metadata.hasId3Tag) {
        return;
    }
    auto& properties = metadata.properties;
    joinTotal(properties, "TRACKNUMBER", {"TRACKTOTAL", "TOTALTRACKS"});
    joinTotal(properties, "DISCNUMBER", {"DISCTOTAL", "TOTALDISCS"});
    if (const auto encoder = findProperty(properties, "ENCODER");
        encoder != properties.end() && findProperty(properties, "ENCODING") == properties.end())
    {
        encoder->key = "ENCODING";
    }
}

/// Adds equivalent GEOB frames for documented native Serato properties so converted AIFF tags remain usable.
/// Original property values are retained too, including malformed or unknown custom fields.
void appendSeratoFrames(AudioMetadata& metadata)
{
    for (const auto& property : metadata.properties) {
        // libc++ uses a pointer here, but MSVC uses an iterator class.
        // Convert to a pointer explicitly so auto* is portable and satisfies clang-tidy.
        const auto* const names
            = std::to_address(std::ranges::find_if(serato::allTagNames, [&property](const auto& entry) {
                  return property.key.equalsIgnoreCase(toJuce(entry.property));
              }));
        if (names == std::to_address(serato::allTagNames.end())) {
            continue;
        }
        for (const auto& value : property.values) {
            appendSeratoFrame(metadata, value, names->description);
        }
    }
}

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

/// Translates a TagLib PropertyMap into our format-agnostic representation.
std::vector<MetadataProperty> readProperties(const TagLib::PropertyMap& propertyMap)
{
    std::vector<MetadataProperty> entries;
    entries.reserve(propertyMap.size());

    for (const auto& [key, values] : propertyMap) {
        MetadataProperty entry;
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
TagLib::PropertyMap buildPropertyMap(const std::vector<MetadataProperty>& properties)
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
std::vector<MetadataPicture> readPictures(const TagLib::File& file)
{
    std::vector<MetadataPicture> pictures;

    const auto keys = file.complexPropertyKeys();
    const bool hasPictureKey = std::ranges::any_of(keys, [](const TagLib::String& key) { return key == "PICTURE"; });

    if (!hasPictureKey) {
        return pictures;
    }

    const auto pictureMaps = file.complexProperties("PICTURE");
    pictures.reserve(pictureMaps.size());

    for (const auto& pictureMap : pictureMaps) {
        MetadataPicture picture;

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
TagLib::List<TagLib::VariantMap> buildPictureList(const std::vector<MetadataPicture>& pictures)
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

/// Applies properties and artwork before restoring original ID3v2 frames.
/// Fails rather than discarding frames when the destination has no writable ID3v2 tag.
bool applyMetadata(
    TagLib::File& file,
    const TagLib::PropertyMap& properties,
    const TagLib::List<TagLib::VariantMap>& pictures,
    const std::vector<juce::MemoryBlock>& frames
)
{
    if (!properties.isEmpty()) {
        file.setProperties(properties);
    }
    if (!pictures.isEmpty()) {
        file.setComplexProperties("PICTURE", pictures);
    }
    if (frames.empty()) {
        return true;
    }
    auto* tag = serato::findId3Tag(file, true);
    return tag != nullptr && writeId3Frames(*tag, frames);
}
}  // namespace

/// Reads only the embedded pictures of a file for display, without its text tags or opaque ID3v2 frames.
/// Front covers come first, then the other pictures in tag order.
/// Pictures over the byte limit are skipped, and at most the given number of pictures are returned.
/// Returns an empty list for a missing file, a file TagLib cannot open, or a file without pictures.
std::vector<MetadataPicture> readArtworkCandidates(const juce::File& file, const ArtworkReadLimits& limits)
{
    if (!file.existsAsFile() || limits.maximumCount == 0) {
        return {};
    }
    const TagLib::FileRef fileRef(utils::tagLibFileName(file.getFullPathName()), false);
    if (fileRef.isNull() || fileRef.file() == nullptr) {
        return {};
    }
    auto pictures = readPictures(*fileRef.file());
    std::erase_if(pictures, [&limits](const MetadataPicture& picture) {
        return picture.data.getSize() > limits.maximumBytes;
    });
    std::ranges::stable_partition(pictures, [](const MetadataPicture& picture) {
        return picture.pictureType == "Front Cover";
    });
    if (pictures.size() > limits.maximumCount) {
        pictures.resize(limits.maximumCount);
    }
    return pictures;
}

/// Reads text properties, pictures and opaque ID3v2 frames from the given file.
/// Returns true on success. Output is cleared on entry.
bool readMetadata(const juce::File& file, AudioMetadata& outMetadata)
{
    outMetadata = AudioMetadata {};

    if (!file.existsAsFile()) {
        return false;
    }

    const auto& fullPath = file.getFullPathName();
    const TagLib::FileRef fileRef(utils::tagLibFileName(fullPath), false);

    if (fileRef.isNull() || fileRef.file() == nullptr) {
        utils::logError("TagLib could not open file for reading: {}", file.getFullPathName().quoted());
        return false;
    }

    auto* taglibFile = fileRef.file();
    outMetadata.properties = readProperties(taglibFile->properties());
    outMetadata.pictures = readPictures(*taglibFile);
    outMetadata.id3v2Frames = readId3Frames(*taglibFile, outMetadata.hasUnsupportedId3Frames);
    outMetadata.hasId3Tag = serato::findId3Tag(*taglibFile) != nullptr;
    if (dynamic_cast<TagLib::FLAC::File*>(taglibFile) != nullptr) {
        appendSeratoFrames(outMetadata);
    }

    return true;
}

/// Writes the given metadata to the file.
/// The file must exist and be in a format TagLib can write.
/// For AIFF and WAV output the tag is saved as ID3v2.4.
bool writeMetadata(const juce::File& file, const AudioMetadata& metadata)
{
    if (metadata.hasUnsupportedId3Frames) {
        utils::logError("Cannot safely preserve unsupported ID3v2 frames in {}", file.getFullPathName().quoted());
        return false;
    }
    if (!file.existsAsFile()) {
        utils::logError("Cannot write metadata, target file is missing: {}", file.getFullPathName().quoted());
        return false;
    }

    if (metadata.isEmpty()) {
        return true;
    }

    const auto propertyMap = buildPropertyMap(metadata.properties);
    const auto pictureList = buildPictureList(metadata.pictures);

    const auto& fullPath = file.getFullPathName();

    if (utils::isAiffFile(file)) {
        // Use the AIFF type directly so we can request ID3v2.4 on save.
        TagLib::RIFF::AIFF::File aiffFile(utils::tagLibFileName(fullPath), false);

        if (!aiffFile.isValid()) {
            utils::logError("TagLib could not open AIFF file for writing: {}", file.getFullPathName().quoted());
            return false;
        }

        return applyMetadata(aiffFile, propertyMap, pictureList, metadata.id3v2Frames)
            && aiffFile.save(TagLib::ID3v2::v4);
    }

    if (utils::isWavFile(file)) {
        TagLib::RIFF::WAV::File wavFile(utils::tagLibFileName(fullPath), false);

        if (!wavFile.isValid()) {
            utils::logError("TagLib could not open WAV file for writing: {}", file.getFullPathName().quoted());
            return false;
        }

        return applyMetadata(wavFile, propertyMap, pictureList, metadata.id3v2Frames)
            && wavFile.save(TagLib::RIFF::WAV::File::AllTags, TagLib::File::StripOthers, TagLib::ID3v2::v4);
    }

    // Generic fallback for any other writable format.
    TagLib::FileRef fileRef(utils::tagLibFileName(fullPath), false);

    if (fileRef.isNull() || fileRef.file() == nullptr) {
        utils::logError("TagLib could not open file for writing: {}", file.getFullPathName().quoted());
        return false;
    }

    return applyMetadata(*fileRef.file(), propertyMap, pictureList, metadata.id3v2Frames) && fileRef.save();
}

/// Copies the tags, embedded pictures and opaque ID3v2 frames onto a freshly written processed output.
/// Drops the Serato overview, which must be regenerated for the changed audio.
/// A source in a format TagLib does not read is not an error, because some inputs have no metadata it can read.
/// A source that cannot be opened at all is, since its tags would be lost while the original goes to the trash,
/// and so is failing to write metadata that was read, so the caller can keep the original and its tags.
/// Returns false when the source could not be opened or writing failed.
bool copyMetadata(const juce::File& sourceFile, const juce::File& destinationFile)
{
    AudioMetadata metadata;

    if (!readMetadata(sourceFile, metadata)) {
        if (sourceFile.createInputStream() == nullptr) {
            utils::logError("Could not open {} to copy its metadata", sourceFile.getFullPathName().quoted());
            return false;
        }
        utils::logInfo("No metadata could be read from {}", sourceFile.getFullPathName().quoted());
        return true;
    }

    removeSeratoOverview(metadata);
    mapVorbisConventions(metadata);
    if (metadata.isEmpty()) {
        return true;
    }

    if (!writeMetadata(destinationFile, metadata)) {
        utils::logError("Failed to write metadata to {}", destinationFile.getFullPathName().quoted());
        return false;
    }

    return true;
}
}  // namespace audiobatch
