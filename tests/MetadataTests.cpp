/// Tests for the metadata functions on the committed tagged fixture.
/// Covers reading Vorbis comments from FLAC and writing them to an AIFF copy as ID3v2.4.

#include "metadata/Metadata.h"
#include "TestHelpers.h"
#include "TestSuites.h"

namespace audiobatch::tests
{
/// Returns the first value of a property, or an empty string when the metadata does not have it.
static juce::String firstPropertyValue(const AudioMetadata& metadata, const juce::String& key)
{
    for (const auto& property : metadata.properties) {
        if (property.key == key && !property.values.isEmpty()) {
            return property.values[0];
        }
    }

    return {};
}

/// Checks that tags survive reading and writing across formats.
class MetadataTests final : public juce::UnitTest
{
public:
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

        beginTest("An untagged file has no metadata");
        {
            AudioMetadata untagged;
            readMetadata(fixtureFile("sine_stereo_44100_16.wav"), untagged);
            expect(untagged.isEmpty());
        }
    }
};

std::unique_ptr<juce::UnitTest> createMetadataTests()
{
    return std::make_unique<MetadataTests>();
}
}  // namespace audiobatch::tests
