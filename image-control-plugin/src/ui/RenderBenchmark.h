/// RenderBenchmark retains opt-in bounded message-thread refresh observations for visible-editor benchmarks.

#pragma once

#include "Parameters.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace esgrove::image_control
{
/// A successful counter observation does not establish GPU completion or physical presentation.
struct RenderBenchmarkSample {
    /// Monotonic time since recorder creation, in seconds.
    double elapsedSeconds {0.0};
    /// Successfully issued spectrum frames first observed at this refresh.
    int successfulFrames {0};
    /// Message-thread refresh work, excluding GPU execution and compositor work.
    double updateSeconds {0.0};
    /// Actual logical editor width at the refresh.
    int logicalWidth {0};
    /// Actual logical editor height at the refresh.
    int logicalHeight {0};
    /// JUCE ancestor visibility, not an occlusion or scanout guarantee.
    bool showing {false};
};

/// Allocated only by an explicit absolute report path. All access belongs to the editor's message thread.
class RenderBenchmark final
{
public:
    /// Allocates bounded storage once, outside refresh, rendering, and audio processing.
    explicit RenderBenchmark(juce::File destination, const bool sweep = false) :
        path(std::move(destination)),
        sweepEnabled(sweep)
    {
        samples.reserve(MAXIMUM_SAMPLES);
    }

    /// Opts in only when a dedicated host supplied an absolute output path before plugin loading.
    [[nodiscard]] static std::unique_ptr<RenderBenchmark> fromEnvironment()
    {
        const auto destination = juce::SystemStats::getEnvironmentVariable("AUDIO_IMAGECONTROL_RENDER_REPORT", {});
        if (!juce::File::isAbsolutePath(destination)) {
            return {};
        }
        const bool sweep = juce::SystemStats::getEnvironmentVariable("AUDIO_IMAGECONTROL_RENDER_SWEEP", {}) == "1";
        return std::make_unique<RenderBenchmark>(juce::File(destination), sweep);
    }

    /// Applies a deterministic live appearance gesture through ordinary message-thread ValueTree listeners.
    /// Benchmark-only publication can allocate, but never enters state restore or an audio callback.
    void applySweep(juce::ValueTree editorState, const std::uint64_t generation)
    {
        applySweepAt(
            std::move(editorState),
            generation,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
        );
    }

    /// Exposes the elapsed gesture origin explicitly so unit tests need neither clocks nor environment mutation.
    void applySweepAt(juce::ValueTree editorState, const std::uint64_t generation, const double elapsed)
    {
        if (!sweepEnabled) {
            return;
        }
        constexpr double percentToNormalized {0.01};
        constexpr double minimumPersistence {0.1};
        constexpr double persistenceSpan {2.4};
        constexpr double minimumContrast {10.0};
        constexpr double contrastSpan {80.0};
        const double phase = std::fmod(elapsed / SWEEP_PERIOD_SECONDS, 1.0);
        const double triangle = 1.0 - std::abs(phase * 2.0 - 1.0);
        const double persistence = minimumPersistence + persistenceSpan * triangle;
        const double contrast = minimumContrast + contrastSpan * triangle;
        if (!firstGeneration.has_value()) {
            firstGeneration = generation;
        }
        if (lastGeneration.has_value() && *lastGeneration != generation) {
            ++generationChanges;
        }
        lastGeneration = generation;
        editorState.setProperty(PERSISTENCE_PROPERTY, persistence, nullptr);
        editorState.setProperty(CONTRAST_PROPERTY, contrast, nullptr);
        minimumSweepPersistence = std::min(minimumSweepPersistence, persistence);
        maximumSweepPersistence = std::max(maximumSweepPersistence, persistence);
        minimumSweepContrast = std::min(minimumSweepContrast, contrast * percentToNormalized);
        maximumSweepContrast = std::max(maximumSweepContrast, contrast * percentToNormalized);
        ++sweepUpdates;
    }

    /// Records bounded GUI work and the successful renderer counter without allocation or locking.
    void observe(
        const std::chrono::steady_clock::time_point refreshStarted,
        const int successfulFrames,
        const int width,
        const int height,
        const bool showing
    )
    {
        if (samples.size() == MAXIMUM_SAMPLES) {
            ++droppedSamples;
            return;
        }
        samples.push_back({
            .elapsedSeconds = std::chrono::duration<double>(refreshStarted - started).count(),
            .successfulFrames = successfulFrames,
            .updateSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - refreshStarted).count(),
            .logicalWidth = width,
            .logicalHeight = height,
            .showing = showing,
        });
    }

    /// Flushes on editor destruction after GL detachment. No refresh, render, or audio callback writes files.
    [[nodiscard]] bool flush() const
    {
        auto root = std::make_unique<juce::DynamicObject>();
        root->setProperty("schema_version", 1);
        root->setProperty("implementation", "cpp-juce-opengl");
        root->setProperty("build_type", IMAGE_CONTROL_BUILD_RELEASE != 0 ? "Release" : "non-Release");
        root->setProperty("native_cpu_enabled", IMAGE_CONTROL_BUILD_NATIVE_CPU != 0);
        root->setProperty("counter_kind", "successful_spectrum_command_issue_not_gpu_completion_or_scanout");
        root->setProperty("frame_timestamp_kind", "first_refresh_observing_successful_counter_not_encoding_time");
        root->setProperty("started_unix_seconds", startedUnixSeconds);
        root->setProperty("maximum_samples", static_cast<int>(MAXIMUM_SAMPLES));
        root->setProperty("dropped_samples", static_cast<juce::int64>(droppedSamples));
        auto sweep = std::make_unique<juce::DynamicObject>();
        sweep->setProperty("enabled", sweepEnabled);
        sweep->setProperty("accepted_updates", static_cast<juce::int64>(sweepUpdates));
        sweep->setProperty("rejected_updates", 0);
        sweep->setProperty("generation_changes", static_cast<juce::int64>(generationChanges));
        sweep->setProperty(
            "first_generation",
            firstGeneration.has_value() ? juce::var(static_cast<juce::int64>(*firstGeneration)) : juce::var()
        );
        sweep->setProperty(
            "last_generation",
            lastGeneration.has_value() ? juce::var(static_cast<juce::int64>(*lastGeneration)) : juce::var()
        );
        sweep->setProperty(
            "minimum_persistence_seconds", sweepUpdates > 0 ? juce::var(minimumSweepPersistence) : juce::var()
        );
        sweep->setProperty(
            "maximum_persistence_seconds", sweepUpdates > 0 ? juce::var(maximumSweepPersistence) : juce::var()
        );
        sweep->setProperty("minimum_contrast", sweepUpdates > 0 ? juce::var(minimumSweepContrast) : juce::var());
        sweep->setProperty("maximum_contrast", sweepUpdates > 0 ? juce::var(maximumSweepContrast) : juce::var());
        sweep->setProperty("driver", "vblank_ui_thread_production_editor_state_listeners");
        sweep->setProperty("period_seconds", SWEEP_PERIOD_SECONDS);
        root->setProperty("appearance_sweep", juce::var(sweep.release()));
        juce::Array<juce::var> rows;
        rows.ensureStorageAllocated(static_cast<int>(samples.size()));
        for (const auto& sample : samples) {
            auto row = std::make_unique<juce::DynamicObject>();
            row->setProperty("elapsed_seconds", sample.elapsedSeconds);
            row->setProperty("successful_frames", sample.successfulFrames);
            row->setProperty("update_seconds", sample.updateSeconds);
            row->setProperty("logical_width", sample.logicalWidth);
            row->setProperty("logical_height", sample.logicalHeight);
            row->setProperty("showing", sample.showing);
            rows.add(juce::var(row.release()));
        }
        root->setProperty("samples", juce::var(rows));
        return path.replaceWithText(juce::JSON::toString(juce::var(root.release()), true));
    }

private:
    /// At 240 Hz this retains more than four minutes without growing during refresh.
    static constexpr std::size_t MAXIMUM_SAMPLES {65'536};
    /// Both native implementations traverse the same continuous gesture every four seconds.
    static constexpr double SWEEP_PERIOD_SECONDS {4.0};
    /// Unit conversion used only for host/report wall-clock alignment.
    static constexpr double SECONDS_PER_MILLISECOND {0.001};
    /// Monotonic origin for gap measurements, unrelated to wall-clock adjustment.
    std::chrono::steady_clock::time_point started {std::chrono::steady_clock::now()};
    /// Explicit output destination, never inferred from the plugin's user settings.
    juce::File path;
    /// Storage reserved once on the message thread before the first refresh.
    std::vector<RenderBenchmarkSample> samples;
    /// Wall origin used only to align the host measurement interval with observations.
    double startedUnixSeconds {static_cast<double>(juce::Time::currentTimeMillis()) * SECONDS_PER_MILLISECOND};
    /// Saturation remains visible in the report rather than silently overwriting early samples.
    std::uint64_t droppedSamples {0};
    /// Explicit opt-in never affects ordinary hosts or offscreen renderer probes.
    bool sweepEnabled {false};
    /// Accepted UI publication count includes both settings in each update.
    std::uint64_t sweepUpdates {0};
    /// Analyzer source transitions observed across successive live settings publications.
    std::uint64_t generationChanges {0};
    /// First source epoch seen by the live driver, independent of pre-editor preparation.
    std::optional<std::uint64_t> firstGeneration;
    /// Latest source epoch seen by the live driver.
    std::optional<std::uint64_t> lastGeneration;
    /// Minimum applied persistence, in seconds.
    double minimumSweepPersistence {MAX_PERSISTENCE_SECONDS};
    /// Maximum applied persistence, in seconds.
    double maximumSweepPersistence {MIN_PERSISTENCE_SECONDS};
    /// Minimum applied normalized contrast.
    double minimumSweepContrast {1.0};
    /// Maximum applied normalized contrast.
    double maximumSweepContrast {0.0};
};
}  // namespace esgrove::image_control
