/// Source bit depth detection for analysis.
/// Declares resolveSourceBitsPerSample, which reports the bit depth of the original encoding,
/// including the depth stored in the tags of MP3 files decoded from lossless sources.

#pragma once

#include <JuceHeader.h>

namespace audiobatch::analysis
{
/// Determines the best available source bit depth for the analyzed file.
/// MP3 files fall back to a tagged value or the default of 16,
/// because the decoder does not expose the original encoding depth.
int resolveSourceBitsPerSample(const juce::AudioFormatReader& reader, const juce::File& file);
}  // namespace audiobatch::analysis
