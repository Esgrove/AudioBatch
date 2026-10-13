/// Shared rules for read failures while decoding an audio file block by block.
/// Declares DecodeFailureTracker, used by analysis, normalization, and plugin processing,
/// so a file that can be analyzed can also be rewritten, and no pass silently works on truncated audio.

#pragma once

#include "StringFormat.h"

#include <JuceHeader.h>

namespace audiobatch
{
/// Tracks block read failures for one decode pass over a file.
/// A failed read in the final block is tolerated,
/// because MP3 files often report a length that overshoots the decodable audio.
/// Isolated failures elsewhere are tolerated too, because the built-in MP3 decoder can fail mid-stream on files
/// that other decoders handle fine.
/// A failed block keeps the samples a floating point decoder produced before the error and is silent otherwise,
/// as utils::readFloatSamples describes.
/// Several failures in a row mean the rest of the file cannot be decoded, and the pass must stop with an error.
class DecodeFailureTracker
{
public:
    /// Creates a tracker for a stream with the given length in samples.
    explicit DecodeFailureTracker(const juce::int64 streamLength) noexcept : totalSamples(streamLength) {}

    /// Records the outcome of reading one block.
    /// Returns false when decoding must stop, after which errorMessage describes the failure.
    bool recordBlock(const bool readSucceeded, const juce::int64 samplePosition, const int samplesInBlock) noexcept
    {
        if (readSucceeded) {
            consecutiveFailures = 0;
            return true;
        }

        if (samplePosition + samplesInBlock >= totalSamples) {
            return true;
        }

        ++failedBlocks;
        ++consecutiveFailures;

        if (failedBlockPosition < 0) {
            failedBlockPosition = samplePosition;
        }

        if (consecutiveFailures >= maximumConsecutiveFailures) {
            stoppedAt = samplePosition;
            return false;
        }

        return true;
    }

    /// Returns the number of failed blocks before the final block.
    [[nodiscard]] int getFailedBlockCount() const noexcept
    {
        return failedBlocks;
    }

    /// Returns the sample position of the first failed block before the final block, or -1 when there was none.
    [[nodiscard]] juce::int64 getFirstFailurePosition() const noexcept
    {
        return failedBlockPosition;
    }

    /// Returns a user-facing notice about the tolerated failures of a completed pass,
    /// or an empty string when every block before the final one decoded.
    /// A pass that writes output reports it, because the failed blocks may hold a short dropout.
    [[nodiscard]] juce::String getNotice() const
    {
        if (failedBlocks == 0) {
            return {};
        }

        return utils::format(
            "{} audio block(s) failed to decode, first at sample {}", failedBlocks, failedBlockPosition
        );
    }

    /// Returns the user-facing error after recordBlock returned false.
    [[nodiscard]] juce::String getErrorMessage() const
    {
        return utils::format("Audio decoding failed at sample {} of {}", stoppedAt, totalSamples);
    }

    /// Number of consecutive failed blocks, outside the final block, after which decoding stops.
    static constexpr int maximumConsecutiveFailures = 3;

private:
    juce::int64 totalSamples;
    juce::int64 failedBlockPosition = -1;
    juce::int64 stoppedAt = -1;
    int failedBlocks = 0;
    int consecutiveFailures = 0;
};
}  // namespace audiobatch
