/// Normalization interface shared by the GUI and CLI targets.
/// The normalization functions rewrite audio files to a target sample peak or integrated loudness,
/// renders the output as AIFF, and re-analyzes the result.
/// AudioNormalizationResult carries the outcome of a single normalize-and-reanalyze pass.

#pragma once

#include "analysis/AudioAnalysis.h"
#include "utils/FileUtils.h"
#include "utils/ThreadUtils.h"

#include <cstdint>
#include <expected>
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
    /// Default target sample peak, in dBFS.
    static constexpr double defaultTargetPeakDbfs {0.0};
    /// Default target integrated loudness, in LUFS.
    static constexpr double defaultTargetLufs {-14.0};
    /// Default highest true peak allowed in loudness mode, in dBTP.
    static constexpr double defaultTruePeakCeilingDbtp {-0.3};
    /// Lowest accepted target sample peak, in dBFS.
    static constexpr double minimumTargetPeakDbfs {-12.0};
    /// Highest accepted target sample peak, in dBFS.
    static constexpr double maximumTargetPeakDbfs {0.0};
    /// Lowest accepted target integrated loudness, in LUFS.
    static constexpr double minimumTargetLufs {-24.0};
    /// Highest accepted target integrated loudness, in LUFS.
    static constexpr double maximumTargetLufs {-6.0};
    /// Largest gain normalization applies, in dB.
    /// A file that would need more is close to silent, and raising it further would mostly amplify noise.
    static constexpr double maximumGainDb {40.0};

    /// Disposes of the files a normalized output replaces:
    /// a converted source, an overwritten output, and a copy of an `.aif` source rewritten in place.
    /// Moves them to the system trash, unless a caller such as a test passes another function.
    utils::FileDisposer disposeFile {utils::trashFile};
    /// Replace an existing output file that is not the source itself.
    /// When false, a file whose output path is already taken fails instead of overwriting it.
    bool overwriteExisting {false};
    /// Whether the target is a sample peak or an integrated loudness.
    AudioNormalizationMode mode {AudioNormalizationMode::peak};
    /// Target sample peak in peak mode, in dBFS.
    double targetPeakDbfs {defaultTargetPeakDbfs};
    /// Target integrated loudness in loudness mode, in LUFS.
    double targetLufs {defaultTargetLufs};
    /// Highest true peak allowed in loudness mode, in dBTP.
    /// When reaching the target loudness would exceed it, the gain is reduced to stay at the ceiling.
    double truePeakCeilingDbtp {defaultTruePeakCeilingDbtp};
};

/// Result payload for a single normalize-and-reanalyze operation.
struct AudioNormalizationResult {
    /// Returns true when the normalize pass could not complete successfully.
    [[nodiscard]] bool hasError() const noexcept
    {
        return !succeeded;
    }

    /// Creates a failure result for the given file and message.
    static AudioNormalizationResult failure(const juce::File& file, const juce::String& message)
    {
        AudioNormalizationResult result;
        result.file = file;
        result.fileName = file.getFileName();
        result.fullPath = file.getFullPathName();
        result.errorMessage = message;
        return result;
    }

    /// The source file the operation ran on.
    juce::File file;
    /// Name of the source file, shown in the results table.
    juce::String fileName;
    /// Absolute path of the source file, which identifies its table row.
    juce::String fullPath;
    /// Empty on success, otherwise the reason the operation failed.
    juce::String errorMessage;
    /// Information about a successful run worth showing to the user, such as blocks that failed to decode.
    juce::String notice;
    /// Analysis of the normalized file, valid when the re-analysis succeeded.
    AudioAnalysisRecord analysisRecord;
    /// True when the normalization pass completed.
    bool succeeded {false};
    /// True once the output was moved into place, which can happen even when re-analysis then failed.
    /// A converted source is in the trash by then, so callers move the record to the output either way.
    bool committed {false};
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

[[nodiscard]] std::expected<float, juce::String> getNormalizationGain(
    const AudioAnalysisRecord& record,
    const AudioNormalizationOptions& options
);

bool canNormalizeFile(const juce::File& file);
bool canNormalizeRecords(const std::vector<AudioAnalysisRecord>& records);

int getNormalizationOutputBitDepth(const AudioAnalysisRecord& record, const juce::AudioFormatReader& reader);
}  // namespace audiobatch
