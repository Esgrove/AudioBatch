/// Progress state that AudioBatchComponent keeps for its background operations.
/// Declares AnalysisRunState for the current analysis run
/// and OperationProgress for the normalization and plugin processing passes.

#pragma once

#include "analysis/AudioAnalysisTypes.h"

#include <JuceHeader.h>

#include <set>

namespace audiobatch
{
/// Persisted preview choices, independent of the current file and audio device.
struct AudioPreviewPreferences {
    bool pluginChainEnabled = false;
    bool latencyCompensated = true;
    bool continuePlayback = false;
    bool seratoWaveformColours = true;
};

/// Progress and inputs of one analysis run, from its start until its completion arrives.
struct AnalysisRunState {
    /// Options of the run.
    /// A run that keeps the results and replaces a running one carries these inputs over.
    AudioAnalysisOptions options;

    /// Full paths of the rows removed while the run was in progress.
    /// Their results are ignored, so a running or carried over run does not add the rows back.
    std::set<juce::String> removedPaths;

    /// Time the run started, in milliseconds from juce::Time::getMillisecondCounterHiRes.
    double startedAtMs = 0.0;

    /// Id of the run, used to drop queued messages from runs that were replaced.
    int runId = -1;

    /// Number of files the plan sent to analysis instead of loading from the cache.
    int analyzedFiles = 0;

    /// Results received so far, out of the expected count, which only drive the status text.
    int completed = 0;
    int expected = 0;

    /// True from the start of the run until its completion arrives.
    /// Kept separate from the progress counters.
    bool running = false;

    /// True once the run's plan arrived, so the status shows progress instead of the folder scan.
    bool planReceived = false;

    /// True when the run replaces the results list, which picks the message shown when no files are found.
    bool replacesResults = false;
};

/// Progress and collected failures of a normalization or plugin processing pass.
struct OperationProgress {
    /// One "file: error" line per failed file, shown in a summary when the pass completes.
    juce::StringArray failures;

    /// Time the pass started, in milliseconds from juce::Time::getMillisecondCounterHiRes.
    double startedAtMilliseconds = 0.0;

    int completed = 0;
    int expected = 0;
    bool inProgress = false;
};
}  // namespace audiobatch
