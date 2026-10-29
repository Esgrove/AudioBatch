/// Implementation of the source overload history builder, its validation, and its cache blob format.
/// The blob holds an explicit little-endian header followed by one flag byte per bucket and channel.

#include "analysis/OverloadHistory.h"

#include "analysis/AudioAnalysisTypes.h"

#include <JuceHeader.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
/// Identifies an overload history blob, the bytes "OVLD" read as a little-endian integer.
constexpr int overloadHistoryMagic {0x444c564f};
/// Raised when the blob layout changes, so older blobs are rejected and their files analyzed again.
constexpr int overloadHistoryVersion {1};
/// Magic, version, sample rate, length, samples per bucket, channel count, and bucket count.
constexpr std::size_t overloadHistoryHeaderBytes {4 + 4 + 8 + 8 + 8 + 4 + 4};
/// Most channels the loudness meter accepts, which the analysis also enforces.
constexpr int maximumOverloadChannels {64};
}  // namespace

/// Starts an empty history for a file with the given stream parameters and analysis block size.
OverloadHistoryBuilder::OverloadHistoryBuilder(
    const int channelCount,
    const double sampleRate,
    const std::int64_t lengthInSamples,
    const int blockSize
) :
    pending(static_cast<std::size_t>(std::max(0, channelCount)), 0),
    samplesPerBlock(std::max(1, blockSize))
{
    history.sampleRate = sampleRate;
    history.lengthInSamples = lengthInSamples;
    history.samplesPerBucket = samplesPerBlock;
    history.channelCount = std::max(0, channelCount);
    history.flags.reserve(OverloadHistory::maximumBuckets * pending.size());
}

/// Folds the flags of the next analysis block, one byte per channel, into the pending bucket.
/// A bucket that holds its full number of blocks moves into the history,
/// merging the history first when it already has the maximum number of buckets.
void OverloadHistoryBuilder::addBlock(const std::span<const std::uint8_t> blockFlags)
{
    const auto count = std::min(blockFlags.size(), pending.size());
    for (std::size_t channel = 0; channel < count; ++channel) {
        pending[channel] |= static_cast<std::uint8_t>(blockFlags[channel] & OverloadHistory::allFlags);
    }
    ++pendingBlocks;
    if (pendingBlocks < blocksPerBucket) {
        return;
    }
    if (history.bucketCount() == OverloadHistory::maximumBuckets) {
        // The pending bucket becomes the first half of a bucket twice as long, which later blocks complete.
        mergePairs();
        return;
    }
    history.flags.insert(history.flags.end(), pending.begin(), pending.end());
    std::ranges::fill(pending, std::uint8_t {0});
    pendingBlocks = 0;
}

/// Moves a final partial bucket into the history and returns the finished history.
std::shared_ptr<const OverloadHistory> OverloadHistoryBuilder::finish()
{
    if (pendingBlocks > 0) {
        if (history.bucketCount() == OverloadHistory::maximumBuckets) {
            mergePairs();
        }
        history.flags.insert(history.flags.end(), pending.begin(), pending.end());
        std::ranges::fill(pending, std::uint8_t {0});
        pendingBlocks = 0;
    }
    return std::make_shared<const OverloadHistory>(std::move(history));
}

/// Merges each pair of neighbouring buckets with a bitwise or and doubles the bucket length.
void OverloadHistoryBuilder::mergePairs()
{
    const auto channels = static_cast<std::size_t>(history.channelCount);
    const auto buckets = history.bucketCount();
    for (std::size_t bucket = 0; bucket < buckets / 2; ++bucket) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            history.flags[bucket * channels + channel] = static_cast<std::uint8_t>(
                history.flags[(2 * bucket) * channels + channel] | history.flags[(2 * bucket + 1) * channels + channel]
            );
        }
    }
    history.flags.resize((buckets / 2) * channels);
    blocksPerBucket *= 2;
    history.samplesPerBucket = blocksPerBucket * samplesPerBlock;
}

