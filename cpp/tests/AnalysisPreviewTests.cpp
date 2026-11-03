/// Tests transient waveform snapshots from the same decoder that measures a file.
/// Exercises partial snapshots, selection changes during decoding, the single latest slot,
/// strong source identity, cancellation, and separation from completed waveform persistence.

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCoordinator.h"
#include "analysis/AnalysisPreview.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/AudioMeasurement.h"
#include "analysis/NativeWaveformData.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/FileUtils.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace audiobatch::tests
{
/// Checks selected-file publication without a desktop, second decoder, or timing-based sleeps.
class AnalysisPreviewTests final : public juce::UnitTest
{
public:
    /// Registers the selected-file progressive waveform suite.
    AnalysisPreviewTests() : juce::UnitTest("Analysis preview", "AudioBatch") {}

    /// Uses decoder progress callbacks as deterministic boundaries after preview observation.
    void runTest() override
    {
        runObserverTests();
        runSelectionTests();
        runLatestSlotTests();
        runCancellationTests();
    }

private:
    void runObserverTests();
    void runSelectionTests();
    void runLatestSlotTests();
    void runCancellationTests();
};

/// Confirms snapshots exist before completion and observer failures cannot change measured results.
void AnalysisPreviewTests::runObserverTests()
{
    beginTest("The analysis observer sees partial waveforms before progress and cannot fail analysis");
    const auto file = fixtureFile("sine_stereo_44100_16.wav");
    const auto reference = analyzeFile(file);
    std::int64_t observedFrames = 0;
    int observerCalls = 0;
    int progressCalls = 0;
    bool ordered = true;
    bool partial = false;
    bool bounded = true;
    const auto result = analyzeFile(
        file,
        {},
        [&](float /*progress*/) {
            ++progressCalls;
            ordered = ordered && observerCalls == progressCalls;
        },
        [&](const AudioMeasurement& measurement, const std::int64_t decodedFrames) {
            ++observerCalls;
            bounded = bounded && decodedFrames > observedFrames && decodedFrames <= reference.lengthInSamples;
            observedFrames = decodedFrames;
            if (decodedFrames < reference.lengthInSamples) {
                const auto snapshot = measurement.waveformSnapshot();
                partial = partial || (snapshot && snapshot->getSize() > 0);
            }
        }
    );
    expect(!result.hasError());
    expect(observerCalls > 1);
    expect(ordered);
    expect(partial);
    expect(bounded);
    expect(observedFrames == reference.lengthInSamples);
    expect(result.nativeWaveform.has_value());
    expectAnalysisMeasurementsEqual(*this, result, reference);

    const auto failedObserver = analyzeFile(file, {}, {}, [](const AudioMeasurement&, std::int64_t) {
        throw std::runtime_error("Optional preview failure");
    });
    expect(!failedObserver.hasError());
    expectAnalysisMeasurementsEqual(*this, failedObserver, reference);
    expect(failedObserver.nativeWaveform == reference.nativeWaveform);
}

/// Switches A to B to A while A is decoding, then checks same-path and strong-identity selection gates.
void AnalysisPreviewTests::runSelectionTests()
{
    beginTest("Selection can change during decoding and old generations never reappear");
    const TemporaryDirectory directory;
    const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "selected.wav");
    const auto other = directory.copyFixture("sine_stereo_44100_16.wav", "other.wav");
    const utils::FileIdentity identity(file);
    AnalysisCache cache(directory.getChildFile("analysis.db"));
    expect(cache.open());
    AnalysisCoordinator coordinator(cache, 1);
    coordinator.setPreviewFile(identity, 1);
    AudioAnalysisOptions options;
    options.inputPaths.add(file);
    options.refresh = true;
    for (int scenario = 0; scenario < 5; ++scenario) {
        coordinator.setPreviewFile(identity, 1);
        int progressCalls = 0;
        bool initialPartial = false;
        bool resetDroppedPending = false;
        bool partialNotPersisted = true;
        coordinator.setProgressCallback([&](const juce::File&, float /*progress*/, int /*runId*/) {
            ++progressCalls;
            if (scenario == 0) {
                partialNotPersisted = partialNotPersisted && !cache.getWaveformData(file);
            }
            if (progressCalls != 1) {
                return;
            }
            const auto preview = coordinator.takePreview();
            initialPartial = preview && preview->selectionGeneration == 1 && preview->decodedFrames > 0
                && preview->decodedFrames < 44'100 && preview->waveform.has_value();
            if (scenario == 0) {
                coordinator.setPreviewFile(other, 2);
                coordinator.setPreviewFile(identity, 3);
            } else if (scenario == 1) {
                coordinator.setPreviewFile(identity, 3);
            } else if (scenario == 2) {
                coordinator.setPreviewFile(other, 3);
            } else if (scenario == 3) {
                auto wrongIdentity = identity;
                ++wrongIdentity.preciseTimes.first;
                coordinator.setPreviewFile(wrongIdentity, 3);
            } else {
                coordinator.setPreviewFile(juce::File {}, 3);
            }
            resetDroppedPending = !coordinator.takePreview();
        });
        const auto results = coordinator.analyzeBlocking(options);
        expect(progressCalls >= 2);
        expect(initialPartial);
        expect(resetDroppedPending);
        expect(partialNotPersisted, "Partial snapshots must not reach completed waveform persistence");
        const auto latest = coordinator.takePreview();
        if (scenario <= 1) {
            expect(latest.has_value());
            expect(latest && latest->selectionGeneration == 3);
        } else {
            expect(!latest, "An unselected, replaced, or cleared source must not publish");
        }
        expect(!coordinator.takePreview());
        expectEquals(static_cast<int>(results.size()), 1);
        const auto completed = cache.getWaveformData(file);
        expect(completed.has_value() && isValidWaveformData(*completed));
    }
}

