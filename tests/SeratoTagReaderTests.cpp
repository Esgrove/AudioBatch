/// Portable Serato container tests write synthetic tags only to temporary fixture copies.
/// Reader coverage depends only on committed fixtures and generated payloads.

#include "serato/SeratoTagReader.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/StringFormat.h"

#include <aifffile.h>
#include <algorithm>
#include <bit>
#include <fileref.h>
#include <flacfile.h>
#include <generalencapsulatedobjectframe.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <tpropertymap.h>
#include <wavfile.h>

namespace audiobatch::tests
{
namespace
{
/// Returns a nonstandard three-slice synthetic payload, not derived from any music file.
std::vector<std::uint8_t> readerPayload()
{
    std::vector<std::uint8_t> bytes(2 + 3 * serato::Overview::bandCount, 37);
    bytes[0] = 1;
    bytes[1] = 5;
    bytes.back() = 127;
    return bytes;
}

/// Transfers a synthetic GEOB frame to the tag, which takes ownership of it.
void addGeob(
    TagLib::ID3v2::Tag& tag,
    const TagLib::String& description,
    std::span<const std::uint8_t> bytes,
    TagLib::String::Type encoding
)
{
    auto frame = std::make_unique<TagLib::ID3v2::GeneralEncapsulatedObjectFrame>();
    frame->setTextEncoding(encoding);
    frame->setMimeType("application/octet-stream");
    frame->setDescription(description);
    TagLib::ByteVector object(static_cast<unsigned int>(bytes.size()), 0);
    std::ranges::transform(bytes, object.begin(), [](std::uint8_t byte) { return std::bit_cast<char>(byte); });
    frame->setObject(object);
    tag.addFrame(frame.release());
}

/// Writes existing ID3v2 containers on temporary copies, including an unrelated
/// Unicode description.
bool writeGeob(
    const juce::File& file,
    std::span<const std::uint8_t> bytes,
    bool invalidDuplicate = false,
    TagLib::String::Type encoding = TagLib::String::Latin1,
    TagLib::ID3v2::Version version = TagLib::ID3v2::v4,
    bool includeOverview = true
)
{
    const auto& fullPath = file.getFullPathName();
    const TagLib::FileRef reference(utils::tagLibFileName(fullPath), false);
    if (reference.isNull()) {
        return false;
    }
    auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(reference.file());
    auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(reference.file());
    auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(reference.file());
    TagLib::ID3v2::Tag* tag = nullptr;
    if (mpeg != nullptr) {
        tag = mpeg->ID3v2Tag(true);
    } else if (aiff != nullptr) {
        tag = aiff->tag();
    } else if (wav != nullptr) {
        tag = wav->ID3v2Tag();
    }
    if (tag == nullptr) {
        return false;
    }
    tag->removeFrames("GEOB");
    addGeob(*tag, TagLib::String("Other \xc3\xa4 description", TagLib::String::UTF8), bytes, encoding);
    if (includeOverview) {
        if (invalidDuplicate) {
            const std::array<std::uint8_t, 3> invalid {1, 5, 0};
            addGeob(*tag, "Serato Overview", invalid, encoding);
        }
        addGeob(*tag, "Serato Overview", bytes, encoding);
    }
    if (mpeg != nullptr) {
        return mpeg->save(TagLib::MPEG::File::AllTags, TagLib::File::StripNone, version);
    }
    if (aiff != nullptr) {
        return aiff->save(version);
    }
    return wav->save(TagLib::RIFF::WAV::File::AllTags, TagLib::File::StripNone, version);
}

/// Constructs the documented null-terminated MIME, empty filename and description wrapper.
juce::MemoryBlock flacWrapper(std::span<const std::uint8_t> bytes)
{
    static constexpr auto prefix = std::to_array("application/octet-stream\0\0Serato Overview");
    juce::MemoryBlock wrapper(prefix.data(), prefix.size());
    wrapper.append(bytes.data(), bytes.size());
    return wrapper;
}

/// Creates silence containers or generated MPEG frame headers without borrowing external audio bytes.
bool createSyntheticContainer(const juce::File& file)
{
    if (file.hasFileExtension("mp3")) {
        juce::MemoryOutputStream output;
        std::array<std::uint8_t, 417> frame {};
        frame[0] = 255;
        frame[1] = 251;
        frame[2] = 144;
        frame[3] = 100;
        for (int index = 0; index < 4; ++index) {
            output.write(frame.data(), frame.size());
        }
        return file.replaceWithData(output.getData(), output.getDataSize());
    }
    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aiff;
    juce::FlacAudioFormat flac;
    juce::AudioFormat* format = &wav;
    if (file.hasFileExtension("flac")) {
        format = &flac;
    } else if (file.hasFileExtension("aif")) {
        format = &aiff;
    }
    std::unique_ptr<juce::OutputStream> output = file.createOutputStream();
    if (!output) {
        return false;
    }
    const auto writer = format->createWriterFor(
        output, juce::AudioFormatWriterOptions().withSampleRate(44'100).withNumChannels(1).withBitsPerSample(16)
    );
    juce::AudioBuffer<float> silence(1, 64);
    silence.clear();
    return writer != nullptr && writer->writeFromAudioSampleBuffer(silence, 0, silence.getNumSamples());
}

/// Generates one supported payload for each nonstandard kind in stable kind order.
std::array<std::vector<std::uint8_t>, 5> syntheticPayloads()
{
    const auto autotags = std::to_array(
        "\x01\x01"
        "129.50\0-3.257\0"
        "0.000"
    );
    const std::vector<std::uint8_t> grid {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 67, 0, 0, 0, 37};
    juce::MemoryOutputStream body;
    body.writeByte(1);
    body.writeByte(1);
    body.writeString("CUE");
    const std::array<std::uint8_t, 13> cue {0, 2, 0, 0, 4, 210, 0, 204, 0, 0, 0, 0, 0};
    body.writeIntBigEndian(static_cast<int>(cue.size()));
    body.write(cue.data(), cue.size());
    body.writeByte(0);
    juce::MemoryOutputStream envelope;
    envelope.writeByte(1);
    envelope.writeByte(1);
    envelope.writeString(juce::Base64::toBase64(body.getData(), body.getDataSize()));
    const std::span markerBytes {static_cast<const std::uint8_t*>(envelope.getData()), envelope.getDataSize()};
    return {
        readerPayload(),
        std::vector<std::uint8_t> {2, 1},
        std::vector<std::uint8_t>(autotags.begin(), autotags.end()),
        grid,
        std::vector<std::uint8_t>(markerBytes.begin(), markerBytes.end()),
    };
}

/// Creates a legacy fourteen-slot fallback with one cue at 100 milliseconds.
std::vector<std::uint8_t> syntheticLegacyMarkers()
{
    std::vector<std::uint8_t> bytes(6 + 14 * 22 + 4, 0);
    bytes[0] = 2;
    bytes[1] = 5;
    bytes[5] = 14;
    for (std::size_t index = 0; index < 14; ++index) {
        bytes[6 + index * 22] = 127;
        bytes[6 + index * 22 + 20] = index < 5 ? 1 : 3;
    }
    bytes[6] = 0;
    bytes[10] = 100;
    return bytes;
}

/// Writes all synthetic kinds, invalid earlier duplicates and standard BPM/key into scratch containers.
bool writeSyntheticTags(
    const juce::File& file,
    bool invalidAutotags = false,
    bool legacyOnly = false,
    bool manyUnrelated = false
)
{
    const auto& fullPath = file.getFullPathName();
    const TagLib::FileRef reference(utils::tagLibFileName(fullPath), false);
    if (reference.isNull()) {
        return false;
    }
    auto properties = reference.file()->properties();
    TagLib::StringList bpm;
    bpm.append("not a number");
    bpm.append("129.50");
    properties.replace("BPM", bpm);
    properties.replace("INITIALKEY", TagLib::StringList("Gm"));
    const auto payloads = syntheticPayloads();
    const std::array<const char*, 5> descriptions {
        "Serato Overview",
        "Serato Analysis",
        "Serato Autotags",
        "Serato BeatGrid",
        "Serato Markers2",
    };
    const std::array<const char*, 5> names {
        "SERATO_OVERVIEW",
        "SERATO_ANALYSIS",
        "SERATO_AUTOGAIN",
        "SERATO_BEATGRID",
        "SERATO_MARKERS_V2",
    };
    auto* flac = dynamic_cast<TagLib::FLAC::File*>(reference.file());
    if (flac != nullptr) {
        for (std::size_t index = 0; index < payloads.size(); ++index) {
            juce::MemoryOutputStream wrapper;
            wrapper.writeString("application/octet-stream");
            wrapper.writeByte(0);
            wrapper.writeString(descriptions[index]);
            wrapper.write(payloads[index].data(), payloads[index].size());
            TagLib::StringList values("!invalid!");
            if (!invalidAutotags || index != 2) {
                const auto encoded
                    = juce::Base64::toBase64(wrapper.getData(), wrapper.getDataSize()).trimCharactersAtEnd("=");
                const auto spaced = juce::String(" \n") + encoded.substring(0, 12) + "\n" + encoded.substring(12);
                values.append(TagLib::String(spaced.toRawUTF8(), TagLib::String::UTF8));
            }
            properties.replace(names[index], values);
        }
        flac->setProperties(properties);
        return flac->save();
    }
    auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(reference.file());
    const auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(reference.file());
    const auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(reference.file());
    TagLib::ID3v2::Tag* tag = nullptr;
    if (mpeg != nullptr) {
        tag = mpeg->ID3v2Tag(true);
    } else if (aiff != nullptr) {
        tag = aiff->tag();
    } else if (wav != nullptr) {
        tag = wav->ID3v2Tag();
    }
    if (tag == nullptr) {
        return false;
    }
    tag->setProperties(properties);
    tag->removeFrames("GEOB");
    if (manyUnrelated) {
        const std::array<std::uint8_t, 1> unrelated {0};
        for (std::size_t index = 0; index <= serato::maxTagRecords; ++index) {
            addGeob(*tag, "Unrelated", unrelated, TagLib::String::Latin1);
        }
    }
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const std::array<std::uint8_t, 1> invalid {0};
        addGeob(*tag, descriptions[index], invalid, TagLib::String::UTF16);
        if ((!invalidAutotags || index != 2) && (!legacyOnly || index != 4)) {
            addGeob(*tag, descriptions[index], payloads[index], TagLib::String::UTF16);
        }
    }
    addGeob(*tag, "Serato Markers_", syntheticLegacyMarkers(), TagLib::String::Latin1);
    return reference.file()->save();
}

/// Replaces the native FLAC property on a temporary fixture copy with supplied duplicate values.
bool writeFlacValues(const juce::File& file, const juce::StringArray& values)
{
    const auto& fullPath = file.getFullPathName();
    TagLib::FLAC::File flac(utils::tagLibFileName(fullPath), false);
    if (!flac.isValid()) {
        return false;
    }
    auto properties = flac.properties();
    TagLib::StringList entries;
    for (const auto& value : values) {
        entries.append(TagLib::String(value.toRawUTF8(), TagLib::String::UTF8));
    }
    properties.replace("SERATO_OVERVIEW", entries);
    flac.setProperties(properties);
    return flac.save();
}

/// Computes a streaming FNV-1a checksum to verify extraction does not alter temporary fixtures.
std::optional<std::uint64_t> fileChecksum(const juce::File& file)
{
    const auto input = file.createInputStream();
    if (input == nullptr) {
        return std::nullopt;
    }
    std::uint64_t checksum = 14'695'981'039'346'656'037ULL;
    std::array<std::uint8_t, 8192> buffer {};
    while (!input->isExhausted()) {
        const auto count = input->read(buffer.data(), static_cast<int>(buffer.size()));
        if (count <= 0) {
            return std::nullopt;
        }
        for (const auto byte : std::span(buffer).first(static_cast<std::size_t>(count))) {
            checksum ^= byte;
            checksum *= 1'099'511'628'211ULL;
        }
    }
    return checksum;
}
}  // namespace

/// Exercises extraction and outcome distinctions without requiring proprietary local inputs.
class SeratoTagReaderTests final : public juce::UnitTest
{
public:
    /// Registers the reader suite with the explicit JUCE runner.
    SeratoTagReaderTests() : juce::UnitTest("Serato tag reader", "AudioBatch") {}

