/// Declares KernelSpectra and PartitionedConvolver, a uniformly partitioned overlap-save FIR convolution.
/// KernelSpectra holds the partition spectra of several kernels of the same length,
/// computed once off the audio thread.
/// PartitionedConvolver keeps the spectra of the recent input partitions of one signal,
/// so the same input can be convolved with every kernel at the cost of one forward FFT per partition.

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace esgrove::image_control
{
/// The frequency-domain partitions of several FIR kernels, ready for PartitionedConvolver.
/// Each kernel is cut into partitions of partitionSize taps,
/// and each partition is zero padded to twice its size and transformed.
class KernelSpectra
{
public:
    KernelSpectra() = default;
    KernelSpectra(std::span<const std::vector<float>> kernels, int partitionSize);

    [[nodiscard]] std::span<const float> getPartition(std::size_t kernel, std::size_t partition) const;

    [[nodiscard]] std::size_t getKernelCount() const;
    [[nodiscard]] std::size_t getPartitionCount() const;

private:
    /// Split complex spectrum of every partition of every kernel, kernel by kernel:
    /// the real parts of a partition's bins followed by their imaginary parts.
    std::vector<float> spectra;

    /// Floats in one partition spectrum: twice the number of non-negative frequency bins.
    std::size_t binValues {0};
    /// Number of kernels.
    std::size_t kernelCount {0};
    /// Number of partitions of each kernel.
    std::size_t partitionCount {0};
};

/// Convolves one signal with the kernels of a KernelSpectra, partition by partition, without latency of its own.
/// Push each input partition once, then convolve it with any number of kernels.
/// Every buffer is allocated in prepare, so pushing and convolving are safe in the audio callback.
class PartitionedConvolver
{
public:
    void convolve(const KernelSpectra& kernels, std::size_t kernel, std::span<float> output);
    void prepare(int samplesPerPartition, std::size_t kernelPartitions);
    void pushInput(std::span<const float> input);
    void reset();

private:
    /// Transform of twice the partition size, created in prepare.
    std::unique_ptr<juce::dsp::FFT> fft;

    /// Accumulates the split complex spectrum of one kernel's output.
    std::vector<float> accumulator;
    /// Split complex spectra of the most recent input partitions, as a ring with the newest at newestPartition.
    std::vector<float> history;
    /// The previous and the current input partition, which overlap-save transforms together.
    std::vector<float> inputWindow;
    /// FFT input and output, twice the FFT size as JUCE requires.
    std::vector<float> workspace;

    /// Floats in one partition spectrum: twice the number of non-negative frequency bins.
    std::size_t binValues {0};
    /// Ring index of the newest input partition spectrum.
    std::size_t newestPartition {0};
    /// Number of partitions of the kernels, and of input spectra kept.
    std::size_t partitionCount {0};
    /// Samples per partition.
    std::size_t partitionSize {0};
};
}  // namespace esgrove::image_control
