/// Tests that PartitionedConvolver computes the same causal convolution as a direct time-domain sum,
/// for several kernels sharing one input history.

#include "dsp/PartitionedConvolver.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Returns the causal convolution of a signal with a kernel at one output sample.
double directConvolution(const std::vector<float>& signal, const std::vector<float>& kernel, const std::size_t sample)
{
    double sum = 0.0;
    for (std::size_t tap = 0; tap < kernel.size() && tap <= sample; ++tap) {
        sum += static_cast<double>(kernel[tap]) * static_cast<double>(signal[sample - tap]);
    }
    return sum;
}
}  // namespace

TEST(Convolver, MatchesDirectConvolutionForEveryKernel)
{
    constexpr int partitionSize = 64;
    const std::vector<std::vector<float>> kernels {
        makeNoise(0.1F, 1000, 1),
        makeNoise(0.1F, 1000, 2),
        makeNoise(0.1F, 1000, 3),
    };
    const KernelSpectra spectra(kernels, partitionSize);
    ASSERT_EQ(spectra.getKernelCount(), 3U);
    ASSERT_EQ(spectra.getPartitionCount(), 16U);

    PartitionedConvolver convolver;
    convolver.prepare(partitionSize, spectra.getPartitionCount());
    const auto input = makeNoise(1.0F, 4096, 4);
    std::vector<std::vector<float>> outputs(kernels.size(), std::vector<float>(input.size()));
    std::vector<float> partition(partitionSize);
    for (std::size_t start = 0; start < input.size(); start += partitionSize) {
        convolver.pushInput(std::span<const float>(input).subspan(start, partitionSize));
        for (std::size_t kernel = 0; kernel < kernels.size(); ++kernel) {
            convolver.convolve(spectra, kernel, partition);
            std::ranges::copy(partition, outputs[kernel].begin() + static_cast<std::ptrdiff_t>(start));
        }
    }

    for (std::size_t kernel = 0; kernel < kernels.size(); ++kernel) {
        for (std::size_t sample = 0; sample < input.size(); sample += 7) {
            ASSERT_NEAR(outputs[kernel][sample], directConvolution(input, kernels[kernel], sample), 1.0e-4)
                << "kernel " << kernel << " sample " << sample;
        }
    }
}

TEST(Convolver, PartitionBoundaryLengthsMatchEverySampleThroughTheTail)
{
    constexpr int partitionSize {32};
    const auto source = makeNoise(0.5F, partitionSize * 5 + 7, 91);
    for (const std::size_t length : {1U, 32U, 33U}) {
        SCOPED_TRACE(length);
        const std::vector<std::vector<float>> kernels {makeNoise(0.25F, length, 92)};
        const KernelSpectra spectra(kernels, partitionSize);
        PartitionedConvolver convolver;
        convolver.prepare(partitionSize, spectra.getPartitionCount());
        const std::size_t outputLength = source.size() + length - 1;
        const std::size_t paddedLength = (outputLength + partitionSize - 1) / partitionSize * partitionSize;
        std::vector<float> input(paddedLength, 0.0F);
        std::ranges::copy(source, input.begin());
        std::vector<float> output(partitionSize);
        for (std::size_t start = 0; start < paddedLength; start += partitionSize) {
            convolver.pushInput(std::span<const float>(input).subspan(start, partitionSize));
            convolver.convolve(spectra, 0, output);
            for (std::size_t sample = 0; sample < output.size(); ++sample) {
                EXPECT_NEAR(output[sample], directConvolution(input, kernels.front(), start + sample), 1.0e-6)
                    << start + sample;
            }
        }
    }
}

TEST(Convolver, ResetClearsHistory)
{
    constexpr int partitionSize = 32;
    const std::vector<std::vector<float>> kernels {makeNoise(0.5F, 100, 5)};
    const KernelSpectra spectra(kernels, partitionSize);
    PartitionedConvolver convolver;
    convolver.prepare(partitionSize, spectra.getPartitionCount());
    const auto noise = makeNoise(1.0F, partitionSize, 6);
    convolver.pushInput(noise);
    convolver.reset();

    const std::vector<float> silence(partitionSize, 0.0F);
    std::vector<float> output(partitionSize, 1.0F);
    convolver.pushInput(silence);
    convolver.convolve(spectra, 0, output);
    for (const float value : output) {
        EXPECT_NEAR(value, 0.0F, 1.0e-7F);
    }
}
}  // namespace esgrove::image_control::test