    /// Writes synthetic metadata only on temporary copies and verifies extraction is read-only.
    void runTest() override
    {
        testOverview();
        testMetadata();
        testTrackInfoFields();
    }

private:
    /// Preserves existing Overview container and outcome regression coverage.
    void testOverview()
    {
        const auto payload = readerPayload();
        const auto expected = serato::Overview::parse(payload);
        const TemporaryDirectory directory;

        beginTest(
            "GEOB extraction supports MP3, AIFF, WAV, ID3v2.3/v2.4 and text "
            "encodings"
        );
        for (const auto* fixture :
             {"sine_stereo_44100_320k.mp3", "sine_stereo_44100_16.aif", "sine_stereo_44100_16.wav"})
        {
            for (const auto version : {TagLib::ID3v2::v3, TagLib::ID3v2::v4}) {
                for (const auto encoding : {TagLib::String::Latin1, TagLib::String::UTF16, TagLib::String::UTF8}) {
                    const auto file = directory.copyFixture(
                        fixture,
                        utils::format(
                            "{}-test.{}",
                            juce::String::fromUTF8("\xc3\xa4"),
                            juce::String(fixture).fromLastOccurrenceOf(".", false, false)
                        )
                    );
                    expect(writeGeob(file, payload, true, encoding, version));
                    const auto before = fileChecksum(file);
                    const auto result = serato::readTag(file);
                    expectStatus(result, serato::ReadStatus::present);
                    if (const auto overview = tagValue<serato::Overview>(result); overview && expected) {
                        expect(overview->slices == expected->slices);
                    }
                    expect(before.has_value() && before == fileChecksum(file));
                }
            }
        }

        beginTest("Unrelated GEOB is absent and only malformed matching GEOB is invalid");
        {
            const auto file = directory.copyFixture("sine_stereo_44100_16.aif");
            expect(writeGeob(file, payload, false, TagLib::String::UTF16, TagLib::ID3v2::v4, false));
            expectStatus(serato::readTag(file), serato::ReadStatus::absent);
            auto malformed = payload;
            malformed.pop_back();
            expect(writeGeob(file, malformed));
            expectStatus(serato::readTag(file), serato::ReadStatus::invalid);
            malformed = payload;
            malformed[1] = 6;
            expect(writeGeob(file, malformed));
            expectStatus(serato::readTag(file), serato::ReadStatus::invalid);
        }

        beginTest(
            "FLAC comments support padding, whitespace, omitted padding and "
            "valid later duplicates"
        );
        const auto flacFile = directory.copyFixture("sine_stereo_44100_16.flac");
        const auto wrapper = flacWrapper(payload);
        const auto encoded = juce::Base64::toBase64(wrapper.getData(), wrapper.getSize());
        for (const auto& value :
             {
                 encoded,
                 encoded.trimCharactersAtEnd("="),
                 juce::String(" \t\r\n") + encoded.substring(0, 20) + "\n" + encoded.substring(20),
             })
        {
            expect(writeFlacValues(flacFile, {"!invalid!", value}));
            const auto before = fileChecksum(flacFile);
            const auto result = serato::readTag(flacFile);
            expectStatus(result, serato::ReadStatus::present);
            if (const auto overview = tagValue<serato::Overview>(result); overview && expected) {
                expect(overview->slices == expected->slices);
            }
            expect(before.has_value() && before == fileChecksum(flacFile));
        }

        beginTest(
            "FLAC rejects missing wrapper fields, wrong descriptions, "
            "invalid padding and oversized values"
        );
        {
            std::vector<juce::String> invalidValues {"A", "AAAA=AAA", "AA===", encoded + "!"};
            const juce::MemoryBlock missingTerminator("application/octet-stream", 24);
            invalidValues.push_back(juce::Base64::toBase64(missingTerminator.getData(), missingTerminator.getSize()));
            invalidValues.push_back(juce::Base64::toBase64(payload.data(), payload.size()));
            for (const std::size_t offset : {0U, 25U, 26U, 41U}) {
                auto corrupt = wrapper;
                const std::uint8_t changed = 'X';
                corrupt.copyFrom(&changed, static_cast<int>(offset), 1);
                invalidValues.push_back(juce::Base64::toBase64(corrupt.getData(), corrupt.getSize()));
            }
            invalidValues.push_back(juce::String::repeatedString("A", 1'500'000));
            // The first character moved up by 0x100 has the same low byte, which a Latin-1 conversion would keep.
            juce::String nonAscii;
            nonAscii << juce::String::charToString(static_cast<juce::juce_wchar>(0x100 + encoded[0]))
                     << encoded.substring(1);
            invalidValues.push_back(nonAscii);
            for (const auto& value : invalidValues) {
                expect(writeFlacValues(flacFile, {value}));
                expectStatus(serato::readTag(flacFile), serato::ReadStatus::invalid);
            }
        }

        beginTest("Absent, unsupported, I/O failure and cancellation remain distinct");
        {
            expectStatus(serato::readTag(fixtureFile("sine_stereo_44100_16.flac")), serato::ReadStatus::absent);
            expectStatus(serato::readTag(fixtureFile("sine_stereo_44100_16.wav")), serato::ReadStatus::absent);
            expectStatus(serato::readTag(fixtureFile("sine_stereo_48000.ogg")), serato::ReadStatus::unsupported);
            expectStatus(serato::readTag(fixtureFile("broken.wav")), serato::ReadStatus::ioError);
            expectStatus(serato::readTag(directory.getChildFile("missing.mp3")), serato::ReadStatus::ioError);
            expectStatus(
                serato::readTag(flacFile, serato::TagKind::overview, [] { return true; }), serato::ReadStatus::cancelled
            );
            int checks = 0;
            expectStatus(
                serato::readTag(flacFile, serato::TagKind::overview, [&checks] { return ++checks >= 2; }),
                serato::ReadStatus::cancelled
            );
        }
    }

