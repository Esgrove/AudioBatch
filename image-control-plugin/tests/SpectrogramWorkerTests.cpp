/// Tests SpectrogramWorker on its own thread with a real analyzer ring and renderer, without an OpenGL context:
/// queued audio becomes particles for the renderer, the audio is discarded unanalyzed while analysis is off,
/// and a producer much faster than real time cannot keep the worker from finishing its passes.
/// Also tests the renderer's bounded pending-particle hand-off entirely on the CPU.

#include "analysis/SpatialAnalyzer.h"
#include "analysis/StereoSampleFifo.h"
#include "ui/SpectrogramParticles.h"
#include "ui/SpectrogramRenderer.h"
#include "ui/SpectrogramWorker.h"

#include <gtest/gtest.h>

#include <juce_core/juce_core.h>
#include <juce_opengl/juce_opengl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <mutex>
#include <numbers>
#include <span>
#include <thread>
#include <vector>

namespace esgrove::image_control::test
{
/// Asserts the CPU hand-off contract without attaching a context or calling any OpenGL entry points.
struct SpectrogramRendererTestAccess {
    /// Observes a renderer that outlives these CPU-only assertions.
    explicit SpectrogramRendererTestAccess(SpectrogramRenderer& rendererToTest) : renderer(rendererToTest) {}

    /// Leaves the test-owned renderer untouched when this non-owning accessor goes out of scope.
    ~SpectrogramRendererTestAccess() = default;

    SpectrogramRendererTestAccess(const SpectrogramRendererTestAccess&) = delete;
    SpectrogramRendererTestAccess(SpectrogramRendererTestAccess&&) = delete;
    SpectrogramRendererTestAccess& operator=(const SpectrogramRendererTestAccess&) = delete;
    SpectrogramRendererTestAccess& operator=(SpectrogramRendererTestAccess&&) = delete;

    /// Checks the entire retained sequence and clear flag while respecting the pending-work mutex.
    void expectPending(const std::span<const SpectrogramParticle> expected, const bool clear) const
    {
        const std::scoped_lock lock(renderer.pendingMutex);
        EXPECT_TRUE(
            std::ranges::equal(
                renderer.pending.particles,
                expected,
                [](const SpectrogramParticle& actual, const SpectrogramParticle& wanted) {
                    return juce::approximatelyEqual(actual.x, wanted.x);
                }
            )
        );
        EXPECT_EQ(renderer.pending.clear, clear);
    }

    /// Renderer owned by the test, which outlives this accessor.
    SpectrogramRenderer& renderer;
};

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

/// Builds distinct CPU-only markers so the assertions distinguish oldest from newest retained particles.
std::vector<SpectrogramParticle> numberedParticles(const std::size_t count)
{
    std::vector<SpectrogramParticle> particles(count);
    for (std::size_t index = 0; index < count; ++index) {
        particles.at(index).x = static_cast<float>(index);
    }
    return particles;
}

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

TEST(SpectrogramRendererCpu, EmptySubmissionsPreserveWaitingParticles)
{
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    const SpectrogramRendererTestAccess access(renderer);
    renderer.submitParticles({}, false);
    EXPECT_EQ(renderer.pendingParticleCount(), 0U);
    access.expectPending({}, false);

    const auto particles = numberedParticles(3);
    renderer.submitParticles(particles, false);
    renderer.submitParticles({}, false);
    EXPECT_EQ(renderer.pendingParticleCount(), particles.size());
    access.expectPending(particles, false);
    EXPECT_FALSE(renderer.isUnavailable());
    EXPECT_FALSE(renderer.gpuFrameMilliseconds().has_value());
}

TEST(SpectrogramRendererCpu, ExactCapPreservesEveryWaitingParticle)
{
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    const SpectrogramRendererTestAccess access(renderer);
    const auto particles = numberedParticles(SPECTROGRAM_MAX_PENDING_PARTICLES);
    const std::span<const SpectrogramParticle> batch(particles);
    renderer.submitParticles(batch.first(batch.size() - 1), false);
    renderer.submitParticles(batch.last(1), false);
    renderer.submitParticles({}, false);
    EXPECT_EQ(renderer.pendingParticleCount(), SPECTROGRAM_MAX_PENDING_PARTICLES);
    access.expectPending(particles, false);
}

TEST(SpectrogramRendererCpu, RepeatedOverflowReplacesOlderWaitingBatches)
{
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    const SpectrogramRendererTestAccess access(renderer);
    const auto particles = numberedParticles(SPECTROGRAM_MAX_PENDING_PARTICLES);
    const auto newest = std::span<const SpectrogramParticle>(particles).last(3);
    for (int cycle = 0; cycle < 3; ++cycle) {
        SCOPED_TRACE(cycle);
        renderer.submitParticles(particles, false);
        EXPECT_EQ(renderer.pendingParticleCount(), SPECTROGRAM_MAX_PENDING_PARTICLES);
        access.expectPending(particles, false);
        renderer.submitParticles(newest, false);
        EXPECT_EQ(renderer.pendingParticleCount(), newest.size());
        access.expectPending(newest, false);
    }
}

TEST(SpectrogramRendererCpu, OversizedSubmissionRetainsOnlyItsNewestCappedTail)
{
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    const SpectrogramRendererTestAccess access(renderer);
    const auto particles = numberedParticles(SPECTROGRAM_MAX_PENDING_PARTICLES + 17);
    const std::span<const SpectrogramParticle> batch(particles);
    renderer.submitParticles(batch.first(3), false);
    renderer.submitParticles(batch, false);
    EXPECT_EQ(renderer.pendingParticleCount(), SPECTROGRAM_MAX_PENDING_PARTICLES);
    access.expectPending(batch.last(SPECTROGRAM_MAX_PENDING_PARTICLES), false);
}

TEST(SpectrogramRendererCpu, ClearDropsWaitingParticlesAndRemainsPending)
{
    juce::OpenGLContext context;
    SpectrogramRenderer renderer(context);
    const SpectrogramRendererTestAccess access(renderer);
    const auto particles = numberedParticles(8);
    const std::span<const SpectrogramParticle> batch(particles);
    renderer.submitParticles(batch, false);
    renderer.submitParticles(batch.last(2), true);
    EXPECT_EQ(renderer.pendingParticleCount(), 2U);
    access.expectPending(batch.last(2), true);

    renderer.submitParticles(batch.last(1), false);
    const std::array<SpectrogramParticle, 3> afterClear {particles[6], particles[7], particles[7]};
    access.expectPending(afterClear, true);
    renderer.submitParticles({}, true);
    EXPECT_EQ(renderer.pendingParticleCount(), 0U);
    access.expectPending({}, true);
    renderer.submitParticles({}, false);
    access.expectPending({}, true);
}

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
