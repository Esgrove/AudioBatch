/// Tests for the metadata functions on the committed tagged fixture.
/// Covers cross-format text, artwork and opaque ID3v2 preservation, and processing without stale Serato overviews.

#include "analysis/AudioAnalysis.h"
#include "metadata/AudioMetadata.h"
#include "normalization/AudioNormalization.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"

#include <aifffile.h>
#include <array>
#include <attachedpictureframe.h>
#include <fileref.h>
#include <flacfile.h>
#include <flacpicture.h>
#include <generalencapsulatedobjectframe.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <privateframe.h>
#include <span>
#include <textidentificationframe.h>
#include <tpropertymap.h>
#include <unknownframe.h>
#include <wavfile.h>

namespace audiobatch::tests
{
namespace
{
/// Returns the first value of a property, or an empty string when the metadata does not have it.
juce::String firstPropertyValue(const AudioMetadata& metadata, const juce::String& key)
{
    for (const auto& property : metadata.properties) {
        if (property.key == key && !property.values.isEmpty()) {
            return property.values[0];
        }
    }

    return {};
}
}  // namespace

/// Checks that tags survive reading and writing across formats.
class MetadataTests final : public juce::UnitTest
{
public:
    /// Registers metadata preservation checks with the shared test runner.
    MetadataTests() : juce::UnitTest("Metadata", "AudioBatch") {}

    /// Runs the metadata checks.
    void runTest() override
    {
        AudioMetadata metadata;

        beginTest("Tags are read from a FLAC file");
        {
            expect(readMetadata(fixtureFile("tagged_stereo_44100_16.flac"), metadata));
            expectEquals(firstPropertyValue(metadata, "TITLE"), juce::String("Test Tone"));
            expectEquals(firstPropertyValue(metadata, "ARTIST"), juce::String("AudioBatch"));
            expectEquals(firstPropertyValue(metadata, "ALBUM"), juce::String("Fixtures"));
        }

        beginTest("Tags written to an AIFF file read back unchanged");
        {
            const TemporaryDirectory directory;
            const auto output = directory.copyFixture("sine_stereo_44100_16.aif");
            expect(writeMetadata(output, metadata));

            AudioMetadata written;
            expect(readMetadata(output, written));
            expectEquals(firstPropertyValue(written, "TITLE"), juce::String("Test Tone"));
            expectEquals(firstPropertyValue(written, "ARTIST"), juce::String("AudioBatch"));
            // ffmpeg stores the comment of the fixture as the Vorbis DESCRIPTION field,
            // which is written to the AIFF file as a custom ID3v2 frame.
            expectEquals(firstPropertyValue(written, "DESCRIPTION"), juce::String("Custom comment"));
        }

        checkProcessingFrames();
        checkLegacyFrames();
        checkFlacFrames();
        checkMixedFlacFrames();
        checkUnsupportedFrames();
        checkVorbisConventions();
        checkMappedTextFrames();

        beginTest("An untagged file has no metadata");
        {
            AudioMetadata untagged;
            readMetadata(fixtureFile("sine_stereo_44100_16.wav"), untagged);
            expect(untagged.isEmpty());
        }
    }

private:
    /// Returns the text of the only frame with the given ID, or an empty string when there is not exactly one.
    static juce::String onlyFrameText(const TagLib::ID3v2::Tag& tag, const char* frameId)
    {
        const auto frames = tag.frameList(frameId);
        return frames.size() == 1 ? juce::String::fromUTF8(frames.front()->toString().toCString(true)) : juce::String();
    }

