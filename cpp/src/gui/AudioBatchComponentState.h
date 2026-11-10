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
/// What an analysis run does with the rows already in the table.
enum class AnalysisRows : std::uint8_t {
    /// Adds the files of the run to the rows and updates the rows of files it analyzes again.
    add,
    /// Clears the table first, for a run over new root folders.
    replace,
    /// Keeps the rows of the files the run finds and drops the others, for a rescan.
    sync,
};

/// Source visualization and arrangement shown in the preview.
enum class PreviewViewMode : std::uint8_t { waveform, spectrogram, sideBySide, stacked };

[[nodiscard]] juce::String previewViewModeLabel(PreviewViewMode mode);
[[nodiscard]] PreviewViewMode previewViewModeFromName(const juce::String& name);
[[nodiscard]] juce::String previewViewModeName(PreviewViewMode mode);

/// Persisted preview choices, independent of the current file and audio device.
struct AudioPreviewPreferences {
    /// Initial top of the LUFS axis, shared by the settings and the waveform.
    static constexpr double DEFAULT_LOUDNESS_GRAPH_MAXIMUM {-4.0};
    /// Initial bottom of the LUFS axis, shared by the settings and the waveform.
    static constexpr double DEFAULT_LOUDNESS_GRAPH_MINIMUM {-24.0};
    /// Highest editable LUFS limit of the source loudness axis.
    static constexpr double MAXIMUM_LOUDNESS_GRAPH_LIMIT {6.0};
    /// Lowest editable LUFS limit of the source loudness axis.
    static constexpr double MINIMUM_LOUDNESS_GRAPH_LIMIT {-60.0};

    // Preserve the designated preference construction order shared by settings and integration tests.
    /// True to play the preview through the plugin chain.
    bool pluginChainEnabled {false};
    /// True to discard the chain's latency pre-roll, so audible audio lines up with the waveform position.
    bool latencyCompensated {true};
    /// True to keep playing when another file is selected during playback, and to play the next file when one ends.
    bool continuePlayback {false};
    /// True to start a newly selected file at its first Serato cue point.
    bool playFromFirstCuePoint {false};
    /// True to colour the waveform with the Serato overview of the file.
    bool seratoWaveformColours {true};
    /// True to draw source short-term loudness over the waveform, or over a full spectrogram.
    bool loudnessGraphEnabled {false};

    /// Bottom of the loudness axis in LUFS.
    double loudnessGraphMinimum {DEFAULT_LOUDNESS_GRAPH_MINIMUM};
    /// Top of the loudness axis in LUFS.
    double loudnessGraphMaximum {DEFAULT_LOUDNESS_GRAPH_MAXIMUM};

    /// True to show the embedded album artwork beside the file name.
    /// Only the Options menu changes it, so Audio Settings carries it over unchanged.
    bool showAlbumArtwork {true};

    /// Source visualization and arrangement, shared by Audio Settings and the Options menu.
    PreviewViewMode viewMode {PreviewViewMode::waveform};

    /// True to show source sample-peak overload indicators.
    bool showSampleOverloads {true};
    /// True to show source true-peak overload indicators independently of sample peaks.
    bool showTruePeakOverloads {true};
    /// True to extend enabled overload indicators as vertical lines across the preview.
    bool showOverloadLines {true};

    /// True to show predicted peaks and estimated loudness after custom gain in the file metrics.
    bool showGainAdjustedMetrics {true};
};

/// The album artwork of the selected file, requested once per file version.
struct ArtworkState {
    /// Reuses both a pending request and a completed no-art result until the selected file changes identity.
    [[nodiscard]] bool needsRequest() const
    {
        return !requested.has_value() || !requested->matches(file);
    }

    /// Drops the image and the no-art cache without forgetting the selected file.
    void invalidate()
    {
        requested.reset();
        image = {};
        completed = false;
    }

    /// The selected file whose artwork is shown or requested, tracked even while the option is off.
    juce::File file;
    /// The decoded thumbnail, invalid while pending or when the file has no usable artwork.
    juce::Image image;
    /// True once the request for the requested version completed, including with no artwork.
    bool completed {false};
    /// The file version the artwork was requested for, so the panel refreshes reuse it instead of reading again.
    std::optional<utils::FileIdentity> requested;
};

/// No analysis, normalization, or plugin processing is running.
struct IdleWork {};

/// Progress and inputs of one analysis run, from its start until its completion arrives.
/// The run scans the input folders until its plan arrives.
struct AnalyzingWork {
    // Preserve designated construction order for run setup and carried-over analysis inputs.
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
    // Preserve aggregate order shared by normalization/processing setup and their test fixtures.
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
