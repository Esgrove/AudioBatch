/// Building, validating, and serializing the source overload history of an analysis.
/// Declares OverloadHistoryBuilder, which collects per-channel sample
/// and true peak overload flags for each analysis block
/// and merges neighbouring buckets to keep the history bounded,
/// and the versioned little-endian blob format the analysis cache stores the history in.

#pragma once

#include "analysis/AudioAnalysisTypes.h"

#include <JuceHeader.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace audiobatch
{
/// Collects the overload flags of consecutive analysis blocks into a bounded OverloadHistory.
/// Each block becomes one bucket until the history would pass OverloadHistory::maximumBuckets,
/// then neighbouring buckets merge with a bitwise or and the stride doubles,
/// so isolated overloads and the final partial bucket are always kept.
class OverloadHistoryBuilder
{
public:
    OverloadHistoryBuilder(int channelCount, double sampleRate, std::int64_t lengthInSamples, int blockSize);

    void addBlock(std::span<const std::uint8_t> blockFlags);
    [[nodiscard]] std::shared_ptr<const OverloadHistory> finish();

private:
    void mergePairs();

    /// The history being built, without the bucket still collecting blocks.
    OverloadHistory history;
    /// Flags of the bucket still collecting blocks, one byte per channel.
    std::vector<std::uint8_t> pending;
    /// Analysis blocks per bucket, a power of two.
    std::int64_t blocksPerBucket {1};
    /// Blocks already folded into the pending bucket.
    std::int64_t pendingBlocks {0};
    /// Source frames per analysis block.
    int samplesPerBlock {1};
};

/// Source frames the true peak flags extend backwards,
/// covering the delay of libebur128's interpolation filter, which reports a peak near a block start in the next block.
inline constexpr std::int64_t truePeakCoverageFrames {32};

[[nodiscard]] int overloadBlockSize(double sampleRate) noexcept;
[[nodiscard]] std::vector<std::uint8_t> projectOverloadColumns(
    const OverloadHistory& history,
    int channel,
    juce::Range<double> visibleRange,
    int columns
);
[[nodiscard]] bool isValidOverloadHistory(const OverloadHistory& history);
[[nodiscard]] bool overloadHistoryMatches(const OverloadHistory& history, const AudioAnalysisRecord& record);
[[nodiscard]] juce::MemoryBlock serializeOverloadHistory(const OverloadHistory& history);
[[nodiscard]] std::shared_ptr<const OverloadHistory> deserializeOverloadHistory(const void* data, std::size_t bytes);
}  // namespace audiobatch
