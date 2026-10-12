/// Portable Serato container tests write synthetic tags only to temporary fixture copies.
/// Reader coverage depends only on committed fixtures and generated payloads.

#include "SeratoTagReader.h"
#include "StringFormat.h"
#include "TagLibUtils.h"
#include "TestHelpers.h"
#include "TestSuites.h"

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

/// Writes existing ID3v2 containers on temporary copies, including an unrelated Unicode description.
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
    std::uint64_t checksum = 14695981039346656037ULL;
    std::array<std::uint8_t, 8192> buffer {};
    while (!input->isExhausted()) {
        const auto count = input->read(buffer.data(), static_cast<int>(buffer.size()));
        if (count <= 0) {
            return std::nullopt;
        }
        for (const auto byte : std::span(buffer).first(static_cast<std::size_t>(count))) {
            checksum ^= byte;
            checksum *= 1099511628211ULL;
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
    SeratoTagReaderTests() : juce::UnitTest("Serato tag reader", "AudioBatch") { }

    /// Writes synthetic metadata only on temporary copies and verifies extraction is read-only.
    void runTest() override
    {
        const auto payload = readerPayload();
        const auto expected = serato::Overview::parse(payload);
        const TemporaryDirectory directory;

        beginTest("GEOB extraction supports MP3, AIFF, WAV, ID3v2.3/v2.4 and text encodings");
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
                    if (result.overview && expected) {
                        expect(result.overview->slices == expected->slices);
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

        beginTest("FLAC comments support padding, whitespace, omitted padding and valid later duplicates");
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
            if (result.overview && expected) {
                expect(result.overview->slices == expected->slices);
            }
            expect(before.has_value() && before == fileChecksum(flacFile));
        }

        beginTest("FLAC rejects missing wrapper fields, wrong descriptions, invalid padding and oversized values");
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
            invalidValues.push_back(juce::String::repeatedString("A", 1500000));
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

private:
    /// Checks both the outcome and the invariant that only present results own an Overview.
    void expectStatus(const serato::TagReadResult& result, serato::ReadStatus status)
    {
        expectEquals(static_cast<int>(result.status), static_cast<int>(status));
        expect(result.overview.has_value() == (status == serato::ReadStatus::present));
    }
};

std::unique_ptr<juce::UnitTest> createSeratoTagReaderTests()
{
    return std::make_unique<SeratoTagReaderTests>();
}
}  // namespace audiobatch::tests