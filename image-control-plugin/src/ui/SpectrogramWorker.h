/// Declares SpectrogramWorker, the background thread that analyzes the queued audio
/// and builds the spectrogram's particles, so the message thread only handles the overlays and the controls.

#pragma once

#include "analysis/SpatialAnalyzer.h"
#include "analysis/StereoSampleFifo.h"
#include "Parameters.h"
#include "ui/SpectrogramParticles.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace esgrove::image_control
{
class SpectrogramRenderer;

/// The analysis settings the editor hands to the worker on every display refresh.
struct SpectrogramAnalysisSettings {
    /// Sample rate of the queued audio, in Hz.
    double sampleRate {ANALYZER_DEFAULT_SAMPLE_RATE};
    /// Level difference between the channels that reaches the edges, in dB.
    double focusDecibels {DEFAULT_FOCUS_DB};
    /// Time constant of the particle fade, in seconds, which scales the particle weight.
    double persistenceSeconds {DEFAULT_PERSISTENCE_SECONDS};
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
/// The editor wakes it once per display refresh, so it analyzes at the same pace as before, off the message thread.
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

    void requestClear() noexcept;
    void setSettings(const SpectrogramAnalysisSettings& newSettings);
    void start();
    void stop();
    void wake();

private:
    void drainQueuedAudio(bool analyze);
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

    /// Guards settings.
    std::mutex settingsMutex;
    /// The latest settings from the editor.
    SpectrogramAnalysisSettings settings;
    /// Number of clears the editor asked for.
    std::atomic<std::uint32_t> requestedClears {0};

    /// Number of clears the worker carried out. Used only on the worker thread.
    std::uint32_t appliedClears {0};
    /// Sample rate the analyzer was last prepared for, or 0 before the first preparation.
    double preparedSampleRate {0.0};
};
}  // namespace esgrove::image_control
