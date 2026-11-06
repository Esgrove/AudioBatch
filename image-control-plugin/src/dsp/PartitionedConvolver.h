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
    /// Interleaved complex bins of every partition of every kernel, kernel by kernel.
    std::vector<float> spectra;

    std::size_t binValues {0};
    std::size_t kernelCount {0};
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
    std::unique_ptr<juce::dsp::FFT> fft;

    /// Accumulates the spectrum of one kernel's output.
    std::vector<float> accumulator;
    /// Spectra of the most recent input partitions, as a ring with the newest at newestPartition.
    std::vector<float> history;
    /// The previous and the current input partition, which overlap-save transforms together.
    std::vector<float> inputWindow;
    /// FFT input and output, twice the FFT size as JUCE requires.
    std::vector<float> workspace;

    std::size_t binValues {0};
    std::size_t newestPartition {0};
    std::size_t partitionCount {0};
    std::size_t partitionSize {0};
};
}  // namespace esgrove::image_control