/// Forces several admitted snapshots without draining the slot and expects only the newest one.
void AnalysisPreviewTests::runLatestSlotTests()
{
    beginTest("Only the latest unread snapshot is retained, including after completion");
    const TemporaryDirectory directory;
    const auto file = directory.copyFixture("sine_stereo_44100_16.wav");
    const utils::FileIdentity identity(file);
    AnalysisCache cache(directory.getChildFile("analysis.db"));
    expect(cache.open());
    AnalysisCoordinator coordinator(cache, 1);
    std::uint64_t generation = 10;
    coordinator.setPreviewFile(identity, generation);
    int progressCalls = 0;
    coordinator.setProgressCallback([&](const juce::File&, const float progress, int /*runId*/) {
        ++progressCalls;
        // Leave the last few blocks unread. Each new generation admits a fresh snapshot immediately.
        if (progress < 0.7f) {
            coordinator.setPreviewFile(identity, ++generation);
        }
    });
    AudioAnalysisOptions options;
    options.inputPaths.add(file);
    const auto results = coordinator.analyzeBlocking(options);
    expectEquals(static_cast<int>(results.size()), 1);
    expect(progressCalls >= 2);
    const auto preview = coordinator.takePreview();
    expect(preview.has_value());
    if (preview) {
        expect(preview->selectionGeneration == generation);
        expect(preview->decodedFrames > 4410);
        expect(preview->decodedFrames <= 44'100);
        expect(preview->waveform.has_value());
    }
    expect(!coordinator.takePreview(), "Taking the latest slot must leave no queued older snapshot");
    coordinator.setPreviewFile(juce::File {}, ++generation);
    expect(!coordinator.takePreview());
}

/// Holds the worker at a decoded-block boundary while the test thread cancels the run.
void AnalysisPreviewTests::runCancellationTests()
{
    beginTest("Cancellation drops an unread snapshot and prevents later delivery");
    const TemporaryDirectory directory;
    const auto file = directory.copyFixture("sine_stereo_44100_16.wav");
    AnalysisCache cache(directory.getChildFile("analysis.db"));
    expect(cache.open());
    juce::WaitableEvent reachedProgress;
    juce::WaitableEvent releaseWorker;
    AnalysisCoordinator coordinator(cache, 1);
    coordinator.setPreviewFile(file, 1);
    coordinator.setProgressCallback([&](const juce::File&, float /*progress*/, int /*runId*/) {
        reachedProgress.signal();
        static_cast<void>(releaseWorker.wait(5000));
    });
    AudioAnalysisOptions options;
    options.inputPaths.add(file);
    static_cast<void>(coordinator.start(options));
    const auto reached = reachedProgress.wait(5000);
    expect(reached);
    coordinator.cancel();
    expect(!coordinator.takePreview());
    releaseWorker.signal();
    coordinator.cancelAndWait();
    expect(!coordinator.takePreview());
    expect(!cache.getWaveformData(file));
    expect(!cache.getAnalysis(file));

    // The same selection can immediately receive a later run despite the earlier throttle timestamp.
    coordinator.setProgressCallback({});
    const auto results = coordinator.analyzeBlocking(options);
    expectEquals(static_cast<int>(results.size()), 1);
    const auto replacement = coordinator.takePreview();
    expect(replacement.has_value());
    expect(replacement && replacement->selectionGeneration == 1);
    expect(!coordinator.takePreview());
}

/// Creates the progressive preview suite for explicit registration by the test runner.
std::unique_ptr<juce::UnitTest> createAnalysisPreviewTests()
{
    return std::make_unique<AnalysisPreviewTests>();
}
}  // namespace audiobatch::tests
