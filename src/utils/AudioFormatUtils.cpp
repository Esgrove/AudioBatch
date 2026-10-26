/// Implementation of the shared audio format helpers.

#include "utils/AudioFormatUtils.h"

#include "utils/AudioMetadata.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

#include <memory>
#include <mpegfile.h>
#include <mpegheader.h>
#include <ranges>
#include <span>
#include <utility>

namespace audiobatch::utils
{
namespace
{
/// Samples of layer III synthesis delay that both JUCE's MP3 reader and Core Audio leave out at the start of a file.
constexpr juce::int64 mp3DecoderDelaySamples {529};
}  // namespace

/// Registers the formats of juce::AudioFormatManager::registerBasicFormats,
/// but with JUCE's MP3 reader ahead of Core Audio, since the manager opens a file with the first format that reads it.
/// JUCE's reader drops the encoder delay recorded in the LAME, Lavf, or Lavc tag of an MP3 file,
/// while Core Audio keeps it for files tagged by FFmpeg encoders,
/// which would move the audio later than the Serato markers of the file.
/// Core Audio still opens an MP3 file that JUCE's reader rejects.
void registerAudioFormats(juce::AudioFormatManager& formatManager)
{
    formatManager.registerFormat(new juce::WavAudioFormat(), true);
    formatManager.registerFormat(new juce::AiffAudioFormat(), false);
#if JUCE_USE_FLAC
    formatManager.registerFormat(new juce::FlacAudioFormat(), false);
#endif
#if JUCE_USE_OGGVORBIS
    formatManager.registerFormat(new juce::OggVorbisAudioFormat(), false);
#endif
#if JUCE_USE_OPUS
    formatManager.registerFormat(new juce::OpusAudioFormat(), false);
#endif
#if JUCE_USE_MP3AUDIOFORMAT
    formatManager.registerFormat(new juce::MP3AudioFormat(), false);
#endif
#if JUCE_MAC || JUCE_IOS
    formatManager.registerFormat(new juce::CoreAudioFormat(), false);
#endif
#if JUCE_USE_WINDOWS_MEDIA_FORMAT
    formatManager.registerFormat(new juce::WindowsMediaAudioFormat(), false);
#endif
}

/// Returns a format manager private to the calling thread, with the formats of registerAudioFormats.
/// Thread-local storage lets worker threads read files concurrently without sharing a manager or locking.
juce::AudioFormatManager& threadLocalFormatManager()
{
    thread_local juce::AudioFormatManager formatManager;
    thread_local const bool initialized = [] {
        registerAudioFormats(formatManager);
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

/// Opens an audio reader and corrects the estimated length of MPEG-1 Layer III files without a VBR header.
/// JUCE estimates their frame count using one frame's byte size, which ignores varying frame padding and trailing tags.
/// Counting complete, contiguous frames avoids decoding past the audio during small-block plugin processing.
/// The decoder leaves out its synthesis delay at the start, so that many fewer samples than the frames hold are read.
/// Files with a frame-count header retain the decoder's length, as do damaged or unsupported MPEG streams.
std::unique_ptr<juce::AudioFormatReader> createAudioReader(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(threadLocalFormatManager().createReaderFor(file));

    if (reader == nullptr || !isMp3File(file)) {
        return reader;
    }

    const auto& fullPath = file.getFullPathName();
    TagLib::MPEG::File mpegFile(tagLibFileName(fullPath));
    const auto* properties = mpegFile.audioProperties();

    if (!mpegFile.isValid() || properties == nullptr || properties->xingHeader() != nullptr
        || properties->version() != TagLib::MPEG::Header::Version1 || properties->layer() != 3)
    {
        return reader;
    }

    const auto firstFrame = mpegFile.firstFrameOffset();
    const auto lastFrame = mpegFile.lastFrameOffset();

    if (firstFrame < 0 || lastFrame < firstFrame) {
        return reader;
    }

    juce::int64 sampleCount = 0;

    for (auto position = firstFrame; position <= lastFrame;) {
        const TagLib::MPEG::Header header(&mpegFile, position, false);

        if (!header.isValid() || header.version() != TagLib::MPEG::Header::Version1 || header.layer() != 3
            || header.sampleRate() != properties->sampleRate() || header.frameLength() <= 0
            || position + header.frameLength() > mpegFile.length())
        {
            return reader;
        }

        sampleCount += header.samplesPerFrame();
        position += header.frameLength();
    }

    reader->lengthInSamples = juce::jmax<juce::int64>(0, sampleCount - mp3DecoderDelaySamples);
    return reader;
}

/// Reads back a freshly written audio file and checks that it has the expected channel count,
/// sample rate, and length, which also catches a short write the writer could not report.
/// Returns a user-facing description of the problem when it does not match.
/// The services run this before the source is replaced or moved to the trash.
std::expected<void, juce::String> validateWrittenAudio(
    juce::AudioFormatManager& formatManager,
    const juce::File& file,
    const int expectedChannels,
    const double expectedSampleRate,
    const juce::int64 expectedLength
)
{
    const std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr) {
        return std::unexpected(juce::String("the output file cannot be read"));
    }

    if (std::cmp_not_equal(reader->numChannels, expectedChannels)) {
        return std::unexpected(
            format("the output has {} channels instead of {}", reader->numChannels, expectedChannels)
        );
    }

    if (!juce::approximatelyEqual(reader->sampleRate, expectedSampleRate)) {
        return std::unexpected(
            format("the output sample rate is {} Hz instead of {} Hz", reader->sampleRate, expectedSampleRate)
        );
    }

    if (reader->lengthInSamples != expectedLength) {
        return std::unexpected(
            format("the output has {} samples instead of {}", reader->lengthInSamples, expectedLength)
        );
    }

    return {};
}

/// Copies JUCE reader metadata into the map type expected by
/// juce::AudioFormatWriterOptions for the AIFF writer, so written files keep
/// the markers and loop points JUCE read from an AIFF source. Metadata from
/// other readers, such as the bext, cue, and smpl chunks of a WAV file, is
/// dropped, because the AIFF writer would build its chunks from keys meant for
/// another format and asserts on WAV metadata. Tags and pictures are copied
/// separately through AudioMetadata.h.
std::unordered_map<juce::String, juce::String> aiffWriterMetadata(const juce::StringPairArray& metadata)
{
    std::unordered_map<juce::String, juce::String> copiedMetadata;

    // JUCE's AIFF reader marks its metadata with this source key,
    // and other readers either set their own or none.
    if (metadata.getValue("MetaDataSource", {}) != "AIFF") {
        return copiedMetadata;
    }

    for (const auto& [key, value] : std::views::zip(metadata.getAllKeys(), metadata.getAllValues())) {
        copiedMetadata.emplace(key, value);
    }

    return copiedMetadata;
}

/// Reads `numSamples` frames starting at `startSample` into the start of each channel as floats,
/// and returns false when the reader reports a failure.
/// JUCE converts the samples of a fixed-point reader to floats only
/// after a successful read, so after a failed read the block would hold raw integers reinterpreted as floats,
/// which play as full-scale noise. The block is zeroed in that case.
/// A floating point reader keeps the samples it decoded before the error.
bool readFloatSamples(
    juce::AudioFormatReader& reader,
    const std::span<float* const> channels,
    const juce::int64 startSample,
    const int numSamples
)
{
    if (reader.read(channels.data(), static_cast<int>(channels.size()), startSample, numSamples)) {
        return true;
    }

    if (!reader.usesFloatingPointData) {
        for (auto* const channel : channels) {
            juce::FloatVectorOperations::clear(channel, numSamples);
        }
    }

    return false;
}

/// Returns the write pointers of every channel of the buffer as a span,
/// so the channel count travels with the pointers.
std::span<float* const> writePointers(juce::AudioBuffer<float>& buffer)
{
    return {buffer.getArrayOfWritePointers(), static_cast<std::size_t>(buffer.getNumChannels())};
}
}  // namespace audiobatch::utils