/// Returns the source frames of one analysis block, a tenth of a second matching the short-term loudness updates.
int overloadBlockSize(const double sampleRate) noexcept
{
    return std::max(1, juce::roundToInt(sampleRate / ShortTermLoudnessHistory::updateRateHz));
}

/// Projects the flags of one channel onto equal-width columns spanning the visible time range.
/// Each column holds the bitwise or of every bucket that overlaps it,
/// so an overload shorter than a column stays visible when zoomed out.
/// True peak flags also cover truePeakCoverageFrames before their bucket, for the interpolation delay,
/// while sample flags keep their exact bucket.
std::vector<std::uint8_t> projectOverloadColumns(
    const OverloadHistory& history,
    const int channel,
    const juce::Range<double> visibleRange,
    const int columns
)
{
    std::vector<std::uint8_t> projected(static_cast<std::size_t>(std::max(0, columns)), 0);
    const auto buckets = history.bucketCount();
    if (columns <= 0 || buckets == 0 || channel < 0 || channel >= history.channelCount
        || visibleRange.getLength() <= 0.0 || history.sampleRate <= 0.0 || history.samplesPerBucket <= 0)
    {
        return projected;
    }
    const auto secondsPerColumn = visibleRange.getLength() / static_cast<double>(columns);
    const auto coverageSeconds = static_cast<double>(truePeakCoverageFrames) / history.sampleRate;
    const auto bucketSeconds = static_cast<double>(history.samplesPerBucket) / history.sampleRate;
    const auto firstBucket = static_cast<std::size_t>(
        std::clamp(std::floor(visibleRange.getStart() / bucketSeconds), 0.0, static_cast<double>(buckets - 1))
    );
    const auto lastColumn = static_cast<double>(columns - 1);
    for (auto bucket = firstBucket; bucket < buckets; ++bucket) {
        const auto start = history.bucketStartTime(bucket);
        if (start - coverageSeconds >= visibleRange.getEnd()) {
            break;
        }
        const auto flags = history.flagsAt(bucket, channel);
        if (flags == 0) {
            continue;
        }
        const auto end = history.bucketEndTime(bucket);
        const auto fill = [&](const double markStart, const double markEnd, const std::uint8_t bits) {
            if (markEnd <= visibleRange.getStart() || markStart >= visibleRange.getEnd()) {
                return;
            }
            const auto firstColumn
                = std::clamp(std::floor((markStart - visibleRange.getStart()) / secondsPerColumn), 0.0, lastColumn);
            const auto finalColumn = std::clamp(
                std::ceil((markEnd - visibleRange.getStart()) / secondsPerColumn) - 1.0, firstColumn, lastColumn
            );
            for (auto column = static_cast<std::size_t>(firstColumn); column <= static_cast<std::size_t>(finalColumn);
                 ++column)
            {
                projected[column] |= bits;
            }
        };
        if ((flags & OverloadHistory::sampleOver) != 0) {
            fill(start, end, OverloadHistory::sampleOver);
        }
        if ((flags & OverloadHistory::truePeakOver) != 0) {
            fill(std::max(0.0, start - coverageSeconds), end, OverloadHistory::truePeakOver);
        }
    }
    return projected;
}

