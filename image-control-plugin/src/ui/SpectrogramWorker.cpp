/// Implements SpectrogramWorker: the thread loop, handing settings and clears from the editor to the worker,
/// and draining, analyzing, and turning the queued audio into particles for the renderer.

#include "ui/SpectrogramWorker.h"

#include "analysis/SpatialAnalyzer.h"
#include "analysis/StereoSampleFifo.h"
#include "ui/SpectrogramParticles.h"
#include "ui/SpectrogramRenderer.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

namespace esgrove::image_control
{
namespace
{
/// Frames drained from the analyzer ring per pop.
constexpr std::size_t DRAIN_CHUNK {4096};
/// Longest wait between two passes when no display refresh wakes the worker, in milliseconds,
/// so the ring keeps draining while the editor's refreshes stall.
constexpr int WAKE_TIMEOUT_MILLISECONDS {50};
/// Longest wait for the thread to finish when the worker stops, in milliseconds.
constexpr int STOP_TIMEOUT_MILLISECONDS {2000};
}  // namespace

/// Creates the worker for the processor's analyzer ring and the renderer. The thread starts with start.
SpectrogramWorker::SpectrogramWorker(StereoSampleFifo& fifo, SpectrogramRenderer& renderer) :
    juce::Thread("ImageControl spectrogram analysis"),
    analyzerFifo(fifo),
    spectrogramRenderer(renderer),
    leftScratch(DRAIN_CHUNK, 0.0F),
    rightScratch(DRAIN_CHUNK, 0.0F)
{
    analyzer.setFrameCallback([this](const std::span<const SpectralPoint> points, const double frameSeconds) {
        particleBuilder.addFrame(points, frameSeconds);
    });
}

/// Stops the thread before the analyzer and the buffers it uses go away.
SpectrogramWorker::~SpectrogramWorker()
{
    stop();
}

/// Asks the worker to clear the analysis and the picture before its next particles,
/// such as after switching the analyzer tap.
void SpectrogramWorker::requestClear() noexcept
{
    requestedClears.fetch_add(1, std::memory_order_relaxed);
}

/// Hands the latest settings to the worker, which applies them on its next pass.
void SpectrogramWorker::setSettings(const SpectrogramAnalysisSettings& newSettings)
{
    const std::scoped_lock lock(settingsMutex);
    settings = newSettings;
}

/// Starts the thread at normal priority. It never touches the audio thread, which only fills the ring.
void SpectrogramWorker::start()
{
    startThread(juce::Thread::Priority::normal);
}

/// Stops the thread and waits for it to finish its pass.
void SpectrogramWorker::stop()
{
    signalThreadShouldExit();
    notify();
    stopThread(STOP_TIMEOUT_MILLISECONDS);
}

/// Wakes the worker for a pass, once per display refresh.
void SpectrogramWorker::wake()
{
    notify();
}

/// Discards the audio that queued while no editor was open, then runs a pass whenever it is woken.
void SpectrogramWorker::run()
{
    drainQueuedAudio(false);
    while (!threadShouldExit()) {
        wait(WAKE_TIMEOUT_MILLISECONDS);
        if (threadShouldExit()) {
            return;
        }
        processQueuedAudio();
    }
}

/// Applies the latest settings and clears, analyzes the queued audio, and hands the particles to the renderer.
void SpectrogramWorker::processQueuedAudio()
{
    SpectrogramAnalysisSettings current;
    {
        const std::scoped_lock lock(settingsMutex);
        current = settings;
    }
    bool clear = false;
    if (!juce::approximatelyEqual(current.sampleRate, preparedSampleRate)) {
        preparedSampleRate = current.sampleRate;
        analyzer.prepare(current.sampleRate);
        clear = true;
    }
    const std::uint32_t clears = requestedClears.load(std::memory_order_relaxed);
    if (clears != appliedClears) {
        appliedClears = clears;
        analyzer.clear();
        clear = true;
    }
    analyzer.setFocus(current.focusDecibels);
    particleBuilder.setFrequencyScale(current.scale);
    particleBuilder.setColours(current.colours);
    particleBuilder.setPersistence(current.persistenceSeconds);

    particleBuilder.clear();
    drainQueuedAudio(current.analyze);
    if (clear || !particleBuilder.particles().empty()) {
        spectrogramRenderer.submitParticles(particleBuilder.particles(), clear);
    }
}

/// Empties the processor's analyzer ring, passing the audio to the analyzer when analyze is set.
/// One pass takes at most a ring's worth of audio.
/// A producer that runs faster than real time, such as an offline render or pluginval,
/// would otherwise refill the ring as fast as it drains and keep the worker in this loop forever.
/// What arrives during the drain waits for the next pass.
void SpectrogramWorker::drainQueuedAudio(const bool analyze)
{
    std::size_t drained {0};
    while (drained < static_cast<std::size_t>(ANALYZER_FIFO_FRAMES)) {
        const auto frames = static_cast<std::size_t>(analyzerFifo.pop(leftScratch, rightScratch));
        if (frames == 0) {
            return;
        }
        drained += frames;
        if (analyze) {
            analyzer.pushSamples(
                std::span<const float>(leftScratch).first(frames), std::span<const float>(rightScratch).first(frames)
            );
        }
        if (frames < DRAIN_CHUNK) {
            return;
        }
    }
}
}  // namespace esgrove::image_control