    /// Checks Vorbis totals join their numbers and the encoder field becomes TSSE, as the Rust application writes them.
    void checkVorbisConventions()
    {
        beginTest("Vorbis track and disc totals join their numbers, and the encoder becomes TSSE");
        const TemporaryDirectory directory;
        const auto source = directory.copyFixture("tagged_stereo_44100_16.flac");
        const auto output = directory.copyFixture("sine_stereo_44100_16.aif");
        {
            const auto& path = source.getFullPathName();
            TagLib::FLAC::File tagged(utils::tagLibFileName(path), false);
            auto properties = tagged.properties();
            properties.insert("TRACKNUMBER", TagLib::StringList("3"));
            properties.insert("TRACKTOTAL", TagLib::StringList("12"));
            properties.insert("DISCNUMBER", TagLib::StringList("1"));
            properties.insert("TOTALDISCS", TagLib::StringList("2"));
            properties.insert("ENCODER", TagLib::StringList("Lavc vorbis"));
            // A native Serato field becomes a GEOB frame, which must not stop the Vorbis mapping.
            juce::MemoryOutputStream wrapper;
            wrapper.writeString("application/octet-stream");
            wrapper.writeString("");
            wrapper.writeString("Serato Autotags");
            wrapper.write("\1\1", 2);
            const auto encoded = juce::Base64::toBase64(wrapper.getData(), wrapper.getDataSize());
            properties.insert(
                "SERATO_AUTOGAIN", TagLib::StringList(TagLib::String(encoded.toRawUTF8(), TagLib::String::UTF8))
            );
            tagged.setProperties(properties);
            expect(tagged.save());
        }
        expect(copyMetadata(source, output));
        const auto& path = output.getFullPathName();
        const TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
        const auto& tag = *tagged.tag();
        expectEquals(static_cast<int>(tag.frameList("GEOB").size()), 1);
        expectEquals(onlyFrameText(tag, "TRCK"), juce::String("3/12"));
        expectEquals(onlyFrameText(tag, "TPOS"), juce::String("1/2"));
        expectEquals(onlyFrameText(tag, "TSSE"), juce::String("Lavc vorbis"));
        for (const auto* frame : tag.frameList("TXXX")) {
            const auto* text = dynamic_cast<const TagLib::ID3v2::UserTextIdentificationFrame*>(frame);
            expect(text != nullptr);
            if (text != nullptr) {
                for (const auto* key : {"TRACKTOTAL", "TOTALDISCS", "ENCODER"}) {
                    expect(text->description().upper() != key, key);
                }
            }
        }
    }

    /// Checks a copied TXXX frame is not duplicated by the standard frame TagLib maps its property to.
    void checkMappedTextFrames()
    {
        beginTest("A copied TXXX frame replaces the standard frame generated for the same property");
        const TemporaryDirectory directory;
        const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "source.aif");
        const auto output = directory.copyFixture("sine_stereo_44100_16.aif", "output.aif");
        {
            const auto& path = source.getFullPathName();
            TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
            auto mood = std::make_unique<TagLib::ID3v2::UserTextIdentificationFrame>();
            mood->setDescription("MOOD");
            mood->setText("Energetic");
            tagged.tag()->addFrame(mood.release());
            tagged.tag()->setTitle("Mood Test");
            expect(tagged.save(TagLib::ID3v2::v4));
        }
        expect(copyMetadata(source, output));
        const auto& path = output.getFullPathName();
        const TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
        const auto& tag = *tagged.tag();
        expect(tag.frameList("TMOO").isEmpty());
        expectEquals(static_cast<int>(tag.frameList("TXXX").size()), 1);
        expectEquals(onlyFrameText(tag, "TIT2"), juce::String("Mood Test"));
    }

    /// Checks unknown custom frame IDs survive and encrypted known frames stop processing before source replacement.
    void checkUnsupportedFrames()
    {
        beginTest("Unknown custom frame IDs survive normalization");
        const TemporaryDirectory directory;
        const auto source = directory.copyFixture("sine_stereo_44100_16.aif");
        {
            const auto& path = source.getFullPathName();
            TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
            const TagLib::ByteVector bytes("XABC\0\0\0\4\0\0data", 14);
            auto frame = std::make_unique<TagLib::ID3v2::UnknownFrame>(bytes);
            tagged.tag()->addFrame(frame.release());
            expect(tagged.save(TagLib::ID3v2::v4));
        }
        AudioMetadata before;
        expect(readMetadata(source, before));
        expect(!before.hasUnsupportedId3Frames);
        const auto normalized = normalizeFile(analyzeFile(source), testNormalizationOptions());
        expect(!normalized.hasError(), outcomeError(normalized.outcome));
        AudioMetadata after;
        expect(readMetadata(source, after));
        expect(after.id3v2Frames == before.id3v2Frames);

        beginTest("Encrypted known frames keep the original file intact rather than risk tag loss");
        {
            const auto& path = source.getFullPathName();
            TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
            auto frame = std::make_unique<TagLib::ID3v2::GeneralEncapsulatedObjectFrame>();
            frame->setDescription("Serato Markers2");
            frame->setObject("opaque encrypted data");
            tagged.tag()->addFrame(frame.release());
            expect(tagged.save(TagLib::ID3v2::v4));
        }
        juce::MemoryBlock original;
        expect(source.loadFileAsData(original));
        const auto bytes = std::span(static_cast<char*>(original.getData()), original.getSize());
        constexpr std::array frameId {'G', 'E', 'O', 'B'};
        const auto frame = std::ranges::search(bytes, frameId);
        expect(!frame.empty());
        if (frame.empty()) {
            return;
        }
        constexpr std::size_t flagsOffset = 9;
        constexpr char encryptionFlag = 4;
        bytes.subspan(static_cast<std::size_t>(frame.begin() - bytes.begin()))[flagsOffset] = encryptionFlag;
        expect(source.replaceWithData(original.getData(), original.getSize()));
        auto options = testNormalizationOptions();
        options.targetPeakDbfs = -12.0;
        const auto result = normalizeFile(analyzeFile(source), options);
        expect(result.hasError());
        juce::MemoryBlock unchanged;
        expect(source.loadFileAsData(unchanged));
        expect(original == unchanged);
    }

