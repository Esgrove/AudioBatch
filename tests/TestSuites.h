/// Factory functions for the AudioBatch unit test suites.
/// Each suite lives in its own source file and is created explicitly by TestMain.cpp,
/// instead of registering itself through a static instance, which would need a global constructor.

#pragma once

#include <JuceHeader.h>

#include <memory>

namespace audiobatch::tests
{
/// Creates the suite that analyzes generated audio files and checks the measured levels.
std::unique_ptr<juce::UnitTest> createAnalysisTests();

/// Creates the suite that checks cache storage and invalidation against a temporary database.
std::unique_ptr<juce::UnitTest> createCacheTests();

/// Creates the suite that checks CLI argument parsing and validation.
std::unique_ptr<juce::UnitTest> createCliTests();

/// Creates the suite that checks the peak, loudness, and file property display formatting.
std::unique_ptr<juce::UnitTest> createFormatTests();

/// Creates the suite that checks reading and writing tags across formats.
std::unique_ptr<juce::UnitTest> createMetadataTests();

/// Creates the suite that runs files through in-process test plugins.
std::unique_ptr<juce::UnitTest> createProcessingTests();

/// Creates the suite that checks normalization output paths, collisions, and the written audio.
std::unique_ptr<juce::UnitTest> createNormalizationTests();
/// Creates the suite that checks the results table column comparators and sort order.
std::unique_ptr<juce::UnitTest> createSortingTests();

/// Creates the suite that checks the shared file, format, string, and cancellation helpers.
std::unique_ptr<juce::UnitTest> createUtilsTests();
}  // namespace audiobatch::tests
