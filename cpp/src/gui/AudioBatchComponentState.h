/// Preview preferences and the state of the background work that AudioBatchComponent runs.
/// Declares BackgroundWork, the variant of the idle state, an analysis run, and a normalization or processing pass,
/// which never run at the same time.
/// Also declares AnalysisResultMessage for results waiting to be merged.

#pragma once

#include "analysis/AudioAnalysisTypes.h"

#include <JuceHeader.h>

#include <set>
#include <variant>

namespace audiobatch
{
/// Persisted preview choices, independent of the current file and audio device.
struct AudioPreviewPreferences {
    /// Practical editable LUFS limits for the source loudness axis.
    static constexpr double minimumLoudnessGraphLimit {-60.0};
    static constexpr double maximumLoudnessGraphLimit {6.0};

    /// Initial LUFS axis range shared by settings and waveform presentation.
    static constexpr double defaultLoudnessGraphMinimum {-24.0};
    static constexpr double defaultLoudnessGraphMaximum {-4.0};

    bool pluginChainEnabled {false};
    bool latencyCompensated {true};
    bool continuePlayback {false};
    bool playFromFirstCuePoint {false};
    bool seratoWaveformColours {true};
    bool loudnessGraphEnabled {false};
    double loudnessGraphMinimum {defaultLoudnessGraphMinimum};
    double loudnessGraphMaximum {defaultLoudnessGraphMaximum};
};

/// What an analysis run does with the rows already in the table.
enum class AnalysisRows : std::uint8_t {
    /// Adds the files of the run to the rows and updates the rows of files it analyzes again.
    add,
    /// Clears the table first, for a run over new root folders.
    replace,
    /// Keeps the rows of the files the run finds and drops the others, for a rescan.
    sync,
};

/// No analysis, normalization, or plugin processing is running.
struct IdleWork {};

/// Progress and inputs of one analysis run, from its start until its completion arrives.
/// The run scans the input folders until its plan arrives.
struct AnalyzingWork {
    /// Options of the run.
    /// A run that keeps the results and replaces a running one carries these inputs over.
    AudioAnalysisOptions options;

    /// Full paths of the rows removed while the run was in progress.
    /// Their results are ignored, so a running or carried over run does not add the rows back.
    std::set<juce::String> removedPaths;

    /// Time the run started, in milliseconds from juce::Time::getMillisecondCounterHiRes.
    double startedAtMs {0.0};

    /// Id of the run, used to drop queued messages from runs that were replaced.
    int runId {-1};

    /// Number of files the plan sent to analysis instead of loading from the cache.
    int analyzedFiles {0};

    /// Results received so far, out of the expected count, which only drive the status text.
    int completed {0};
    int expected {0};

    /// True once the run's plan arrived, so the status shows progress instead of the folder scan.
    bool planReceived {false};

    /// True when the files of the run make up the whole results list,
    /// which drops the rows of other files when the plan arrives and picks the message shown when no files are found.
    bool replacesResults {false};
};

/// One analysis result waiting to be merged into the table, with the run it belongs to.
struct AnalysisResultMessage {
    AudioAnalysisRecord record;
    int runId {0};
};

/// Progress and collected failures of a normalization or plugin processing pass.
struct OperationProgress {
    /// One "file: error" line per failed file, shown in a summary when the pass completes.
    juce::StringArray failures;

    /// Time the pass started, in milliseconds from juce::Time::getMillisecondCounterHiRes.
    double startedAtMilliseconds {0.0};

    int completed {0};
    int expected {0};

    /// Id of the coordinator run of the pass, which drops the queued messages of a pass that ended before it.
    int runId {-1};
};

/// A normalization pass, from its start until its completion arrives.
struct NormalizingWork : OperationProgress {};

/// A plugin processing pass, from its start until its completion arrives.
struct ProcessingWork : OperationProgress {};

/// The background work of the main view, which runs one analysis, normalization, or processing pass at a time.
using BackgroundWork = std::variant<IdleWork, AnalyzingWork, NormalizingWork, ProcessingWork>;
}  // namespace audiobatch
