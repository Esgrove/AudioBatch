/// Declares SpectrogramWorker, the background thread that analyzes the queued audio
/// and builds the spectrogram's particles, so the message thread only handles the overlays and the controls.

#pragma once

#include "analysis/SpatialAnalyzer.h"
#include "analysis/StereoSampleFifo.h"
#include "Parameters.h"
#include "ui/SpectrogramParticles.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace esgrove::image_control
{
class SpectrogramRenderer;

/// The analysis settings the editor hands to the worker on every display refresh.
struct SpectrogramAnalysisSettings {
    /// Maps the level difference between the channels to the stereo position.
    StereoPositionCurve positionCurve;
    /// Editor-observed sample rate in Hz, used to invalidate queued audio on settings changes.
    /// The FIFO's source-rate metadata determines how the worker prepares its analyzer.
    double sampleRate {ANALYZER_DEFAULT_SAMPLE_RATE};
    /// Time constant of the particle fade, in seconds, which scales the particle weight.
    double persistenceSeconds {DEFAULT_PERSISTENCE_SECONDS};
    /// Contrast of the level curve, from 0 for linear in dB to 1 for the steepest S-curve.
    double contrast {DEFAULT_CONTRAST_PERCENT / MAX_CONTRAST_PERCENT};
    /// How frequency maps onto the horizontal axis.
    FrequencyScale scale {FrequencyScale::logarithmic};
    /// How the particles are coloured.
    SpectrogramColours colours {SpectrogramColours::frequency};
    /// Whether to analyze the queued audio. Otherwise it is discarded, such as while rendering is bypassed.
    bool analyze {true};
};

/// Drains the processor's analyzer ring, runs the spatial analysis, and builds the particles on its own thread,
/// then hands them to the renderer.
///
/// The editor wakes it once per display refresh, keeping analysis off the message thread.
/// It also wakes on its own now and then, so the ring keeps draining if the display refreshes stall.
/// The analyzer and the particle builder belong to the worker thread alone.
/// The editor talks to it only through setSettings, requestClear, and wake, which are thread safe.
class SpectrogramWorker final : private juce::Thread
{
public:
    SpectrogramWorker(StereoSampleFifo& fifo, SpectrogramRenderer& renderer);

    ~SpectrogramWorker() override;

    SpectrogramWorker(const SpectrogramWorker&) = delete;
    SpectrogramWorker(SpectrogramWorker&&) = delete;
    SpectrogramWorker& operator=(const SpectrogramWorker&) = delete;
    SpectrogramWorker& operator=(SpectrogramWorker&&) = delete;

    [[nodiscard]] std::uint64_t completedPassCount() const noexcept;
    [[nodiscard]] std::uint64_t requestPass() noexcept;

    void requestClear() noexcept;
    void setSettings(const SpectrogramAnalysisSettings& newSettings);
    void start();
    void stop();
    void wake();

private:
    void analyzeCurrentEpoch(std::size_t frames, AnalyzerEpoch epoch);
    void drainQueuedAudio(bool analyze, AnalyzerEpoch epoch);
    void processQueuedAudio();
    void run() override;

    /// The processor's analyzer ring, which outlives the worker. The worker is its only consumer.
    StereoSampleFifo& analyzerFifo;
    /// Receives the particles. Owned by the editor, which stops the worker before it goes away.
    SpectrogramRenderer& spectrogramRenderer;

    /// Analyzes the queued audio. Used only on the worker thread.
    SpatialAnalyzer analyzer;
    /// Turns the analyzed frames into particles. Used only on the worker thread.
    SpectrogramParticleBuilder particleBuilder;
    /// Scratch buffer the left channel of the ring is drained into.
    std::vector<float> leftScratch;
    /// Scratch buffer the right channel of the ring is drained into.
    std::vector<float> rightScratch;
    /// Source generations copied under the same SPSC read scope as the samples.
    std::vector<AnalyzerEpoch> epochScratch;

    /// Guards settings.
    std::mutex settingsMutex;
    /// The latest settings from the editor.
    SpectrogramAnalysisSettings settings;
    /// Number of clears the editor asked for.
    std::atomic<std::uint32_t> requestedClears {0};

    /// Number of clears the worker carried out. Used only on the worker thread.
    std::uint32_t appliedClears {0};
    /// Latest requested synchronization ticket, captured before a worker pass.
    std::atomic<std::uint64_t> requestedPasses {0};
    /// Last completed ticket, published only after particles and clears reach the renderer.
    std::atomic<std::uint64_t> completedPasses {0};
    /// Source generation the analyzer history belongs to, used on the worker thread only.
    AnalyzerEpoch appliedEpoch {0};
    /// Sample rate the analyzer was last prepared for, or 0 before the first preparation.
    double preparedSampleRate {0.0};
};
}  // namespace esgrove::image_control
