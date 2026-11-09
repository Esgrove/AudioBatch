/// Implements KernelSpectra and PartitionedConvolver.
/// The convolver uses uniformly partitioned overlap-save:
/// each input partition is transformed together with the previous one,
/// the spectra of the recent partitions are multiplied with the matching kernel partitions and summed,
/// and the second half of the inverse transform is the exact linear convolution of the partition.
/// Spectra are stored split, with the real parts of all bins followed by the imaginary parts,
/// so the multiply-add over the bins is plain lane-by-lane arithmetic
/// that NEON and AVX2 vectorize with fused multiply-adds.

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
/// Returns the FFT order for transforms of twice the partition size.
int fftOrderForPartition(const int partitionSize)
{
    return juce::roundToInt(std::log2(static_cast<double>(partitionSize) * 2.0));
}

/// Copies JUCE's interleaved spectrum, real and imaginary part per bin, into the split layout:
/// the real parts of all bins, then the imaginary parts.
void splitComplex(const std::span<const float> interleaved, const std::span<float> split)
{
    const std::size_t bins = split.size() / 2;
    for (std::size_t bin = 0; bin < bins; ++bin) {
        split[bin] = interleaved[bin * 2];
        split[bins + bin] = interleaved[bin * 2 + 1];
    }
}

/// Copies a split spectrum back into JUCE's interleaved layout.
void interleaveComplex(const std::span<const float> split, const std::span<float> interleaved)
{
    const std::size_t bins = split.size() / 2;
    for (std::size_t bin = 0; bin < bins; ++bin) {
        interleaved[bin * 2] = split[bin];
        interleaved[bin * 2 + 1] = split[bins + bin];
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

    binValues = (fftSize / 2 + 1) * 2;
    partitionCount = (kernelLength + partitionTaps - 1) / partitionTaps;
    spectra.assign(kernelCount * partitionCount * binValues, 0.0F);

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
            const auto destination = (kernel * partitionCount + partition) * binValues;
            splitComplex(
                std::span<const float>(workspace).first(binValues),
                std::span<float>(spectra).subspan(destination, binValues)
            );
        }
    }
}

/// Returns the split complex spectrum of one partition of one kernel: the real parts, then the imaginary parts.
std::span<const float> KernelSpectra::getPartition(const std::size_t kernel, const std::size_t partition) const
{
    return std::span<const float>(spectra).subspan((kernel * partitionCount + partition) * binValues, binValues);
}

/// Returns the number of kernels.
std::size_t KernelSpectra::getKernelCount() const
{
    return kernelCount;
}

/// Returns the number of partitions of each kernel.
std::size_t KernelSpectra::getPartitionCount() const
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
    binValues = (fftSize / 2 + 1) * 2;
    accumulator.assign(binValues, 0.0F);
    history.assign(partitionCount * binValues, 0.0F);
    inputWindow.assign(fftSize, 0.0F);
    workspace.assign(fftSize * 2, 0.0F);
    reset();
}

/// Clears the input history, as if the signal had been silent.
void PartitionedConvolver::reset()
{
    std::ranges::fill(history, 0.0F);
    std::ranges::fill(inputWindow, 0.0F);
    newestPartition = 0;
}

/// Adds the next partition of the input, which must hold exactly one partition of samples.
/// Transforms it together with the previous partition and stores the spectrum as the newest in the history.
void PartitionedConvolver::pushInput(const std::span<const float> input)
{
    jassert(input.size() == partitionSize);
    std::copy_n(inputWindow.begin() + static_cast<std::ptrdiff_t>(partitionSize), partitionSize, inputWindow.begin());
    std::ranges::copy(input, inputWindow.begin() + static_cast<std::ptrdiff_t>(partitionSize));

    std::ranges::copy(inputWindow, workspace.begin());
    std::fill(workspace.begin() + static_cast<std::ptrdiff_t>(inputWindow.size()), workspace.end(), 0.0F);
    fft->performRealOnlyForwardTransform(workspace.data(), true);

    newestPartition = (newestPartition + 1) % partitionCount;
    splitComplex(
        std::span<const float>(workspace).first(binValues),
        std::span<float>(history).subspan(newestPartition * binValues, binValues)
    );
}

/// Writes the convolution of the newest input partition with one kernel to the output, one partition long.
/// The output is the exact causal convolution with the kernel, without any latency of the convolver itself.
void PartitionedConvolver::convolve(const KernelSpectra& kernels, const std::size_t kernel, std::span<float> output)
{
    jassert(kernels.getPartitionCount() == partitionCount);
    jassert(output.size() == partitionSize);

    std::ranges::fill(accumulator, 0.0F);
    // In the hottest loop of the plugin, every span has the same length and the loop stays below it,
    // so the compiler can prove each access in range, drop the hardening checks,
    // and turn the split complex multiply-add into vector fused multiply-adds.
    // Cutting the spans to the length checks their sizes once per partition.
    const std::size_t bins = binValues / 2;
    const auto sumReal = std::span<float>(accumulator).first(bins);
    const auto sumImaginary = std::span<float>(accumulator).subspan(bins).first(bins);
    for (std::size_t partition = 0; partition < partitionCount; ++partition) {
        const std::size_t slot = (newestPartition + partitionCount - partition) % partitionCount;
        const auto input = std::span<const float>(history).subspan(slot * binValues, binValues);
        const auto taps = kernels.getPartition(kernel, partition);
        const auto inputReal = input.first(bins);
        const auto inputImaginary = input.subspan(bins).first(bins);
        const auto tapReal = taps.first(bins);
        const auto tapImaginary = taps.subspan(bins).first(bins);
        for (std::size_t bin = 0; bin < bins; ++bin) {
            sumReal[bin] += inputReal[bin] * tapReal[bin] - inputImaginary[bin] * tapImaginary[bin];
            sumImaginary[bin] += inputReal[bin] * tapImaginary[bin] + inputImaginary[bin] * tapReal[bin];
        }
    }

    interleaveComplex(accumulator, std::span<float>(workspace).first(binValues));
    std::fill(workspace.begin() + static_cast<std::ptrdiff_t>(binValues), workspace.end(), 0.0F);
    fft->performRealOnlyInverseTransform(workspace.data());
    std::copy_n(workspace.begin() + static_cast<std::ptrdiff_t>(partitionSize), partitionSize, output.begin());
}
}  // namespace esgrove::image_control
