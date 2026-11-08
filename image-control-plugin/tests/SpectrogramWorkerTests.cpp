/// Tests SpectrogramWorker on its own thread with a real analyzer ring and renderer, without an OpenGL context:
/// queued audio becomes particles for the renderer, the audio is discarded unanalyzed while analysis is off,
/// and a producer much faster than real time cannot keep the worker from finishing its passes.

#include "analysis/StereoSampleFifo.h"
#include "ui/SpectrogramRenderer.h"
#include "ui/SpectrogramWorker.h"

#include <gtest/gtest.h>

#include <juce_core/juce_core.h>
#include <juce_opengl/juce_opengl.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numbers>
#include <thread>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Frames of audio queued in the tests, a sixth of a second at 48 kHz.
constexpr std::size_t QUEUED_FRAMES {8192};
/// Frequency of the test tone, in Hz.
constexpr double TONE_FREQUENCY {1000.0};
/// Amplitude of the test tone.
constexpr float TONE_AMPLITUDE {0.5F};
/// Longest wait for the worker in the tests.
constexpr std::chrono::seconds WAIT_LIMIT {5};
/// Time the worker gets to discard the audio that queued before it started.
constexpr std::chrono::milliseconds STARTUP_TIME {200};

/// Returns a sine tone at the analyzer's default sample rate.
std::vector<float> tone(const std::size_t frames)
{
    std::vector<float> samples(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        samples[frame] = TONE_AMPLITUDE
            * static_cast<float>(std::sin(
                2.0 * std::numbers::pi * TONE_FREQUENCY * static_cast<double>(frame) / ANALYZER_DEFAULT_SAMPLE_RATE
            ));
    }
    return samples;
}

/// Wakes the worker until a condition holds or the wait limit passes, and returns whether it held.
bool wakeUntil(SpectrogramWorker& worker, const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
    while (std::chrono::steady_clock::now() < deadline) {
        worker.wake();
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

/// Returns whether the ring is empty, leaving it so if it is. Pops at most one frame.
bool ringIsEmpty(StereoSampleFifo& fifo)
{
    std::vector<float> left(1);
    std::vector<float> right(1);
    return fifo.pop(left, right) == 0;
}
}  // namespace

TEST(SpectrogramWorker, QueuedAudioBecomesParticles)
{
    StereoSampleFifo fifo;
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    SpectrogramWorker worker(fifo, renderer);
    worker.setSettings({.analyze = true});
    worker.start();
    std::this_thread::sleep_for(STARTUP_TIME);

    const auto samples = tone(QUEUED_FRAMES);
    fifo.push(samples, samples);
    EXPECT_TRUE(wakeUntil(worker, [&renderer] { return renderer.pendingParticleCount() > 0; }));
    worker.stop();
}

TEST(SpectrogramWorker, AudioIsDiscardedWhileAnalysisIsOff)
{
    StereoSampleFifo fifo;
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    SpectrogramWorker worker(fifo, renderer);
    worker.setSettings({.analyze = false});
    worker.start();
    std::this_thread::sleep_for(STARTUP_TIME);

    const auto samples = tone(QUEUED_FRAMES);
    fifo.push(samples, samples);
    EXPECT_TRUE(wakeUntil(worker, [&fifo] { return ringIsEmpty(fifo); }));
    worker.stop();
    EXPECT_EQ(renderer.pendingParticleCount(), 0U);
}

TEST(SpectrogramWorker, StopsWhileAProducerFloodsTheRing)
{
    // A producer far faster than real time, as in an offline render or pluginval,
    // must not keep the worker draining forever, or stopping it would hang.
    StereoSampleFifo fifo;
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    SpectrogramWorker worker(fifo, renderer);
    worker.setSettings({.analyze = true});
    worker.start();

    std::atomic<bool> producing {true};
    std::thread producer([&fifo, &producing] {
        const auto samples = tone(QUEUED_FRAMES);
        while (producing) {
            fifo.push(samples, samples);
        }
    });
    for (int pass = 0; pass < 20; ++pass) {
        worker.wake();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto start = std::chrono::steady_clock::now();
    worker.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start, WAIT_LIMIT);
    producing = false;
    producer.join();
}
}  // namespace esgrove::image_control::test
