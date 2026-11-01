/// Standalone in-memory VST3 benchmarking through the plugin-linked test executable.
/// The explicit benchmark mode does not run unit tests or modify application settings or source audio.

#pragma once

#include <JuceHeader.h>

namespace audiobatch::tests
{
int runPluginBenchmark(const juce::StringArray& arguments);
}
