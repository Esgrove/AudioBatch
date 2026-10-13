/// Implementation of the source bit depth detection.
/// Reads the bit depth from the reader for lossless formats.
/// MP3 files have no source bit depth, since the format stores no sample depth
/// and the decoder output says nothing about the original encoding,
/// so they get a fixed default instead of a value read from the tags.

#include "BitDepthDetection.h"

#include "FileUtils.h"

namespace audiobatch::analysis
{
/// Bit depth assumed for MP3 files.
constexpr int defaultMp3BitsPerSample = 16;

/// Returns true when the reader reports the decoder output bit depth instead of the source bit depth.
/// JUCE's MP3 reader is the known case, since decoding hides the original encoding depth.
static bool reportsDecodedBitDepth(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (utils::isMp3File(file)) {
        return true;
    }

    return reader.getFormatName().containsIgnoreCase("mp3");
}

/// Determines the best available source bit depth for the analyzed file.
///
/// Lossless formats report the depth stored in the file.
/// MP3 files use the default of 16,
/// because the format stores no sample depth and the decoder does not expose the original encoding depth.
int resolveSourceBitsPerSample(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (reportsDecodedBitDepth(reader, file)) {
        return defaultMp3BitsPerSample;
    }

    return static_cast<int>(reader.bitsPerSample);
}
}  // namespace audiobatch::analysis
