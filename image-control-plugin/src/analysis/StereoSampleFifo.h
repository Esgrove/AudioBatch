/// Declares and implements StereoSampleFifo,
/// the lock-free single-producer single-consumer ring that carries audio from the audio thread to the analyzer.
/// The audio thread pushes every block, the editor's analysis worker thread pops,
/// and samples that do not fit while no editor drains the ring are dropped.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstddef>
#include <span>

namespace esgrove::image_control
{
/// Frames the analyzer ring holds, about two thirds of a second at 48 kHz.
constexpr int ANALYZER_FIFO_FRAMES {32'768};

/// A fixed-size stereo sample ring for one producer thread and one consumer thread.
class StereoSampleFifo
{
public:
    /// Allocates a ring of ANALYZER_FIFO_FRAMES stereo frames.
    StereoSampleFifo() : buffer(2, ANALYZER_FIFO_FRAMES)
    {
        buffer.clear();
    }

    /// Copies up to output.size() frames into the two outputs, which must be the same size,
    /// and returns how many were copied. Called only by the consumer thread.
    int pop(const std::span<float> left, const std::span<float> right)
    {
        jassert(left.size() == right.size());
        const auto read = fifo.read(static_cast<int>(left.size()));
        copyOut(read.startIndex1, read.blockSize1, 0, left, right);
        copyOut(read.startIndex2, read.blockSize2, read.blockSize1, left, right);
        return read.blockSize1 + read.blockSize2;
    }

    /// Appends frames, dropping those that do not fit. Never blocks or allocates.
    /// A mono signal passes the same span as both channels. Called only by the producer thread.
    void push(const std::span<const float> left, const std::span<const float> right)
    {
        jassert(left.size() == right.size());
        const int frames = std::min(static_cast<int>(left.size()), fifo.getFreeSpace());
        const auto write = fifo.write(frames);
        copyIn(write.startIndex1, write.blockSize1, 0, left, right);
        copyIn(write.startIndex2, write.blockSize2, write.blockSize1, left, right);
    }

private:
    /// Copies a contiguous run of frames from the inputs, starting at an offset in them, into the ring.
    void copyIn(
        const int ringIndex,
        const int frames,
        const int offset,
        const std::span<const float> left,
        const std::span<const float> right
    )
    {
        if (frames > 0) {
            buffer.copyFrom(0, ringIndex, left.subspan(static_cast<std::size_t>(offset)).data(), frames);
            buffer.copyFrom(1, ringIndex, right.subspan(static_cast<std::size_t>(offset)).data(), frames);
        }
    }

    /// Copies a contiguous run of frames from the ring into the outputs, starting at an offset in them.
    void copyOut(
        const int ringIndex,
        const int frames,
        const int offset,
        const std::span<float> left,
        const std::span<float> right
    ) const
    {
        if (frames > 0) {
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
        }
    }

    juce::AbstractFifo fifo {ANALYZER_FIFO_FRAMES};
    juce::AudioBuffer<float> buffer;
};
}  // namespace esgrove::image_control
