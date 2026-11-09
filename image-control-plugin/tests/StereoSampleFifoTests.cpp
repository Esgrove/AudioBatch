/// Tests the analyzer SPSC ring's full/drop and wrap ordering, source epoch boundaries,
/// and concurrent publication of stereo samples and their metadata.

#include "analysis/StereoSampleFifo.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace esgrove::image_control::test
{
TEST(StereoSampleFifo, FullQueueDropsNewestFramesAndWrapsInOrder)
{
    StereoSampleFifo fifo;
    const auto capacity = static_cast<std::size_t>(ANALYZER_FIFO_FRAMES - 1);
    std::vector<float> samples(capacity + 100);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = static_cast<float>(index);
    }
    const auto epoch = fifo.getEpoch();
    EXPECT_EQ(fifo.push(samples, samples, epoch), static_cast<int>(capacity));
    EXPECT_EQ(fifo.push(samples, samples, epoch), 0);
    std::vector<float> left(1000);
    std::vector<float> right(1000);
    std::vector<AnalyzerEpoch> epochs(1000);
    ASSERT_EQ(fifo.pop(left, right, epochs), 1000);
    for (std::size_t index = 0; index < left.size(); ++index) {
        EXPECT_EQ(left[index], static_cast<float>(index));
        EXPECT_EQ(right[index], left[index]);
        EXPECT_EQ(epochs[index], epoch);
    }
    EXPECT_EQ(fifo.push(samples, samples), 1000);
    left.resize(capacity);
    right.resize(capacity);
    epochs.resize(capacity);
    ASSERT_EQ(fifo.pop(left, right, epochs), static_cast<int>(capacity));
    for (std::size_t index = 0; index < capacity; ++index) {
        const auto expected = index < capacity - 1000 ? index + 1000 : index - (capacity - 1000);
        EXPECT_EQ(left[index], static_cast<float>(expected));
        EXPECT_EQ(right[index], left[index]);
        EXPECT_EQ(epochs[index], epoch);
    }
    EXPECT_EQ(fifo.pop(left, right), 0);
}

TEST(StereoSampleFifo, SourceBoundariesKeepMetadataAndRejectLateBlocks)
{
    StereoSampleFifo fifo;
    const std::vector<float> samples(32, 1.0F);
    const auto output = fifo.getEpoch();
    ASSERT_TRUE(StereoSampleFifo::analyzesOutput(output));
    ASSERT_EQ(fifo.push(samples, samples, output), 32);
    fifo.setTap(false);
    const auto input = fifo.getEpoch();
    EXPECT_NE(input, output);
    EXPECT_FALSE(StereoSampleFifo::analyzesOutput(input));
    EXPECT_EQ(fifo.push(samples, samples, output), 0);
    ASSERT_EQ(fifo.push(samples, samples, input), 32);
    fifo.setSampleRate(96'000.0);
    const auto highRate = fifo.getEpoch();
    EXPECT_NE(input, highRate);
    EXPECT_DOUBLE_EQ(fifo.getSampleRate(), 96'000.0);
    EXPECT_FALSE(StereoSampleFifo::analyzesOutput(highRate));
    EXPECT_EQ(fifo.push(samples, samples, input), 0);
    ASSERT_EQ(fifo.push(samples, samples, highRate), 32);
    fifo.beginEpoch();
    EXPECT_NE(fifo.getEpoch(), highRate);
    EXPECT_EQ(fifo.push(samples, samples, highRate), 0);
    std::vector<float> left(96);
    std::vector<float> right(96);
    std::vector<AnalyzerEpoch> epochs(96);
    ASSERT_EQ(fifo.pop(left, right, epochs), 96);
    for (std::size_t index = 0; index < epochs.size(); ++index) {
        EXPECT_EQ(epochs[index], index < 32 ? output : (index < 64 ? input : highRate));
    }
}

TEST(StereoSampleFifo, ConcurrentConsumerSeesOrderedStereoAndEpochsAcrossWraps)
{
    StereoSampleFifo fifo;
    constexpr std::size_t frames = 250'000;
    constexpr std::size_t chunk = 257;
    std::atomic<bool> finished {false};
    std::atomic<bool> failed {false};
    std::atomic<std::size_t> consumed {0};
    std::thread consumer([&] {
        std::vector<float> left(chunk);
        std::vector<float> right(chunk);
        std::vector<AnalyzerEpoch> epochs(chunk);
        std::size_t received = 0;
        float previous = -1.0F;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const bool producerFinished = finished.load(std::memory_order_acquire);
            const auto count = fifo.pop(left, right, epochs);
            for (int frame = 0; frame < count; ++frame) {
                const auto index = static_cast<std::size_t>(frame);
                const auto epoch = epochs[index];
                if (!(left[index] > previous) || !juce::approximatelyEqual(right[index], -left[index])
                    || !StereoSampleFifo::isStable(epoch)
                    || StereoSampleFifo::analyzesOutput(epoch) != (static_cast<int>(left[index]) % 2 == 0))
                {
                    failed.store(true);
                }
                previous = left[index];
                ++received;
            }
            if (count == 0 && producerFinished) {
                break;
            }
            std::this_thread::yield();
        }
        consumed.store(received);
        if (received == 0 || !finished.load(std::memory_order_acquire)) {
            failed.store(true);
        }
    });
    std::vector<float> left(1);
    std::vector<float> right(1);
    std::size_t accepted = 0;
    for (std::size_t index = 0; index < frames; ++index) {
        fifo.setTap(index % 2 == 0);
        left[0] = static_cast<float>(index);
        right[0] = -left[0];
        accepted += static_cast<std::size_t>(fifo.push(left, right, fifo.getEpoch()));
    }
    finished.store(true, std::memory_order_release);
    consumer.join();
    EXPECT_FALSE(failed.load());
    EXPECT_EQ(consumed.load(), accepted);
}
}  // namespace esgrove::image_control::test
