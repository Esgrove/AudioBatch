/// Implements KernelSpectra and PartitionedConvolver.
/// The convolver uses uniformly partitioned overlap-save:
/// each input partition is transformed together with the previous one,
/// the spectra of the recent partitions are multiplied with the matching kernel partitions and summed,
/// and the second half of the inverse transform gives the causal FIR output, subject to floating-point rounding.
/// Spectra are stored split, with the real parts of all bins followed by the imaginary parts,
/// so the multiply-add over the bins is lane-by-lane arithmetic suitable for compiler vectorization.

#include "dsp/PartitionedConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Alignment granularity in floats so each split real and imaginary plane stays 32-byte SIMD-aligned.
constexpr std::size_t SIMD_FLOAT_LANES {8};

/// Rounds a bin count up to the next multiple of SIMD_FLOAT_LANES.
constexpr std::size_t paddedComponentStride(const std::size_t bins) noexcept
{
    return (bins + SIMD_FLOAT_LANES - 1) & ~(SIMD_FLOAT_LANES - 1);
}

/// Returns the FFT order for transforms of twice the partition size.
int fftOrderForPartition(const int partitionSize)
{
    return juce::roundToInt(std::log2(static_cast<double>(partitionSize) * 2.0));
}

/// Copies JUCE's interleaved spectrum into zero-padded split planes: real bins, then imaginary bins.
void splitComplex(
    const std::span<const float> interleaved,
    const std::span<float> split,
    const std::size_t bins,
    const std::size_t stride
)
{
    const auto real = split.first(stride);
    const auto imaginary = split.subspan(stride, stride);
    for (std::size_t bin = 0; bin < bins; ++bin) {
        real[bin] = interleaved[bin * 2];
        imaginary[bin] = interleaved[bin * 2 + 1];
    }
    std::fill(real.begin() + static_cast<std::ptrdiff_t>(bins), real.end(), 0.0F);
    std::fill(imaginary.begin() + static_cast<std::ptrdiff_t>(bins), imaginary.end(), 0.0F);
}

/// Copies split real and imaginary bin planes back into JUCE's interleaved layout.
void interleaveComplex(
    const std::span<const float> real,
    const std::span<const float> imaginary,
    const std::span<float> interleaved,
    const std::size_t bins
)
{
    for (std::size_t bin = 0; bin < bins; ++bin) {
        interleaved[bin * 2] = real[bin];
        interleaved[bin * 2 + 1] = imaginary[bin];
    }
}
}  // namespace

/// Transforms every partition of every kernel.
/// All kernels must have the same length, and the partition size must be a power of two.
/// Allocates during preparation or kernel design, never in a real-time processing callback.
KernelSpectra::KernelSpectra(const std::span<const std::vector<float>> kernels, const int partitionSize) :
    kernelCount(kernels.size())
{
    const auto partitionTaps = static_cast<std::size_t>(partitionSize);
    const juce::dsp::FFT fft(fftOrderForPartition(partitionSize));
    const auto fftSize = static_cast<std::size_t>(fft.getSize());
    const std::size_t kernelLength = kernels.empty() ? 0 : kernels.front().size();

    const std::size_t bins = fftSize / 2 + 1;
    componentStride = paddedComponentStride(bins);
    const std::size_t partitionValues = componentStride * 2;
    partitionCount = (kernelLength + partitionTaps - 1) / partitionTaps;
    spectra.assign(kernelCount * partitionCount * partitionValues, 0.0F);

    std::vector<float> workspace(fftSize * 2);
    for (std::size_t kernel = 0; kernel < kernelCount; ++kernel) {
        const auto& taps = kernels[kernel];
        jassert(taps.size() == kernelLength);
        for (std::size_t partition = 0; partition < partitionCount; ++partition) {
            std::ranges::fill(workspace, 0.0F);
            const std::size_t first = partition * partitionTaps;
            const std::size_t count = std::min(partitionTaps, kernelLength - first);
            std::copy_n(taps.begin() + static_cast<std::ptrdiff_t>(first), count, workspace.begin());
            fft.performRealOnlyForwardTransform(workspace.data(), true);
            const auto destination = (kernel * partitionCount + partition) * partitionValues;
            splitComplex(
                std::span<const float>(workspace).first(bins * 2),
                std::span<float>(spectra).subspan(destination, partitionValues),
                bins,
                componentStride
            );
        }
    }
}

/// Returns the padded split complex spectrum of one partition of one kernel: real plane, then imaginary plane.
std::span<const float> KernelSpectra::getPartition(const std::size_t kernel, const std::size_t partition) const
{
    const std::size_t partitionValues = componentStride * 2;
    return std::span<const float>(spectra).subspan(
        (kernel * partitionCount + partition) * partitionValues, partitionValues
    );
}

/// Returns the padded float stride of each real or imaginary component plane.
std::size_t KernelSpectra::getComponentStride() const noexcept
{
    return componentStride;
}

