/// Opt-in, device-free measurements of spectrogram construction, artifact backfill, codecs, and cache access.
/// The test executable calls this hook before GoogleTest argument parsing.

#pragma once

#include <JuceHeader.h>

namespace audiobatch::tests
{
int runSpectrogramBenchmark(const juce::StringArray& arguments);
}
