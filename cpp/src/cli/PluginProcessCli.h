/// CLI plugin processing through native saved presets and the shared processing coordinator.
/// The CLI pumps the message loop on its main thread while discovery and rendering run on workers.

#pragma once

#include "cli/AudioAnalysisCli.h"

#include <expected>
#include <map>
#include <vector>

namespace audiobatch::cli
{
void persistOperationOutput(AnalysisCache& cache, const OperationOutcome& outcome);

[[nodiscard]] std::expected<std::map<juce::String, AudioNormalizationResult>, juce::String> processWithProgress(
    const std::vector<AudioAnalysisRecord>& records,
    const ProcessCommand& command,
    AnalysisCache& cache,
    const utils::CancellationCheck& isInterrupted
);
}  // namespace audiobatch::cli
