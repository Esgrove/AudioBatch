/// Implements the SPSC stereo ring between the audio thread and analyzer.
/// Each frame carries its source epoch, so tap, sample-rate, and clear boundaries discard stale audio
/// without resetting indices or making the control thread a second consumer.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace esgrove::image_control
{
/// Storage frames. AbstractFifo reserves one slot to distinguish full from empty.
constexpr int ANALYZER_FIFO_FRAMES {32'768};
/// Default source rate until the processor is prepared by its host.
constexpr double ANALYZER_FIFO_DEFAULT_SAMPLE_RATE {48'000.0};
/// Epoch token captured by the producer before choosing its analyzer tap.
using AnalyzerEpoch = std::uint64_t;

/// A fixed-size stereo ring with exactly one producer and one consumer.
/// The control-side epoch setters never read or mutate the queue indices.
class StereoSampleFifo
{
public:
    /// Allocates the audio storage before either thread starts.
    StereoSampleFifo() : buffer(2, ANALYZER_FIFO_FRAMES), frameEpochs(static_cast<std::size_t>(ANALYZER_FIFO_FRAMES), 0)
    {
        buffer.clear();
    }

    /// Returns the tap encoded in a captured epoch, not the potentially newer control setting.
    [[nodiscard]] static bool analyzesOutput(const AnalyzerEpoch epoch) noexcept
    {
        return (epoch & OUTPUT_BIT) != 0;
    }

    /// Returns whether a token describes a fully published source configuration.
    [[nodiscard]] static bool isStable(const AnalyzerEpoch epoch) noexcept
    {
        return (epoch & UPDATING_BIT) == 0;
    }

    /// Captures the source generation before the producer selects input or output.
    [[nodiscard]] AnalyzerEpoch getEpoch() const noexcept
    {
        return sourceEpoch.load(std::memory_order_acquire);
    }

    /// Returns the source rate. Read between two equal, stable epoch observations for a coherent snapshot.
    [[nodiscard]] double getSampleRate() const noexcept
    {
        return sampleRate.load(std::memory_order_acquire);
    }

    /// Invalidates queued frames without touching either thread's ring indices.
    void beginEpoch() noexcept
    {
        sourceEpoch.fetch_add(EPOCH_STEP, std::memory_order_acq_rel);
    }

    /// Publishes a source rate and invalidates old audio. Called during processor preparation, not processing.
    void setSampleRate(const double rate) noexcept
    {
        if (!std::isfinite(rate) || rate <= 0.0 || juce::approximatelyEqual(rate, getSampleRate())) {
            return;
        }
        auto epoch = getEpoch();
        for (;;) {
            if (isStable(epoch)
                && sourceEpoch.compare_exchange_weak(epoch, epoch | UPDATING_BIT, std::memory_order_acq_rel))
            {
                break;
            }
            epoch = getEpoch();
        }
        sampleRate.store(rate, std::memory_order_release);
        // Other invalidations may advance the generation while the rate is being published.
        sourceEpoch.fetch_add(EPOCH_STEP - UPDATING_BIT, std::memory_order_release);
    }

    /// Publishes the chosen tap and invalidates audio captured from the other tap.
    void setTap(const bool output) noexcept
    {
        auto epoch = getEpoch();
        for (;;) {
            if (!isStable(epoch)) {
                epoch = getEpoch();
                continue;
            }
            if (analyzesOutput(epoch) == output) {
                return;
            }
            const auto next = ((epoch + EPOCH_STEP) & ~OUTPUT_BIT) | (output ? OUTPUT_BIT : 0);
            if (sourceEpoch.compare_exchange_weak(epoch, next, std::memory_order_acq_rel)) {
                return;
            }
        }
    }

    /// Pops audio only. Call on the consumer thread, never alongside another pop.
    int pop(const std::span<float> left, const std::span<float> right)
    {
        return pop(left, right, {});
    }

    /// Pops audio and optionally its per-frame epochs. All nonempty spans must have the same size.
    /// The read scope publishes consumption only after audio and metadata have both been copied.
    int pop(const std::span<float> left, const std::span<float> right, const std::span<AnalyzerEpoch> epochs)
    {
        jassert(left.size() == right.size());
        jassert(epochs.empty() || epochs.size() == left.size());
        const auto count = std::min(left.size(), static_cast<std::size_t>(ANALYZER_FIFO_FRAMES));
        const auto read = fifo.read(static_cast<int>(count));
        copyOut(read.startIndex1, read.blockSize1, 0, left, right, epochs);
        copyOut(read.startIndex2, read.blockSize2, read.blockSize1, left, right, epochs);
        return read.blockSize1 + read.blockSize2;
    }

    /// Convenience push for producers that do not select a tap between capture and push.
    int push(const std::span<const float> left, const std::span<const float> right)
    {
        return push(left, right, getEpoch());
    }

    /// Appends the frames that fit, dropping the rest, and returns the accepted count.
    /// The producer captures epoch before selecting the tap. A concurrent change drops the old block.
    /// Never resets the consumer index, blocks, or allocates.
    int push(const std::span<const float> left, const std::span<const float> right, const AnalyzerEpoch epoch)
    {
        jassert(left.size() == right.size());
        if (!isStable(epoch) || epoch != getEpoch()) {
            return 0;
        }
        const auto count = std::min(left.size(), static_cast<std::size_t>(ANALYZER_FIFO_FRAMES));
        const auto write = fifo.write(static_cast<int>(count));
        copyIn(write.startIndex1, write.blockSize1, 0, left, right, epoch);
        copyIn(write.startIndex2, write.blockSize2, write.blockSize1, left, right, epoch);
        return write.blockSize1 + write.blockSize2;
    }

private:
    /// Copies one contiguous input run and its epoch before publishing the write scope.
    void copyIn(
        const int ringIndex,
        const int frames,
        const int offset,
        const std::span<const float> left,
        const std::span<const float> right,
        const AnalyzerEpoch epoch
    )
    {
        if (frames > 0) {
            buffer.copyFrom(0, ringIndex, left.subspan(static_cast<std::size_t>(offset)).data(), frames);
            buffer.copyFrom(1, ringIndex, right.subspan(static_cast<std::size_t>(offset)).data(), frames);
            std::fill_n(frameEpochs.begin() + ringIndex, frames, epoch);
        }
    }

    /// Copies one contiguous audio and metadata run while its slots still belong to the consumer.
    void copyOut(
        const int ringIndex,
        const int frames,
        const int offset,
        const std::span<float> left,
        const std::span<float> right,
        const std::span<AnalyzerEpoch> epochs
    ) const
    {
        if (frames <= 0) {
            return;
        }
        const auto first = static_cast<std::size_t>(ringIndex);
        const auto count = static_cast<std::size_t>(frames);
        const auto destination = static_cast<std::size_t>(offset);
        const auto ringSize = static_cast<std::size_t>(ANALYZER_FIFO_FRAMES);
        std::ranges::copy(
            std::span<const float>(buffer.getReadPointer(0), ringSize).subspan(first, count),
            left.subspan(destination).begin()
        );
        std::ranges::copy(
            std::span<const float>(buffer.getReadPointer(1), ringSize).subspan(first, count),
            right.subspan(destination).begin()
        );
        if (!epochs.empty()) {
            std::ranges::copy(
                std::span<const AnalyzerEpoch>(frameEpochs).subspan(first, count), epochs.subspan(destination).begin()
            );
        }
    }

    /// Bit marking an in-progress sample-rate publication.
    static constexpr AnalyzerEpoch UPDATING_BIT {1};
    /// Bit carrying the selected tap in every generation.
    static constexpr AnalyzerEpoch OUTPUT_BIT {2};
    /// Increment preserving the two configuration bits.
    static constexpr AnalyzerEpoch EPOCH_STEP {4};

    /// Publishes producer and consumer indices after their scoped copies complete.
    juce::AbstractFifo fifo {ANALYZER_FIFO_FRAMES};
    /// Audio slots owned exclusively by the producer or consumer according to fifo.
    juce::AudioBuffer<float> buffer;
    /// Source token per slot, published together with its two audio samples.
    std::vector<AnalyzerEpoch> frameEpochs;
    /// Source generation and tap, with a transient bit while publishing a rate.
    std::atomic<AnalyzerEpoch> sourceEpoch {OUTPUT_BIT};
    /// Source sample rate, read with the sourceEpoch publication protocol.
    std::atomic<double> sampleRate {ANALYZER_FIFO_DEFAULT_SAMPLE_RATE};
};
}  // namespace esgrove::image_control