    /// Checks multi-kind extraction on generated containers and independent track properties.
    void testMetadata()
    {
        const TemporaryDirectory directory;
        beginTest(
            "Synthetic containers return all kinds independently and recover "
            "later valid duplicates"
        );
        for (const auto* extension : {"mp3", "aif", "wav", "flac"}) {
            const auto file = directory.getChildFile(utils::format("synthetic.{}", extension));
            expect(createSyntheticContainer(file));
            expect(writeSyntheticTags(file));
            const auto before = fileChecksum(file);
            const auto results = serato::readTags(file, serato::allTagKinds);
            expectEquals(static_cast<int>(results.size()), 6);
            for (std::size_t index = 0; index < results.size(); ++index) {
                expectEquals(static_cast<int>(results[index].kind), static_cast<int>(index + 1));
                expectEquals(static_cast<int>(results[index].status()), static_cast<int>(serato::ReadStatus::present));
                expect(serato::serializeTag(results[index]).getSize() > 0);
            }
            if (results.size() == 6) {
                const auto info = tagValue<serato::TrackInfo>(results[5]);
                if (info) {
                    expectEquals(info->bpm.value_or(0), 129.5);
                    expectEquals(info->key, juce::String("Gm"));
                }
                const auto markers = tagValue<serato::Markers>(results[4]);
                if (markers && !markers->cues.empty()) {
                    expectEquals(static_cast<int>(markers->cues.front().positionMilliseconds), 1234);
                }
            }
            expect(before.has_value() && before == fileChecksum(file));
            expect(writeSyntheticTags(file, true));
            const auto mixed = serato::readTags(file, serato::allTagKinds);
            for (std::size_t index = 0; index < mixed.size(); ++index) {
                expectEquals(
                    static_cast<int>(mixed[index].status()),
                    static_cast<int>(index == 2 ? serato::ReadStatus::invalid : serato::ReadStatus::present)
                );
            }
            const auto cancelled = serato::readTags(file, serato::allTagKinds, [] { return true; });
            for (const auto& result : cancelled) {
                expectEquals(static_cast<int>(result.status()), static_cast<int>(serato::ReadStatus::cancelled));
                expect(serato::serializeTag(result).getSize() == 0);
            }
        }

        beginTest(
            "Legacy marker fallback, unrelated frame count and repeated "
            "requested kinds remain independent"
        );
        {
            const auto file = directory.getChildFile("fallback.wav");
            expect(createSyntheticContainer(file));
            expect(writeSyntheticTags(file, false, true, true));
            const auto markers = serato::readTag(file, serato::TagKind::markers);
            expectEquals(static_cast<int>(markers.status()), static_cast<int>(serato::ReadStatus::present));
            if (const auto value = tagValue<serato::Markers>(markers); value && !value->cues.empty()) {
                expectEquals(static_cast<int>(value->cues.front().positionMilliseconds), 100);
            }
            expectStatus(serato::readTag(file), serato::ReadStatus::present);
            const std::array kinds {
                serato::TagKind::analysis,
                std::bit_cast<serato::TagKind>(std::uint8_t {99}),
                serato::TagKind::analysis,
            };
            const auto results = serato::readTags(file, kinds);
            expectEquals(static_cast<int>(results.size()), 3);
            if (results.size() == 3) {
                expectEquals(static_cast<int>(results[0].status()), static_cast<int>(serato::ReadStatus::present));
                expectEquals(static_cast<int>(results[1].status()), static_cast<int>(serato::ReadStatus::unsupported));
                expectEquals(static_cast<int>(results[2].status()), static_cast<int>(serato::ReadStatus::present));
            }
            expect(serato::readTags(file, {}).empty());
        }

        beginTest(
            "Standard key survives invalid BPM and Analysis alone never "
            "supplies musical key"
        );
        {
            const auto file = directory.getChildFile("properties.wav");
            expect(createSyntheticContainer(file));
            expect(writeSyntheticTags(file));
            const auto& fullPath = file.getFullPathName();
            {
                TagLib::RIFF::WAV::File wav(utils::tagLibFileName(fullPath), false);
                auto properties = wav.properties();
                properties.replace("BPM", TagLib::StringList("nan"));
                wav.setProperties(properties);
                expect(wav.save());
            }
            const auto keyOnly = serato::readTag(file, serato::TagKind::trackInfo);
            expectEquals(static_cast<int>(keyOnly.status()), static_cast<int>(serato::ReadStatus::present));
            if (const auto info = tagValue<serato::TrackInfo>(keyOnly)) {
                expect(!info->bpm);
                expectEquals(info->key, juce::String("Gm"));
            }
            {
                TagLib::RIFF::WAV::File wav(utils::tagLibFileName(fullPath), false);
                auto properties = wav.properties();
                properties.erase("BPM");
                properties.erase("INITIALKEY");
                wav.setProperties(properties);
                expect(wav.save());
            }
            expectEquals(
                static_cast<int>(serato::readTag(file, serato::TagKind::analysis).status()),
                static_cast<int>(serato::ReadStatus::present)
            );
            expectEquals(
                static_cast<int>(serato::readTag(file, serato::TagKind::trackInfo).status()),
                static_cast<int>(serato::ReadStatus::absent)
            );
        }
    }

