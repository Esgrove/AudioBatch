/// Implementation of the shared audio format helpers.

#include "AudioFormatUtils.h"

#include "StringFormat.h"

#include <memory>
#include <span>
#include <utility>

namespace audiobatch::utils
{
/// Returns a format manager private to the calling thread, with the basic formats registered.
/// Thread-local storage lets worker threads read files concurrently without sharing a manager or locking.
juce::AudioFormatManager& threadLocalFormatManager()
{
    thread_local juce::AudioFormatManager formatManager;
    thread_local const bool initialized = [] {
        formatManager.registerBasicFormats();
        return true;
    }();

    juce::ignoreUnused(initialized);
    return formatManager;
}

/// Returns the AIFF format registered in the manager, or nullptr when this build has none.
juce::AudioFormat* findAiffFormat(const juce::AudioFormatManager& formatManager)
{
    if (auto* format = formatManager.findFormatForFileExtension("aif"); format != nullptr) {
        return format;
    }

    return formatManager.findFormatForFileExtension("aiff");
}

/// Reads `numSamples` frames starting at `startSample` into the start of each channel as floats,
/// and returns false when the reader reports a failure.
/// JUCE converts the samples of a fixed-point reader to floats only after a successful read,
/// so after a failed read the block would hold raw integers reinterpreted as floats, which play as full-scale noise.
/// The block is zeroed in that case.
/// A floating point reader keeps the samples it decoded before the error.
bool readFloatSamples(
    juce::AudioFormatReader& reader,
    float* const* channels,
    const int numChannels,
    const juce::int64 startSample,
    const int numSamples
)
{
    if (reader.read(channels, numChannels, startSample, numSamples)) {
        return true;
    }

    if (!reader.usesFloatingPointData) {
        for (auto* const channel : std::span(channels, static_cast<std::size_t>(numChannels))) {
            juce::FloatVectorOperations::clear(channel, numSamples);
        }
    }

    return false;
}

/// Reads back a freshly written audio file and checks that it has the expected channel count, sample rate,
/// and length, which also catches a short write the writer could not report.
/// Returns an empty string when it matches, or a user-facing description of the problem.
/// The services run this before the source is replaced or moved to the trash.
juce::String validateWrittenAudio(
    juce::AudioFormatManager& formatManager,
    const juce::File& file,
    const int expectedChannels,
    const double expectedSampleRate,
    const juce::int64 expectedLength
)
{
    const std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr) {
        return "the output file cannot be read";
    }

    if (std::cmp_not_equal(reader->numChannels, expectedChannels)) {
        return format("the output has {} channels instead of {}", reader->numChannels, expectedChannels);
    }

    if (!juce::approximatelyEqual(reader->sampleRate, expectedSampleRate)) {
        return format("the output sample rate is {} Hz instead of {} Hz", reader->sampleRate, expectedSampleRate);
    }

    if (reader->lengthInSamples != expectedLength) {
        return format("the output has {} samples instead of {}", reader->lengthInSamples, expectedLength);
    }

    return {};
}

/// Copies JUCE reader metadata into the map type expected by juce::AudioFormatWriterOptions for the AIFF writer,
/// so written files keep the markers and loop points JUCE read from an AIFF source.
/// Metadata from other readers, such as the bext, cue, and smpl chunks of a WAV file, is dropped,
/// because the AIFF writer would build its chunks from keys meant for another format and asserts on WAV metadata.
/// Tags and pictures are copied separately through MetadataService.
std::unordered_map<juce::String, juce::String> aiffWriterMetadata(const juce::StringPairArray& metadata)
{
    std::unordered_map<juce::String, juce::String> copiedMetadata;

    // JUCE's AIFF reader marks its metadata with this source key, and other readers either set their own or none.
    if (metadata.getValue("MetaDataSource", {}) != "AIFF") {
        return copiedMetadata;
    }

    const auto& keys = metadata.getAllKeys();
    const auto& values = metadata.getAllValues();

    for (int index = 0; index < keys.size(); ++index) {
        copiedMetadata.emplace(keys[index], values[index]);
    }

    return copiedMetadata;
}
}  // namespace audiobatch::utils