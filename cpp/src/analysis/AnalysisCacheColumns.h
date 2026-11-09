/// Codecs of the shared analysis cache's measurement columns.
/// Declares the short-term loudness history blob codec, the readers and binders of the source-statistics columns,
/// and the bounds both apply, which AnalysisCache uses to load and store every record.

#pragma once

#include "analysis/AudioAnalysisTypes.h"

#include <cstddef>
#include <memory>
#include <span>
#include <sqlite3.h>

namespace audiobatch
{
/// Largest channel count a cached record may describe, matching the meters of both applications.
constexpr int MAXIMUM_CACHED_CHANNELS {64};

/// Practical per-record allocation budget for a loudness history, separate from SQLite's much larger blob limit.
constexpr std::size_t MAXIMUM_HISTORY_BYTES {16UZ * 1024 * 1024};

/// Upper loudness bound of cached values, deliberately above the roughly +800 LUFS of finite float PCM.
constexpr double MAXIMUM_CACHED_LOUDNESS {1000.0};

[[nodiscard]] juce::MemoryBlock serializeLoudnessHistory(const AudioAnalysisRecord& record);
[[nodiscard]] std::shared_ptr<const ShortTermLoudnessHistory> deserializeLoudnessHistory(
    std::span<const std::byte> data,
    const AudioAnalysisRecord& record
);

[[nodiscard]] bool readSignalMetrics(sqlite3_stmt& statement, AudioAnalysisRecord& record);
[[nodiscard]] bool bindSignalMetrics(sqlite3_stmt& statement, const AudioAnalysisRecord& record);
}  // namespace audiobatch