/// Checks the internal consistency of a history before it is stored or adopted:
/// its channel count, its timing, the bucket count its timing implies,
/// that the bucket length is the smallest power-of-two multiple of the block size that keeps the history bounded,
/// and that every flag byte holds only legal bits.
bool isValidOverloadHistory(const OverloadHistory& history)
{
    if (history.channelCount <= 0 || history.channelCount > maximumOverloadChannels
        || !std::isfinite(history.sampleRate) || history.sampleRate < 1.0
        || history.sampleRate > static_cast<double>(std::numeric_limits<int>::max()) || history.lengthInSamples < 0)
    {
        return false;
    }
    const auto blockSize = static_cast<std::int64_t>(overloadBlockSize(history.sampleRate));
    if (history.samplesPerBucket < blockSize || history.samplesPerBucket % blockSize != 0) {
        return false;
    }
    const auto blocksPerBucket = history.samplesPerBucket / blockSize;
    if (!std::has_single_bit(static_cast<std::uint64_t>(blocksPerBucket))) {
        return false;
    }
    const auto blocks = history.lengthInSamples / blockSize + (history.lengthInSamples % blockSize != 0 ? 1 : 0);
    const auto bucketsFor
        = [blocks](const std::int64_t perBucket) { return blocks / perBucket + (blocks % perBucket != 0 ? 1 : 0); };
    const auto maximumBuckets = static_cast<std::int64_t>(OverloadHistory::maximumBuckets);
    const auto expectedBuckets = bucketsFor(blocksPerBucket);
    if (expectedBuckets > maximumBuckets || (blocksPerBucket > 1 && bucketsFor(blocksPerBucket / 2) <= maximumBuckets))
    {
        return false;
    }
    if (history.flags.size()
        != static_cast<std::size_t>(expectedBuckets) * static_cast<std::size_t>(history.channelCount))
    {
        return false;
    }
    return std::ranges::all_of(history.flags, [](const auto value) {
        return (value & static_cast<std::uint8_t>(~OverloadHistory::allFlags)) == 0;
    });
}

/// Returns true when a valid history describes the stream of the analysis record it belongs to.
bool overloadHistoryMatches(const OverloadHistory& history, const AudioAnalysisRecord& record)
{
    return isValidOverloadHistory(history) && history.channelCount == record.channels
        && juce::roundToInt(history.sampleRate) == record.sampleRate
        && history.lengthInSamples == record.lengthInSamples
        && juce::approximatelyEqual(
               static_cast<double>(history.lengthInSamples) / history.sampleRate, record.durationSeconds
        );
}

/// Writes an explicit little-endian header and the flag bytes, or an empty block for an invalid history.
juce::MemoryBlock serializeOverloadHistory(const OverloadHistory& history)
{
    juce::MemoryBlock data;
    if (!isValidOverloadHistory(history)) {
        return data;
    }
    juce::MemoryOutputStream output(data, false);
    output.writeInt(overloadHistoryMagic);
    output.writeInt(overloadHistoryVersion);
    output.writeDouble(history.sampleRate);
    output.writeInt64(history.lengthInSamples);
    output.writeInt64(history.samplesPerBucket);
    output.writeInt(history.channelCount);
    output.writeInt(static_cast<int>(history.bucketCount()));
    output.write(history.flags.data(), history.flags.size());
    return data;
}

/// Validates the header and the exact payload size before allocating the flags,
/// then validates the complete history, returning null for any malformed blob.
std::shared_ptr<const OverloadHistory> deserializeOverloadHistory(const void* data, const std::size_t bytes)
{
    if (data == nullptr || bytes < overloadHistoryHeaderBytes) {
        return {};
    }
    juce::MemoryInputStream input(data, bytes, false);
    if (input.readInt() != overloadHistoryMagic || input.readInt() != overloadHistoryVersion) {
        return {};
    }
    OverloadHistory history;
    history.sampleRate = input.readDouble();
    history.lengthInSamples = input.readInt64();
    history.samplesPerBucket = input.readInt64();
    history.channelCount = input.readInt();
    const auto buckets = input.readInt();
    if (history.channelCount <= 0 || history.channelCount > maximumOverloadChannels || buckets < 0
        || std::cmp_greater(buckets, OverloadHistory::maximumBuckets)
        || bytes
            != overloadHistoryHeaderBytes
                + static_cast<std::size_t>(buckets) * static_cast<std::size_t>(history.channelCount))
    {
        return {};
    }
    history.flags.resize(static_cast<std::size_t>(buckets) * static_cast<std::size_t>(history.channelCount));
    if (!history.flags.empty()
        && input.read(history.flags.data(), static_cast<int>(history.flags.size()))
            != static_cast<int>(history.flags.size()))
    {
        return {};
    }
    if (!isValidOverloadHistory(history)) {
        return {};
    }
    return std::make_shared<const OverloadHistory>(std::move(history));
}
}  // namespace audiobatch
