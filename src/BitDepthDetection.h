/// Source bit depth detection for analysis.
/// Declares resolveSourceBitsPerSample, which reports the bit depth of the original encoding
/// as read from the audio file itself, never from its metadata tags.

#pragma once

#include <JuceHeader.h>

namespace audiobatch::analysis
{
/// Determines the best available source bit depth for the analyzed file.
///
/// Lossless formats report the depth stored in the file.
/// MP3 files use the default of 16,
/// because the format stores no sample depth and the decoder does not expose the original encoding depth.
int resolveSourceBitsPerSample(const juce::AudioFormatReader& reader, const juce::File& file);
}  // namespace audiobatch::analysis
