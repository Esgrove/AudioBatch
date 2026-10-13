/// Source bit depth detection for analysis.
/// Declares resolveSourceBitsPerSample, which reports the bit depth of the original encoding
/// as read from the audio file itself, never from its metadata tags.

#pragma once

#include <JuceHeader.h>

namespace audiobatch::analysis
{
int resolveSourceBitsPerSample(const juce::AudioFormatReader& reader, const juce::File& file);
}
