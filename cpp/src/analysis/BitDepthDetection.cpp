/// Implementation of the source bit depth detection.
/// Reads the bit depth from the reader for lossless formats.
/// MP3 and AAC files have no source bit depth, since the formats store no sample depth
/// and the decoder output says nothing about the original encoding,
/// so they get a fixed default instead of a value read from the tags.
/// ALAC files in MP4 get the depth stored in their sample description,
/// since the platform decoders reading them do not expose it.

#include "analysis/BitDepthDetection.h"

#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"

#include <audioproperties.h>
#include <mp4file.h>
#include <mp4properties.h>

namespace audiobatch::analysis
{
namespace
{
/// Bit depth assumed for lossy files, which is also the depth their normalized output is written in.
constexpr int defaultLossyBitsPerSample {16};

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
bool isPlatformDecodedMp4(const juce::AudioFormatReader& reader, const juce::File& file)
{
    const auto& name = reader.getFormatName();
    return file.hasFileExtension("m4a;m4b;mp4;aac")
        && (name.containsIgnoreCase("CoreAudio") || name.containsIgnoreCase("Windows Media"));
}

/// Returns the depth TagLib reads from the ALAC sample description of an MP4 file.
/// AAC, which has no source depth, and files TagLib cannot read get the lossy default.
int resolveMp4BitsPerSample(const juce::File& file)
{
    const TagLib::MP4::File mp4File(utils::tagLibFileName(file.getFullPathName()), true, TagLib::AudioProperties::Fast);
    const auto* properties = mp4File.isValid() ? mp4File.audioProperties() : nullptr;
    if (properties != nullptr && properties->codec() == TagLib::MP4::Properties::ALAC
        && properties->bitsPerSample() > 0)
    {
        return properties->bitsPerSample();
    }

    return defaultLossyBitsPerSample;
}
}  // namespace

/// Determines the best available source bit depth for the analyzed file.
///
/// Lossless formats report the depth stored in the file.
/// MP3 and AAC files use the default of 16,
/// because the formats store no sample depth and the decoder does not expose the original encoding depth.
/// ALAC files read through a platform decoder report the depth in their sample description,
/// which both applications report, since the parity script compares the depth.
int resolveSourceBitsPerSample(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (reportsDecodedBitDepth(reader, file)) {
        return defaultLossyBitsPerSample;
    }
    if (isPlatformDecodedMp4(reader, file)) {
        return resolveMp4BitsPerSample(file);
    }

    return static_cast<int>(reader.bitsPerSample);
}
}  // namespace audiobatch::analysis
