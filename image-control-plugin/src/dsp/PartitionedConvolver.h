/// Declares KernelSpectra and PartitionedConvolver, a uniformly partitioned overlap-save FIR convolution.
/// KernelSpectra holds the partition spectra of several kernels of the same length,
/// computed during preparation, background design, or explicitly non-real-time processing.
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

    [[nodiscard]] std::size_t getComponentStride() const noexcept;
    [[nodiscard]] std::size_t getKernelCount() const noexcept;
    [[nodiscard]] std::size_t getPartitionCount() const noexcept;

private:
    /// Split complex spectrum of every partition of every kernel, kernel by kernel:
    /// the padded real parts of a partition's bins followed by their padded imaginary parts.
    std::vector<float> spectra;

    /// Padded floats per real or imaginary component array, rounded up to an 8-float SIMD multiple.
    std::size_t componentStride {0};
    /// Number of kernels.
    std::size_t kernelCount {0};
    /// Number of partitions of each kernel.
    std::size_t partitionCount {0};
};

/// Convolves one signal with the kernels of a KernelSpectra, partition by partition, without latency of its own.
/// Push every input partition once, even when no output is needed, then convolve it with any number of kernels.
/// Output calls only read history, so resuming a skipped output immediately uses the retained raw input.
/// Every buffer is allocated in prepare, so pushing and convolving are safe in the audio callback.
class PartitionedConvolver
{
public:
    void convolve(const KernelSpectra& kernels, std::size_t kernel, std::span<float> output);
    void prepare(int samplesPerPartition, std::size_t kernelPartitions);
    void pushInput(std::span<const float> input);
    void reset() noexcept;

private:
    /// Transform of twice the partition size, created in prepare.
    std::unique_ptr<juce::dsp::FFT> fft;

    /// Accumulates the real part of one kernel's output spectrum.
    std::vector<float> accumulatorReal;
    /// Accumulates the imaginary part of one kernel's output spectrum.
    std::vector<float> accumulatorImaginary;
    /// Split complex spectra of the most recent input partitions, as a ring with the newest at newestPartition.
    std::vector<float> history;
    /// Previous input partition, transformed ahead of the incoming partition for overlap-save.
    std::vector<float> previousInput;
    /// FFT input and output, twice the FFT size as JUCE requires.
    std::vector<float> workspace;

    /// Non-negative frequency bins in one partition spectrum.
    std::size_t binCount {0};
    /// Padded floats per real or imaginary component array, rounded up to an 8-float SIMD multiple.
    std::size_t componentStride {0};
    /// Ring index of the newest input partition spectrum.
    std::size_t newestPartition {0};
    /// Number of partitions of the kernels, and of input spectra kept.
    std::size_t partitionCount {0};
    /// Samples per partition.
    std::size_t partitionSize {0};
};
}  // namespace esgrove::image_control
