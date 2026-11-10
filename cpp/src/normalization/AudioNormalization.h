/// Normalization interface shared by the GUI and CLI targets.
/// The normalization functions rewrite audio files to a target sample peak or integrated loudness,
/// render the output as AIFF, and measure the quantized PCM while writing it.
/// AudioNormalizationResult carries the outcome and the committed output measurements,
/// or the reused source analysis for an in-place file already at its target.

#pragma once

#include "analysis/AudioAnalysis.h"
#include "utils/FileUtils.h"
#include "utils/ThreadUtils.h"

#include <cstdint>
#include <expected>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch
{
/// What normalization aims the file's level at.
enum class AudioNormalizationMode : std::uint8_t {
    /// Scale the file so its sample peak reaches the target peak.
    peak,
    /// Scale the file so its integrated loudness reaches the target loudness, as far as the true peak ceiling allows.
    loudness,
};

/// Options controlling how a file is normalized.
struct AudioNormalizationOptions {
    /// Default target integrated loudness, in LUFS.
    static constexpr double DEFAULT_TARGET_LUFS {-14.0};
    /// Default target sample peak, in dBFS.
    static constexpr double DEFAULT_TARGET_PEAK_DBFS {0.0};
    /// Default highest true peak allowed in loudness mode, in dBTP.
    static constexpr double DEFAULT_TRUE_PEAK_CEILING_DBTP {-0.3};

    /// Largest gain normalization applies, in dB.
    /// A file that would need more is close to silent, and raising it further would mostly amplify noise.
    static constexpr double MAXIMUM_GAIN_DB {40.0};
    /// Highest accepted target integrated loudness, in LUFS.
    static constexpr double MAXIMUM_TARGET_LUFS {-6.0};
    /// Highest accepted target sample peak, in dBFS.
    static constexpr double MAXIMUM_TARGET_PEAK_DBFS {0.0};
    /// Lowest accepted target integrated loudness, in LUFS.
    static constexpr double MINIMUM_TARGET_LUFS {-24.0};
    /// Lowest accepted target sample peak, in dBFS.
    static constexpr double MINIMUM_TARGET_PEAK_DBFS {-12.0};

    /// Whether the target is a sample peak or an integrated loudness.
    AudioNormalizationMode mode {AudioNormalizationMode::peak};
    /// Disposes of the files a normalized output replaces:
    /// a converted source, an overwritten output, and a copy of an `.aif` source rewritten in place.
    /// Moves them to the system trash, unless a caller such as a test passes another function.
    utils::FileDisposer disposeFile {utils::trashFile};

    /// Target integrated loudness in loudness mode, in LUFS.
    double targetLufs {DEFAULT_TARGET_LUFS};
    /// Target sample peak in peak mode, in dBFS.
    double targetPeakDbfs {DEFAULT_TARGET_PEAK_DBFS};
    /// Highest true peak allowed in loudness mode, in dBTP.
    /// When reaching the target loudness would exceed it, the gain is reduced to stay at the ceiling.
    double truePeakCeilingDbtp {DEFAULT_TRUE_PEAK_CEILING_DBTP};

    /// Replace an existing output file that is not the source itself.
    /// When false, a file whose output path is already taken fails instead of overwriting it.
    bool overwriteExisting {false};
};

/// Result of normalizing one file, with the source it ran on and what happened to it.
struct AudioNormalizationResult {
    /// Creates the result of a file the user cancelled before it was changed.
    static AudioNormalizationResult cancelled(const juce::File& file)
    {
        return forFile(file, OperationFailed {.error = "Normalization was cancelled", .cancelled = true});
    }

    /// Creates a failure result for the given file and message.
    static AudioNormalizationResult failure(const juce::File& file, const juce::String& message)
    {
        return forFile(file, OperationFailed {.error = message, .cancelled = false});
    }

    /// Creates the result of an operation on the given source file.
    static AudioNormalizationResult forFile(const juce::File& file, OperationOutcome outcome)
    {
        AudioNormalizationResult result;
        result.file = file;
        result.fileName = file.getFileName();
        result.fullPath = file.getFullPathName();
        result.outcome = std::move(outcome);
        return result;
    }

    /// Returns true unless the output was written and analyzed, or the file was already at its target.
    [[nodiscard]] bool hasError() const noexcept
    {
        return !std::holds_alternative<OperationSucceeded>(outcome);
    }

    /// The source file the operation ran on.
    juce::File file;
    /// Name of the source file, shown in the results table.
    juce::String fileName;
    /// Absolute path of the source file, which identifies its table row.
    juce::String fullPath;
    /// What the operation did with the file.
    OperationOutcome outcome;
};

AudioNormalizationResult normalizeFile(
    const AudioAnalysisRecord& sourceRecord,
    const AudioNormalizationOptions& options,
    const utils::CancellationCheck& isCancelled = {},
    const utils::ProgressCallback& onProgress = {}
);

juce::File getNormalizationOutputFile(const juce::File& sourceFile);
juce::String batchOutputConflictMessage(const juce::File& sourceFile);
juce::String buildNormalizationUnavailableMessage(const std::vector<AudioAnalysisRecord>& records);
juce::String getFormatSupportSummary();
juce::String getNormalizationSupportMessage(const juce::File& file);
juce::StringArray findBatchOutputConflicts(const std::vector<AudioAnalysisRecord>& records);

[[nodiscard]] std::expected<LinearGain, juce::String> getNormalizationGain(
    const AudioAnalysisRecord& record,
    const AudioNormalizationOptions& options
);

int getNormalizationOutputBitDepth(const AudioAnalysisRecord& record, const juce::AudioFormatReader& reader);

bool canNormalizeFile(const juce::File& file);
bool canNormalizeRecords(const std::vector<AudioAnalysisRecord>& records);
}  // namespace audiobatch