    /// Checks optional FLAC ID3 tags neither replace native custom values nor accumulate translated objects.
    void checkMixedFlacFrames()
    {
        beginTest("Mixed native and ID3 FLAC tags retain custom fields, artwork and stable Serato frame counts");
        const TemporaryDirectory directory;
        const auto source = directory.copyFixture("tagged_stereo_44100_16.flac");
        const auto output = directory.copyFixture("sine_stereo_44100_16.aif", "mixed.aif");
        const auto second = directory.copyFixture("sine_stereo_44100_16.aif", "second.aif");
        const TagLib::ByteVector payload("\0\1\2\xff", 4);
        {
            const auto& path = source.getFullPathName();
            TagLib::FLAC::File tagged(utils::tagLibFileName(path), false);
            auto properties = tagged.properties();
            properties.insert("NATIVE_CUSTOM", TagLib::StringList("native value"));
            juce::MemoryOutputStream wrapper;
            wrapper.writeString("application/octet-stream");
            wrapper.writeString("");
            wrapper.writeString("Serato BeatGrid");
            wrapper.write(payload.data(), payload.size());
            const auto encoded = juce::Base64::toBase64(wrapper.getData(), wrapper.getDataSize());
            properties.insert(
                "SERATO_BEATGRID", TagLib::StringList(TagLib::String(encoded.toRawUTF8(), TagLib::String::UTF8))
            );
            tagged.setProperties(properties);
            auto picture = std::make_unique<TagLib::FLAC::Picture>();
            picture->setMimeType("image/jpeg");
            picture->setType(TagLib::FLAC::Picture::FrontCover);
            picture->setData("native picture");
            tagged.addPicture(picture.release());
            auto text = std::make_unique<TagLib::ID3v2::UserTextIdentificationFrame>();
            text->setDescription("ID3_CUSTOM");
            text->setText("ID3 value");
            auto* tag = tagged.ID3v2Tag(true);
            tag->addFrame(text.release());
            auto object = std::make_unique<TagLib::ID3v2::GeneralEncapsulatedObjectFrame>();
            object->setDescription("Serato BeatGrid");
            object->setMimeType("application/octet-stream");
            object->setObject(payload);
            tag->addFrame(object.release());
            auto id3Picture = std::make_unique<TagLib::ID3v2::AttachedPictureFrame>();
            id3Picture->setMimeType("image/jpeg");
            id3Picture->setType(TagLib::ID3v2::AttachedPictureFrame::BackCover);
            id3Picture->setPicture("ID3 picture");
            tag->addFrame(id3Picture.release());
            expect(tagged.save());
        }
        AudioMetadata metadata;
        expect(readMetadata(source, metadata));
        const auto frameCount = metadata.id3v2Frames.size();
        expect(writeMetadata(source, metadata));
        expect(readMetadata(source, metadata));
        expect(metadata.id3v2Frames.size() == frameCount);
        expect(copyMetadata(source, output));
        expect(copyMetadata(output, second));
        for (const auto& file : {output, second}) {
            AudioMetadata written;
            expect(readMetadata(file, written));
            expectEquals(firstPropertyValue(written, "NATIVE_CUSTOM"), juce::String("native value"));
            expectEquals(firstPropertyValue(written, "ID3_CUSTOM"), juce::String("ID3 value"));
            expectEquals(static_cast<int>(written.pictures.size()), 2);
            const auto& path = file.getFullPathName();
            const TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
            expectEquals(static_cast<int>(tagged.tag()->frameList("GEOB").size()), 1);
        }
    }