/// Returns the number of kernels.
std::size_t KernelSpectra::getKernelCount() const noexcept
{
    return kernelCount;
}

/// Returns the number of partitions of each kernel.
std::size_t KernelSpectra::getPartitionCount() const noexcept
{
    return partitionCount;
}

/// Allocates the buffers for a power-of-two partition size and kernels of the given number of partitions,
/// and clears the input history.
void PartitionedConvolver::prepare(const int samplesPerPartition, const std::size_t kernelPartitions)
{
    fft = std::make_unique<juce::dsp::FFT>(fftOrderForPartition(samplesPerPartition));
    const auto fftSize = static_cast<std::size_t>(fft->getSize());
    partitionSize = static_cast<std::size_t>(samplesPerPartition);
    partitionCount = kernelPartitions;
    binCount = fftSize / 2 + 1;
    componentStride = paddedComponentStride(binCount);
    accumulatorReal.assign(componentStride, 0.0F);
    accumulatorImaginary.assign(componentStride, 0.0F);
    history.assign(partitionCount * componentStride * 2, 0.0F);
    previousInput.assign(partitionSize, 0.0F);
    workspace.assign(fftSize * 2, 0.0F);
    reset();
}

/// Clears the input history, as if the signal had been silent.
void PartitionedConvolver::reset() noexcept
{
    std::ranges::fill(history, 0.0F);
    std::ranges::fill(previousInput, 0.0F);
    newestPartition = 0;
}

/// Adds the next partition of the input, which must hold exactly one partition of samples.
/// Transforms it together with the previous partition and stores the spectrum as the newest in the history.
void PartitionedConvolver::pushInput(const std::span<const float> input)
{
    jassert(input.size() == partitionSize);
    std::ranges::copy(previousInput, workspace.begin());
    std::ranges::copy(input, workspace.begin() + static_cast<std::ptrdiff_t>(partitionSize));
    std::ranges::copy(input, previousInput.begin());
    std::fill(workspace.begin() + static_cast<std::ptrdiff_t>(partitionSize * 2), workspace.end(), 0.0F);
    fft->performRealOnlyForwardTransform(workspace.data(), true);

    if (partitionCount == 0) {
        return;
    }
    newestPartition = newestPartition + 1 == partitionCount ? 0 : newestPartition + 1;
    const std::size_t partitionValues = componentStride * 2;
    splitComplex(
        std::span<const float>(workspace).first(binCount * 2),
        std::span<float>(history).subspan(newestPartition * partitionValues, partitionValues),
        binCount,
        componentStride
    );
}

/// Writes the convolution of the newest input partition with one kernel to the output, one partition long.
/// The output is the causal convolution with the kernel, subject to FFT rounding and without added latency.
/// This reads retained input spectra without advancing history, so skipped outputs need no warmup when resumed.
void PartitionedConvolver::convolve(const KernelSpectra& kernels, const std::size_t kernel, std::span<float> output)
{
    jassert(kernels.getPartitionCount() == partitionCount);
    jassert(kernels.getComponentStride() == componentStride);
    jassert(output.size() == partitionSize);

    std::ranges::fill(accumulatorReal, 0.0F);
    std::ranges::fill(accumulatorImaginary, 0.0F);
    const std::size_t stride = componentStride;
    const std::size_t partitionValues = stride * 2;
    const auto sumReal = std::span<float>(accumulatorReal).first(stride);
    const auto sumImaginary = std::span<float>(accumulatorImaginary).first(stride);
    std::size_t slot = newestPartition;
    for (std::size_t partition = 0; partition < partitionCount; ++partition) {
        const auto input = std::span<const float>(history).subspan(slot * partitionValues, partitionValues);
        const auto taps = kernels.getPartition(kernel, partition);
        const auto inputReal = input.first(stride);
        const auto inputImaginary = input.subspan(stride, stride);
        const auto tapReal = taps.first(stride);
        const auto tapImaginary = taps.subspan(stride, stride);
        for (std::size_t bin = 0; bin < stride; ++bin) {
            sumReal[bin] += inputReal[bin] * tapReal[bin] - inputImaginary[bin] * tapImaginary[bin];
            sumImaginary[bin] += inputReal[bin] * tapImaginary[bin] + inputImaginary[bin] * tapReal[bin];
        }
        slot = slot == 0 ? partitionCount - 1 : slot - 1;
    }

    const std::size_t interleavedValues = binCount * 2;
    interleaveComplex(sumReal, sumImaginary, std::span<float>(workspace).first(interleavedValues), binCount);
    std::fill(workspace.begin() + static_cast<std::ptrdiff_t>(interleavedValues), workspace.end(), 0.0F);
    fft->performRealOnlyInverseTransform(workspace.data());
    std::copy_n(workspace.begin() + static_cast<std::ptrdiff_t>(partitionSize), partitionSize, output.begin());
}
}  // namespace esgrove::image_control
