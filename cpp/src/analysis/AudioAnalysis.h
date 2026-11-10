/// Audio analysis interface shared by the GUI and CLI targets.
/// Declares the free functions that decode audio files and measure sample peak, true peak,
/// and integrated loudness, and provide file discovery and record sorting built on the AudioAnalysisRecord types.
/// Display formatting is declared in AudioAnalysisFormatting.h.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "utils/ThreadUtils.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace audiobatch
{
class AudioMeasurement;

/// Transient loudness of decoded samples after custom gain and hard clipping at 0 dBFS.
/// Never persisted or substituted for source measurements, and excludes limiter and plugin behavior.
struct GainAdjustedLoudness {
    double integratedLufs {AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS};
    double maxShortTermLufs {AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS};
};

[[nodiscard]] std::optional<double> estimateLimiterIntegratedLoudness(
    double sourceIntegratedLufs,
    Decibels gain,
    double hardClipIntegratedLufs
);

[[nodiscard]] std::optional<GainAdjustedLoudness> measureGainAdjustedLoudness(
    const juce::File& file,
    Decibels gain,
    const utils::CancellationCheck& isCancelled = {}
);

/// Observes the decoder's measurement after each successful push, before progress is reported.
/// The measurement is borrowed only for the call. Optional preview failures never fail analysis.
using AnalysisPreviewObserver = std::function<void(const AudioMeasurement&, std::int64_t decodedFrames)>;

AudioAnalysisRecord analyzeFile(
    const juce::File& file,
    const utils::CancellationCheck& isCancelled = {},
    const utils::ProgressCallback& onProgress = {},
    const AnalysisPreviewObserver& onPreview = {}
);

juce::Array<juce::File> collectInputFiles(
    const juce::Array<juce::File>& inputPaths,
    bool recursive,
    const utils::CancellationCheck& isCancelled = {}
);

bool isSupportedAudioFile(const juce::File& file);

void sortRecords(std::vector<AudioAnalysisRecord>& records, AudioAnalysisSortMode sortMode, bool ascending);
}  // namespace audiobatch