    /// Checks lossless binary copying and overview removal through in-place normalization.
    void checkProcessingFrames()
    {
        beginTest("Processing copies opaque custom frames but drops the Serato overview");
        {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "source.aif");
            const auto output = directory.copyFixture("sine_stereo_44100_16.aif", "output.aif");
            const TagLib::ByteVector payload("\0\1\2\xff", 4);
            {
                const auto& path = source.getFullPathName();
                TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
                TagLib::PropertyMap properties;
                properties.insert("SERATO_OVERVIEW", TagLib::StringList("stale overview"));
                tagged.setProperties(properties);
                for (const auto* description :
                     {
                         "Serato Markers2",
                         "Serato Markers_",
                         "Serato BeatGrid",
                         "Serato Analysis",
                         "Serato Autotags",
                         "Custom Object",
                         "Serato Overview",
                     })
                {
                    auto frame = std::make_unique<TagLib::ID3v2::GeneralEncapsulatedObjectFrame>();
                    frame->setDescription(description);
                    frame->setMimeType("application/octet-stream");
                    frame->setFileName("custom.bin");
                    frame->setObject(payload);
                    tagged.tag()->addFrame(frame.release());
                }
                auto privateFrame = std::make_unique<TagLib::ID3v2::PrivateFrame>();
                privateFrame->setOwner("custom-owner");
                privateFrame->setData(payload);
                tagged.tag()->addFrame(privateFrame.release());
                expect(tagged.save(TagLib::ID3v2::v4));
            }

            expect(copyMetadata(source, output));
            {
                const auto& path = output.getFullPathName();
                const TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
                expectEquals(static_cast<int>(tagged.tag()->frameList("GEOB").size()), 6);
                for (const auto* frame : tagged.tag()->frameList("GEOB")) {
                    const auto* object = dynamic_cast<const TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frame);
                    expect(object != nullptr);
                    if (object != nullptr) {
                        expect(object->description() != "Serato Overview");
                        expect(object->object() == payload);
                        expect(object->fileName() == "custom.bin");
                        expect(object->mimeType() == "application/octet-stream");
                    }
                }
                const auto privateFrames = tagged.tag()->frameList("PRIV");
                expectEquals(static_cast<int>(privateFrames.size()), 1);
                if (!privateFrames.isEmpty()) {
                    const auto* privateFrame = dynamic_cast<const TagLib::ID3v2::PrivateFrame*>(privateFrames.front());
                    expect(privateFrame != nullptr);
                    if (privateFrame != nullptr) {
                        expect(privateFrame->owner() == "custom-owner");
                        expect(privateFrame->data() == payload);
                    }
                }
            }

