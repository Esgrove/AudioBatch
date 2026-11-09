/// Implements SpectrogramWorker: the thread loop, handing settings and clears from the editor to the worker,
/// and draining, analyzing, and turning the queued audio into particles for the renderer.

#include "ui/SpectrogramWorker.h"

#include "analysis/SpatialAnalyzer.h"
#include "analysis/StereoSampleFifo.h"
#include "ui/SpectrogramParticles.h"
#include "ui/SpectrogramRenderer.h"

#include <atomic>
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
}  // namespace

/// Creates the worker for the processor's analyzer ring and the renderer. The thread starts with start.
SpectrogramWorker::SpectrogramWorker(StereoSampleFifo& fifo, SpectrogramRenderer& renderer) :
    juce::Thread("ImageControl spectrogram analysis"),
    analyzerFifo(fifo),
    spectrogramRenderer(renderer),
    leftScratch(DRAIN_CHUNK, 0.0F),
    rightScratch(DRAIN_CHUNK, 0.0F),
    epochScratch(DRAIN_CHUNK, 0)
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

/// Returns the last completed synchronization ticket without touching the SPSC consumer index.
std::uint64_t SpectrogramWorker::completedPassCount() const noexcept
{
    return completedPasses.load(std::memory_order_acquire);
}

/// Requests a pass and returns a ticket acknowledged only after the worker finishes that pass.
std::uint64_t SpectrogramWorker::requestPass() noexcept
{
    const auto ticket = requestedPasses.fetch_add(1, std::memory_order_acq_rel) + 1;
    notify();
    return ticket;
}

/// Asks the worker to clear the analysis and the picture before its next particles,
/// such as after switching the analyzer tap.
void SpectrogramWorker::requestClear() noexcept
{
    analyzerFifo.beginEpoch();
    requestedClears.fetch_add(1, std::memory_order_release);
    notify();
}

/// Hands the latest settings to the worker, which applies them on its next pass.
void SpectrogramWorker::setSettings(const SpectrogramAnalysisSettings& newSettings)
{
    const std::scoped_lock lock(settingsMutex);
    if (!juce::approximatelyEqual(settings.sampleRate, newSettings.sampleRate)) {
        // Source rate metadata is authoritative, but settings-only clients still need a queue boundary.
        analyzerFifo.beginEpoch();
    }
    if (settings.analyze != newSettings.analyze) {
        analyzerFifo.beginEpoch();
    }
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
    // Joining indefinitely is safe because each pass is bounded and checks the exit flag.
    // A timed stopThread can forcibly kill a thread while it owns settings or renderer locks.
    waitForThreadToExit(-1);
}

/// Wakes the worker for a pass, once per display refresh.
void SpectrogramWorker::wake()
{
    notify();
}

/// Discards the audio that queued while no editor was open, then runs a pass whenever it is woken.
void SpectrogramWorker::run()
{
    analyzerFifo.beginEpoch();
    while (!threadShouldExit()) {
        const auto ticket = requestedPasses.load(std::memory_order_acquire);
        processQueuedAudio();
        completedPasses.store(ticket, std::memory_order_release);
        if (requestedPasses.load(std::memory_order_acquire) == ticket && !threadShouldExit()) {
            wait(WAKE_TIMEOUT_MILLISECONDS);
        }
    }
}

/// Applies the latest settings and clears, analyzes the queued audio, and hands the particles to the renderer.
void SpectrogramWorker::processQueuedAudio()
{
    SpectrogramAnalysisSettings current;
    AnalyzerEpoch epoch = 0;
    {
        const std::scoped_lock lock(settingsMutex);
        current = settings;
        epoch = analyzerFifo.getEpoch();
    }
    if (!StereoSampleFifo::isStable(epoch)) {
        return;
    }
    const auto sourceRate = analyzerFifo.getSampleRate();
    if (analyzerFifo.getEpoch() != epoch) {
        return;
    }
    bool clear = epoch != appliedEpoch;
    if (!juce::approximatelyEqual(sourceRate, preparedSampleRate)) {
        preparedSampleRate = sourceRate;
        analyzer.prepare(sourceRate);
        clear = true;
    }
    const std::uint32_t clears = requestedClears.load(std::memory_order_acquire);
    if (clears != appliedClears || clear) {
        appliedClears = clears;
        appliedEpoch = epoch;
        analyzer.clear();
        particleBuilder.resetLevel();
        clear = true;
    }
    analyzer.setPositionCurve(current.positionCurve);
    particleBuilder.setFrequencyScale(current.scale);
    particleBuilder.setColours(current.colours);
    particleBuilder.setPersistence(current.persistenceSeconds);
    particleBuilder.setContrast(current.contrast);

    particleBuilder.clear();
    drainQueuedAudio(current.analyze, epoch);
    if (analyzerFifo.getEpoch() != epoch) {
        // A source boundary crossed during analysis. Never publish this generation under the new settings.
        analyzer.clear();
        particleBuilder.clear();
        particleBuilder.resetLevel();
        spectrogramRenderer.submitParticles({}, true);
        return;
    }
    if (clear || !particleBuilder.particles().empty()) {
        spectrogramRenderer.submitParticles(particleBuilder.particles(), clear);
    }
}

/// Feeds only contiguous spans captured under the current source configuration to the analyzer.
void SpectrogramWorker::analyzeCurrentEpoch(const std::size_t frames, const AnalyzerEpoch epoch)
{
    std::size_t first = 0;
    while (first < frames) {
        while (first < frames && epochScratch[first] != epoch) {
            ++first;
        }
        std::size_t last = first;
        while (last < frames && epochScratch[last] == epoch) {
            ++last;
        }
        if (last > first) {
            analyzer.pushSamples(
                std::span<const float>(leftScratch).subspan(first, last - first),
                std::span<const float>(rightScratch).subspan(first, last - first)
            );
        }
        first = last;
    }
}

/// Empties the processor's analyzer ring, passing the audio to the analyzer when analyze is set.
/// One pass takes at most a ring's worth of audio.
/// A producer that runs faster than real time, such as an offline render or pluginval,
/// would otherwise refill the ring as fast as it drains and keep the worker in this loop forever.
/// At most a ring's capacity is examined per pass, even if the producer refills the freed slots.
void SpectrogramWorker::drainQueuedAudio(const bool analyze, const AnalyzerEpoch epoch)
{
    std::size_t drained {0};
    while (drained < static_cast<std::size_t>(ANALYZER_FIFO_FRAMES) && !threadShouldExit()) {
        if (analyzerFifo.getEpoch() != epoch) {
            return;
        }
        const auto frames = static_cast<std::size_t>(analyzerFifo.pop(leftScratch, rightScratch, epochScratch));
        if (frames == 0) {
            return;
        }
        drained += frames;
        if (analyze) {
            analyzeCurrentEpoch(frames, epoch);
        }
        if (frames < DRAIN_CHUNK) {
            return;
        }
    }
}
}  // namespace esgrove::image_control
