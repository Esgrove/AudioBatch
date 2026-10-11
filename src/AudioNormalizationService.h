/// Normalization interface shared by the GUI and CLI targets.
/// AudioNormalizationService rewrites audio files to a target sample peak or integrated loudness,
/// renders the output as AIFF, and re-analyzes the result.
/// AudioNormalizationResult carries the outcome of a single normalize-and-reanalyze pass.

#pragma once

#include "AudioAnalysisService.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

/// What normalization aims the file's level at.
enum class AudioNormalizationMode : std::uint8_t {
    /// Scale the file so its sample peak reaches the target peak.
    peak,
    /// Scale the file so its integrated loudness reaches the target loudness,
    /// as far as the true peak ceiling allows.
    loudness,
};

/// Options controlling how a file is normalized.
struct AudioNormalizationOptions {
    /// Default target sample peak, in dBFS.
    static constexpr double defaultTargetPeakDbfs = 0.0;
    /// Default target integrated loudness, in LUFS.
    static constexpr double defaultTargetLufs = -14.0;
    /// Default highest true peak allowed in loudness mode, in dBTP.
    static constexpr double defaultTruePeakCeilingDbtp = -0.3;

    /// Replace an existing output file that is not the source itself.
    /// When false, a file whose output path is already taken fails instead of overwriting it.
    bool overwriteExisting = false;
    AudioNormalizationMode mode = AudioNormalizationMode::peak;
    /// Target sample peak in peak mode, in dBFS.
    double targetPeakDbfs = defaultTargetPeakDbfs;
    /// Target integrated loudness in loudness mode, in LUFS.
    double targetLufs = defaultTargetLufs;
    /// Highest true peak allowed in loudness mode, in dBTP.
    /// When reaching the target loudness would exceed it, the gain is reduced to stay at the ceiling.
    double truePeakCeilingDbtp = defaultTruePeakCeilingDbtp;
};

/// Result payload for a single normalize-and-reanalyze operation.
struct AudioNormalizationResult {
    juce::File file;
    juce::String fileName;
    juce::String fullPath;
    juce::String errorMessage;
    AudioAnalysisRecord analysisRecord;
    bool succeeded = false;

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
};

/// Stateless helpers for normalizing audio files and refreshing their analysis data.
class AudioNormalizationService
{
public:
    /// Returns true when the caller wants normalization to stop as soon as possible.
    /// Polled from the normalizing thread, so it must be thread-safe and cheap.
    using CancellationCheck = std::function<bool()>;

    /// Returns the output path normalization writes for the given source, which is always a sibling `<name>.aif`.
    static juce::File getOutputFile(const juce::File& sourceFile);

    /// Returns the full paths of the records that cannot be normalized in this batch
    /// because an earlier record in the batch writes the same output file.
    /// A source that is already the output file, such as `<name>.aif` normalized in place, always wins.
    /// Otherwise the record that sorts first by path wins, so the result does not depend on input order.
    static juce::StringArray findBatchOutputConflicts(const std::vector<AudioAnalysisRecord>& records);

    /// Returns the linear gain that brings the analyzed file to the target in the options,
    /// or an empty optional with errorMessage set when the file has no measurable level for that mode.
    static std::optional<float> getNormalizationGain(
        const AudioAnalysisRecord& record,
        const AudioNormalizationOptions& options,
        juce::String& errorMessage
    );

    /// Returns the AIFF bit depth for output written from the given source.
    /// Keeps the analyzed source depth when the AIFF writer supports it,
    /// uses 16 bits for MP3 sources, and falls back to the highest supported depth,
    /// so for example 32-bit float sources are written as 24-bit.
    static int getOutputBitDepth(const AudioAnalysisRecord& record, const juce::AudioFormatReader& reader);

    /// Returns the user-facing error message for a record that lost a batch output conflict.
    static juce::String batchOutputConflictMessage(const juce::File& sourceFile);

    /// Returns true when the file's format can be rewritten by this build during normalization.
    static bool canNormalizeFile(const juce::File& file);

    /// Describes why normalization is unavailable for the given file format, or returns an empty string when supported.
    static juce::String getNormalizationSupportMessage(const juce::File& file);

    /// Summarizes the readable and writable formats available for in-place normalization in this build.
    static juce::String getFormatSupportSummary();

    /// Rewrites a file to the target level in the options, then re-analyzes the result.
    /// Fails without writing anything when the output path already exists and is not the source,
    /// unless options.overwriteExisting is set, in which case the existing output is moved to the system trash.
    /// A call cancelled before its output is committed returns a failure result and leaves the original file untouched.
    /// When the file changed on disk since sourceRecord was analyzed, it is analyzed again before computing the gain.
    static AudioNormalizationResult normalizeFile(
        const AudioAnalysisRecord& sourceRecord,
        const AudioNormalizationOptions& options,
        const CancellationCheck& isCancelled = {}
    );

private:
    /// Returns a per-thread format manager used for reading source files.
    /// Thread-local state avoids locking when normalize jobs run on multiple worker threads.
    static juce::AudioFormatManager& getThreadLocalFormatManager();
};