            AudioMetadata expected;
            expect(readMetadata(output, expected));
            expect(firstPropertyValue(expected, "SERATO_OVERVIEW").isEmpty());
            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
            expect(!result.hasError(), outcomeError(result.outcome));
            AudioMetadata normalized;
            expect(readMetadata(source, normalized));
            expect(normalized.id3v2Frames == expected.id3v2Frames);
        }
    }

    /// Checks opaque data in older ID3v2 versions and non-AIFF source containers.
    void checkLegacyFrames()
    {
        beginTest("ID3v2.3 MP3 and WAV custom frames survive conversion to AIFF");
        for (const auto* fixture : {"sine_stereo_44100_320k.mp3", "sine_stereo_44100_16.wav"}) {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture(fixture);
            const auto output = directory.copyFixture("sine_stereo_44100_16.aif");
            const TagLib::ByteVector payload(256, '\xff');
            {
                const auto& path = source.getFullPathName();
                const TagLib::FileRef reference(utils::tagLibFileName(path), false);
                auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(reference.file());
                auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(reference.file());
                expect(mpeg != nullptr || wav != nullptr);
                if (mpeg == nullptr && wav == nullptr) {
                    continue;
                }
                auto* tag = mpeg != nullptr ? mpeg->ID3v2Tag(true) : wav->ID3v2Tag();
                for (const auto* description : {"Serato Markers2", "Serato Overview"}) {
                    auto frame = std::make_unique<TagLib::ID3v2::GeneralEncapsulatedObjectFrame>();
                    frame->setDescription(description);
                    frame->setObject(payload);
                    tag->addFrame(frame.release());
                }
                if (mpeg != nullptr) {
                    expect(mpeg->save(TagLib::MPEG::File::AllTags, TagLib::File::StripNone, TagLib::ID3v2::v3));
                } else {
                    expect(wav->save(TagLib::RIFF::WAV::File::AllTags, TagLib::File::StripNone, TagLib::ID3v2::v3));
                }
            }
            expect(copyMetadata(source, output));
            const auto& path = output.getFullPathName();
            const TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
            const auto frames = tagged.tag()->frameList("GEOB");
            expectEquals(static_cast<int>(frames.size()), 1);
            if (!frames.isEmpty()) {
                const auto* object = dynamic_cast<const TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frames.front());
                expect(object != nullptr);
                if (object != nullptr) {
                    expect(object->description() == "Serato Markers2");
                    expect(object->object() == payload);
                }
            }
        }

        beginTest("An ID3v2.3 date split over TYER, TDAT and TIME survives as one TDRC frame");
        {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture("sine_stereo_44100_320k.mp3");
            const auto output = directory.copyFixture("sine_stereo_44100_16.aif");
            {
                const auto& path = source.getFullPathName();
                TagLib::MPEG::File mpeg(utils::tagLibFileName(path), false);
                auto properties = mpeg.properties();
                properties.replace("DATE", TagLib::StringList("2024-05-17T10:30"));
                mpeg.setProperties(properties);
                expect(mpeg.save(TagLib::MPEG::File::AllTags, TagLib::File::StripNone, TagLib::ID3v2::v3));
            }
            {
                const auto& path = source.getFullPathName();
                TagLib::MPEG::File mpeg(utils::tagLibFileName(path), false);
                expect(!mpeg.ID3v2Tag()->frameList("TDAT").isEmpty());
            }
            expect(copyMetadata(source, output));
            const auto& path = output.getFullPathName();
            const TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
            expectEquals(onlyFrameText(*tagged.tag(), "TDRC"), juce::String("2024-05-17T10:30"));
            expect(tagged.tag()->frameList("TDAT").isEmpty());
        }
    }

    /// Checks native FLAC wrappers are translated without losing their original custom text values.
    void checkFlacFrames()
    {
        beginTest("Native FLAC Serato fields become usable AIFF objects without the overview");
        {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture("tagged_stereo_44100_16.flac");
            const auto output = directory.copyFixture("sine_stereo_44100_16.aif");
            const TagLib::ByteVector payload("\0\1\2\xff", 4);
            {
                const auto& path = source.getFullPathName();
                TagLib::FLAC::File tagged(utils::tagLibFileName(path), false);
                auto properties = tagged.properties();
                for (const auto& [key, description] :
                     {
                         std::pair {"SERATO_MARKERS_V2", "Serato Markers2"},
                         std::pair {"SERATO_MARKERS", "Serato Markers_"},
                         std::pair {"SERATO_BEATGRID", "Serato BeatGrid"},
                         std::pair {"SERATO_ANALYSIS", "Serato Analysis"},
                         std::pair {"SERATO_AUTOGAIN", "Serato Autotags"},
                         std::pair {"SERATO_OVERVIEW", "Serato Overview"},
                     })
                {
                    juce::MemoryOutputStream wrapper;
                    wrapper.writeString("application/octet-stream");
                    wrapper.writeString("");
                    wrapper.writeString(description);
                    wrapper.write(payload.data(), payload.size());
                    const auto encoded = juce::Base64::toBase64(wrapper.getData(), wrapper.getDataSize());
                    properties.insert(
                        key, TagLib::StringList(TagLib::String(encoded.toRawUTF8(), TagLib::String::UTF8))
                    );
                }
                properties.insert("CUSTOM_DATA", TagLib::StringList("opaque custom value"));
                tagged.setProperties(properties);
                expect(tagged.save());
            }
            expect(copyMetadata(source, output));
            AudioMetadata written;
            expect(readMetadata(output, written));
            expectEquals(firstPropertyValue(written, "CUSTOM_DATA"), juce::String("opaque custom value"));
            expect(firstPropertyValue(written, "SERATO_MARKERS_V2").isNotEmpty());
            expect(firstPropertyValue(written, "SERATO_OVERVIEW").isEmpty());
            const auto& path = output.getFullPathName();
            const TagLib::RIFF::AIFF::File tagged(utils::tagLibFileName(path), false);
            expectEquals(static_cast<int>(tagged.tag()->frameList("GEOB").size()), 5);
            for (const auto* frame : tagged.tag()->frameList("GEOB")) {
                const auto* object = dynamic_cast<const TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frame);
                expect(object != nullptr);
                if (object != nullptr) {
                    expect(object->description() != "Serato Overview");
                    expect(object->object() == payload);
                }
            }
        }
    }
};

/// Creates the metadata suite for registration with the shared runner.
std::unique_ptr<juce::UnitTest> createMetadataTests()
{
    return std::make_unique<MetadataTests>();
}
}  // namespace audiobatch::tests
