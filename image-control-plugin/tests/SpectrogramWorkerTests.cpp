/// Tests SpectrogramWorker on its own thread with a real analyzer ring and renderer, without an OpenGL context:
/// queued audio becomes particles for the renderer, the audio is discarded unanalyzed while analysis is off,
/// and a producer much faster than real time cannot keep the worker from finishing its passes.

#include "analysis/SpatialAnalyzer.h"
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

/// Waits for a completed worker pass rather than racing its SPSC read index with a second consumer.
bool finishPass(SpectrogramWorker& worker)
{
    const auto ticket = worker.requestPass();
    return wakeUntil(worker, [&worker, ticket] { return worker.completedPassCount() >= ticket; });
}
}  // namespace

TEST(SpectrogramWorker, QueuedAudioBecomesParticles)
{
    StereoSampleFifo fifo;
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    SpectrogramWorker worker(fifo, renderer);
    worker.setSettings({.positionCurve = StereoPositionCurve {}, .analyze = true});
    worker.start();
    ASSERT_TRUE(finishPass(worker));

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
    worker.setSettings({.positionCurve = StereoPositionCurve {}, .analyze = false});
    worker.start();
    ASSERT_TRUE(finishPass(worker));

    const auto samples = tone(QUEUED_FRAMES);
    fifo.push(samples, samples);
    ASSERT_TRUE(finishPass(worker));
    // Filling the entire usable capacity proves that the worker drained the previous block.
    const std::vector<float> full(static_cast<std::size_t>(ANALYZER_FIFO_FRAMES - 1), 0.0F);
    EXPECT_EQ(fifo.push(full, full), ANALYZER_FIFO_FRAMES - 1);
    ASSERT_TRUE(finishPass(worker));
    worker.stop();
    EXPECT_EQ(renderer.pendingParticleCount(), 0U);
}

TEST(SpectrogramWorker, SourceChangesDiscardQueuedAudioAndAnalyzerHistory)
{
    for (const bool changeRate : {false, true}) {
        SCOPED_TRACE(changeRate);
        StereoSampleFifo fifo;
        juce::OpenGLContext context;
        SpectrogramRenderer renderer(context);
        SpectrogramWorker worker(fifo, renderer);
        worker.start();
        ASSERT_TRUE(finishPass(worker));
        const auto samples = tone(QUEUED_FRAMES);
        fifo.push(samples, samples);
        ASSERT_TRUE(finishPass(worker));
        ASSERT_GT(renderer.pendingParticleCount(), 0U);

        // The queue and analyzer both contain the old source when the configuration changes.
        fifo.push(samples, samples);
        if (changeRate) {
            fifo.setSampleRate(96'000.0);
            worker.setSettings({.positionCurve = StereoPositionCurve {}, .sampleRate = 96'000.0});
        } else {
            fifo.setTap(false);
        }
        ASSERT_TRUE(finishPass(worker));
        EXPECT_EQ(renderer.pendingParticleCount(), 0U);
        const std::vector<float> silence(QUEUED_FRAMES, 0.0F);
        fifo.push(silence, silence);
        ASSERT_TRUE(finishPass(worker));
        EXPECT_EQ(renderer.pendingParticleCount(), 0U);
        fifo.push(samples, samples);
        ASSERT_TRUE(finishPass(worker));
        EXPECT_GT(renderer.pendingParticleCount(), 0U);
        worker.stop();
    }
}

TEST(SpectrogramWorker, ClearRejectsAlreadyQueuedAudio)
{
    StereoSampleFifo fifo;
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    SpectrogramWorker worker(fifo, renderer);
    worker.start();
    ASSERT_TRUE(finishPass(worker));
    const auto samples = tone(QUEUED_FRAMES);
    fifo.push(samples, samples);
    worker.requestClear();
    ASSERT_TRUE(finishPass(worker));
    EXPECT_EQ(renderer.pendingParticleCount(), 0U);
    worker.stop();
}

TEST(SpectrogramWorker, StopsWhileAProducerFloodsTheRing)
{
    // A producer far faster than real time, as in an offline render or pluginval,
    // must not keep the worker draining forever, or stopping it would hang.
    StereoSampleFifo fifo;
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    SpectrogramWorker worker(fifo, renderer);
    worker.setSettings({.positionCurve = StereoPositionCurve {}, .analyze = true});
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