    /// Checks that track information also carries the album, genre, year, and comment of the standard tags.
    void testTrackInfoFields()
    {
        const TemporaryDirectory directory;
        beginTest("Track information carries the album, genre, year, and comment of the standard tags");
        {
            const auto tagged = tagValue<serato::TrackInfo>(
                serato::readTag(fixtureFile("tagged_stereo_44100_16.flac"), serato::TagKind::trackInfo)
            );
            expect(tagged.has_value());
            if (tagged) {
                expectEquals(tagged->album, juce::String("Fixtures"));
                expectEquals(tagged->comment, juce::String("Custom comment"));
            }

            const auto file = directory.getChildFile("dates.wav");
            expect(createSyntheticContainer(file));
            expect(writeSyntheticTags(file));
            {
                TagLib::RIFF::WAV::File wav(utils::tagLibFileName(file.getFullPathName()), false);
                auto properties = wav.properties();
                properties.replace("DATE", TagLib::StringList("2024-05-01"));
                properties.replace("GENRE", TagLib::StringList("Techno"));
                wav.setProperties(properties);
                expect(wav.save());
            }
            const auto dated = tagValue<serato::TrackInfo>(serato::readTag(file, serato::TagKind::trackInfo));
            expect(dated.has_value());
            if (dated) {
                expectEquals(dated->year, juce::String("2024"));
                expectEquals(dated->genre, juce::String("Techno"));
            }
        }
    }

    /// Checks both the outcome and that a present result owns an Overview.
    void expectStatus(const serato::TagReadResult& result, serato::ReadStatus status)
    {
        expectEquals(static_cast<int>(result.status()), static_cast<int>(status));
        expect(tagValue<serato::Overview>(result).has_value() == (status == serato::ReadStatus::present));
    }
};

std::unique_ptr<juce::UnitTest> createSeratoTagReaderTests()
{
    return std::make_unique<SeratoTagReaderTests>();
}
}  // namespace audiobatch::tests
