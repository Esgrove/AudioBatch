/// Preview preferences, the album artwork state,
/// and the state of the background work that AudioBatchComponent runs.
/// Declares BackgroundWork, the variant of the idle state, an analysis run, and a normalization or processing pass,
/// which never run at the same time.
/// Also declares AnalysisResultMessage for results waiting to be merged.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "utils/FileUtils.h"

#include <JuceHeader.h>

#include <optional>
#include <set>
#include <variant>

namespace audiobatch
{
/// Persisted preview choices, independent of the current file and audio device.
struct AudioPreviewPreferences {
    /// Lowest editable LUFS limit of the source loudness axis.
    static constexpr double minimumLoudnessGraphLimit {-60.0};
    /// Highest editable LUFS limit of the source loudness axis.
    static constexpr double maximumLoudnessGraphLimit {6.0};

    /// Initial bottom of the LUFS axis, shared by the settings and the waveform.
    static constexpr double defaultLoudnessGraphMinimum {-24.0};
    /// Initial top of the LUFS axis, shared by the settings and the waveform.
    static constexpr double defaultLoudnessGraphMaximum {-4.0};

    /// True to play the preview through the plugin chain.
    bool pluginChainEnabled {false};
    /// True to delay the preview by the chain latency, so the audio lines up with the waveform position.
    bool latencyCompensated {true};
    /// True to keep playing when another file is selected during playback.
    bool continuePlayback {false};
    /// True to start a newly selected file at its first Serato cue point.
    bool playFromFirstCuePoint {false};
    /// True to colour the waveform with the Serato overview of the file.
    bool seratoWaveformColours {true};
    /// True to draw the short-term loudness curve over the waveform.
    bool loudnessGraphEnabled {false};
    /// Bottom of the loudness axis in LUFS.
    double loudnessGraphMinimum {defaultLoudnessGraphMinimum};
    /// Top of the loudness axis in LUFS.
    double loudnessGraphMaximum {defaultLoudnessGraphMaximum};
    /// True to show the embedded album artwork beside the file name.
    /// Only the Options menu changes it, so Audio Settings carries it over unchanged.
    bool showAlbumArtwork {true};
};

/// The album artwork of the selected file, requested once per file version.
struct ArtworkState {
    /// The selected file whose artwork is shown or requested, tracked even while the option is off.
    juce::File file;
    /// The file version the artwork was requested for, so the panel refreshes reuse it instead of reading again.
    std::optional<utils::FileIdentity> requested;
    /// The decoded thumbnail, invalid while pending or when the file has no usable artwork.
    juce::Image image;
    /// True once the request for the requested version completed, including with no artwork.
    bool completed {false};
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

    /// Results received so far, which only drive the status text and the progress.
    int completed {0};
    /// Results the run expects, the number of files its plan found, lowered by rows removed during the run.
    int expected {0};

    /// True once the run's plan arrived, so the status shows progress instead of the folder scan.
    bool planReceived {false};

    /// True when the files of the run make up the whole results list,
    /// which drops the rows of other files when the plan arrives and picks the message shown when no files are found.
    bool replacesResults {false};
};

/// One analysis result waiting to be merged into the table, with the run it belongs to.
struct AnalysisResultMessage {
    /// The analysis result.
    AudioAnalysisRecord record;
    /// Id of the analysis run that produced it.
    int runId {0};
};

/// Progress and collected failures of a normalization or plugin processing pass.
struct OperationProgress {
    /// One "file: error" line per failed file, shown in a summary when the pass completes.
    juce::StringArray failures;

    /// Time the pass started, in milliseconds from juce::Time::getMillisecondCounterHiRes.
    double startedAtMilliseconds {0.0};

    /// Files whose result arrived so far.
    int completed {0};
    /// Files the pass started.
    int expected {0};

    /// Id of the coordinator run of the pass, which drops the queued messages of a pass that ended before it.
    int runId {-1};

    /// True once the user cancelled the pass, which then waits for its files in progress before it completes.
    bool cancelling {false};
};

/// A normalization pass, from its start until its completion arrives.
struct NormalizingWork : OperationProgress {};

/// A plugin processing pass, from its start until its completion arrives.
struct ProcessingWork : OperationProgress {};

/// The background work of the main view, which runs one analysis, normalization, or processing pass at a time.
using BackgroundWork = std::variant<IdleWork, AnalyzingWork, NormalizingWork, ProcessingWork>;
}  // namespace audiobatch
