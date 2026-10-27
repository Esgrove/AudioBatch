/// Implementation of the source bit depth detection.
/// Reads the bit depth from the reader for lossless formats.
/// MP3 files have no source bit depth, since the format stores no sample depth
/// and the decoder output says nothing about the original encoding,
/// so they get a fixed default instead of a value read from the tags.
/// AAC and ALAC files in MP4 report no bit depth, since the platform decoders reading them do not expose it.

#include "analysis/BitDepthDetection.h"

#include "utils/FileUtils.h"

namespace audiobatch::analysis
{
namespace
{
/// Bit depth assumed for MP3 files.
constexpr int defaultMp3BitsPerSample {16};

/// Returns true when the reader reports the decoder output bit depth instead of the source bit depth.
/// JUCE's MP3 reader is the known case, since decoding hides the original encoding depth.
bool reportsDecodedBitDepth(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (utils::isMp3File(file)) {
        return true;
    }

    return reader.getFormatName().containsIgnoreCase("mp3");
}

/// Returns true for MP4 audio read through a platform decoder, CoreAudio or Windows Media,
/// which reports the width of its own output rather than a depth stored in the file.
/// AAC has no source depth, and the decoders do not expose the depth of ALAC.
bool isPlatformDecodedMp4(const juce::AudioFormatReader& reader, const juce::File& file)
{
    const auto& name = reader.getFormatName();
    return file.hasFileExtension("m4a;m4b;mp4;aac")
        && (name.containsIgnoreCase("CoreAudio") || name.containsIgnoreCase("Windows Media"));
}
}  // namespace

/// Determines the best available source bit depth for the analyzed file.
///
/// Lossless formats report the depth stored in the file.
/// MP3 files use the default of 16,
/// because the format stores no sample depth and the decoder does not expose the original encoding depth.
/// MP4 files read through a platform decoder report 0, as the Rust application does for them.
int resolveSourceBitsPerSample(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (reportsDecodedBitDepth(reader, file)) {
        return defaultMp3BitsPerSample;
    }
    if (isPlatformDecodedMp4(reader, file)) {
        return 0;
    }

    return static_cast<int>(reader.bitsPerSample);
}
}  // namespace audiobatch::analysis
