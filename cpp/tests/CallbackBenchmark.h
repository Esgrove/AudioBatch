/// Hardware-free benchmarks of the complete production AudioPreviewPlayer callback.
/// Explicit timing runs are separate from the untimed smoke contract tests and never open hardware or user storage.

#pragma once

#include <JuceHeader.h>

namespace audiobatch::tests
{
int runCallbackBenchmark(const juce::StringArray& arguments);
void runCallbackBenchmarkContractTests(juce::UnitTest& test);
}  // namespace audiobatch::tests
