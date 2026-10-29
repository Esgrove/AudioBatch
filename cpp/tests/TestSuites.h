/// Factory functions for the AudioBatch unit test suites.
/// Each suite lives in its own source file and is created explicitly by TestMain.cpp,
/// instead of registering itself through a static instance, which would need a global constructor.

#pragma once

#include <JuceHeader.h>

#include <memory>

namespace audiobatch::tests
{
/// Creates the suite that analyzes the committed audio fixtures and checks the measured levels.
std::unique_ptr<juce::UnitTest> createAnalysisTests();

/// Creates the suite that checks the plans, results, and replaced runs of the analysis coordinator.
std::unique_ptr<juce::UnitTest> createAnalysisCoordinatorTests();

/// Creates the suite that checks the blocked reasons and progress of the background work of the main view.
std::unique_ptr<juce::UnitTest> createBackgroundWorkTests();

/// Creates the suite that checks cache storage and invalidation against a temporary database.
std::unique_ptr<juce::UnitTest> createCacheTests();

/// Creates the suite that checks CLI argument parsing and validation.
std::unique_ptr<juce::UnitTest> createCliTests();

/// Creates the suite that checks the per-file activity labels and operation messages of the results table.
std::unique_ptr<juce::UnitTest> createFileActivityTests();

/// Creates the suite that checks the peak, loudness, and file property display formatting.
std::unique_ptr<juce::UnitTest> createFormatTests();

std::unique_ptr<juce::UnitTest> createGuiFontTests();

/// Creates the suite that checks reading and writing tags across formats.
std::unique_ptr<juce::UnitTest> createMetadataTests();

/// Creates the suite that saves and loads chain presets,
/// and null tests the output of the repository's test VST3 against the committed expected files.
std::unique_ptr<juce::UnitTest> createPluginPresetTests();

/// Creates the suite that checks the queuing, cancellation, and stop requests of plugin processing runs,
/// and the creation of their plugin chains.
std::unique_ptr<juce::UnitTest> createPluginCoordinatorTests();

/// Creates the suite that checks the plugin scan lock, the known-plugins list file, and the CLI plugin listing.
std::unique_ptr<juce::UnitTest> createPluginScanTests();

/// Creates the suite that runs files through in-process test plugins.
std::unique_ptr<juce::UnitTest> createProcessingTests();

/// Creates the suite that checks the live preview renderer and the latency of the preview chain.
std::unique_ptr<juce::UnitTest> createPreviewPlaybackTests();

/// Creates the suite that checks the preview player, its simulated output device, and the preview preferences.
std::unique_ptr<juce::UnitTest> createPreviewPlayerTests();

/// Creates the suite that checks normalization output paths, collisions, and the written audio.
std::unique_ptr<juce::UnitTest> createNormalizationTests();

/// Creates the suite that checks the files a normalization run queues, what it publishes, and its cancellation.
std::unique_ptr<juce::UnitTest> createNormalizeCoordinatorTests();

/// Creates the suite that checks the source overload history, its blob and cache storage, and its projection.
std::unique_ptr<juce::UnitTest> createOverloadHistoryTests();

/// Creates the suite that checks the transitions between an empty, released, and loaded preview.
std::unique_ptr<juce::UnitTest> createPreviewControllerTests();

/// Creates the suite that checks merging, removing, and sorting the rows of the results table.
std::unique_ptr<juce::UnitTest> createResultsModelTests();

/// Creates the suite that checks the results table column comparators and sort order.
std::unique_ptr<juce::UnitTest> createSortingTests();

/// Creates the suite that checks bounded Serato Overview parsing and numeric serialization.
std::unique_ptr<juce::UnitTest> createSeratoOverviewTests();

/// Creates the suite that checks read-only Serato GEOB and FLAC extraction on temporary copies.
std::unique_ptr<juce::UnitTest> createSeratoTagReaderTests();

/// Creates the suite that checks synthetic metadata payloads and bounded per-kind cache codecs.
std::unique_ptr<juce::UnitTest> createSeratoTagsTests();

/// Creates the suite that checks bounded preview loading, stale delivery rejection and cache restart.
std::unique_ptr<juce::UnitTest> createSeratoPreviewLoaderTests();

/// Creates the suite that checks cue arrows, selection resets and musical metadata in the GUI preview.
std::unique_ptr<juce::UnitTest> createSeratoPreviewTests();

/// Creates the suite that checks the shared file, format, string, cancellation, and decode failure helpers.
std::unique_ptr<juce::UnitTest> createUtilsTests();
}  // namespace audiobatch::tests
