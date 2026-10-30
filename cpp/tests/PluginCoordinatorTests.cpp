/// Tests for PluginProcessingCoordinator and the creation of the plugin chains of a run.
/// Covers the worker budget, the chain pool, the order and concurrency of queued files,
/// cancellation and stop requests while a file is in progress, the run id of a replaced run,
/// the failures of PluginLibrary::instantiateChains, and the run that PluginLibrary::createProcessingRun assembles.
/// Files whose output already exists fail before their plugins are prepared,
/// so the runs need no message loop and never replace a file.

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "plugins/PluginLibrary.h"
#include "plugins/PluginProcessing.h"
#include "plugins/PluginProcessingCoordinator.h"
#include "ProcessingTestPlugins.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include <condition_variable>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// How long a coordinator test waits for a run to reach the expected state before it fails.
constexpr auto coordinatorTimeout {std::chrono::seconds(10)};

/// Start error of a run whose chains do not match its plugins.
constexpr auto noChainsMessage {"No plugin chain instances were available to start processing."};

/// One published result and the run it belongs to.
struct PublishedResult {
    PluginProcessingResult result;
    int runId {0};
};

/// Collects what a coordinator publishes, and can hold the first file of a run in its starting callback
/// until the test releases it, so the test can cancel or stop a run while one of its files is in progress.
/// Attach it to a coordinator declared after it, so the coordinator and its jobs are gone before it is.
class CoordinatorRecorder
{
public:
    /// Holds the first started file when holdFirstFile is set, and lets every file through otherwise.
    explicit CoordinatorRecorder(const bool holdFirstFile) : holding(holdFirstFile) {}

    /// Installs the starting, result, completion, and start error callbacks on the coordinator.
    void attach(PluginProcessingCoordinator& coordinator)
    {
        coordinator.setStartingCallback([this](const juce::File& file, const int runId) {
            std::unique_lock lock(mutex);
            startedFiles.push_back(file);
            startedRunIds.push_back(runId);
            condition.notify_all();
            if (holding && startedFiles.size() == 1) {
                condition.wait_for(lock, coordinatorTimeout, [this] { return released; });
            }
        });
        coordinator.setResultCallback([this](const PluginProcessingResult& result, const int runId) {
            const std::scoped_lock lock(mutex);
            results.push_back({.result = result, .runId = runId});
        });
        coordinator.setCompletionCallback([this](const int totalFiles, const int runId) {
            const std::scoped_lock lock(mutex);
            completions.emplace_back(totalFiles, runId);
            condition.notify_all();
        });
        coordinator.setStartErrorCallback([this](const juce::String& message, const int runId) {
            const std::scoped_lock lock(mutex);
            startErrors.add(message);
            startErrorRunIds.push_back(runId);
        });
    }

    /// Waits until the given number of files have started, and returns false on a timeout.
    bool waitForStartedFiles(const std::size_t count)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, coordinatorTimeout, [&] { return startedFiles.size() >= count; });
    }

    /// Waits until the run with the given id has published its completion, and returns false on a timeout.
    bool waitForCompletion(const int runId)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, coordinatorTimeout, [&] {
            return std::ranges::contains(completions, runId, &std::pair<int, int>::second);
        });
    }

    /// Lets the held file continue.
    void release()
    {
        const std::scoped_lock lock(mutex);
        released = true;
        condition.notify_all();
    }

    /// Returns the run ids of the files that started, in the order they started.
    [[nodiscard]] std::vector<int> getStartedRunIds() const
    {
        const std::scoped_lock lock(mutex);
        return startedRunIds;
    }

    /// Returns the published results with their run ids, in the order they were published.
    [[nodiscard]] std::vector<PublishedResult> getResults() const
    {
        const std::scoped_lock lock(mutex);
        return results;
    }

    /// Returns the total and the run id of every published completion.
    [[nodiscard]] std::vector<std::pair<int, int>> getCompletions() const
    {
        const std::scoped_lock lock(mutex);
        return completions;
    }

    /// Returns the attempted run ids attached to start errors.
    [[nodiscard]] std::vector<int> getStartErrorRunIds() const
    {
        const std::scoped_lock lock(mutex);
        return startErrorRunIds;
    }

    /// Returns the messages of the start error callback.
    [[nodiscard]] juce::StringArray getStartErrors() const
    {
        const std::scoped_lock lock(mutex);
        return startErrors;
    }

private:
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<juce::File> startedFiles;
    std::vector<int> startedRunIds;
    std::vector<PublishedResult> results;
    std::vector<std::pair<int, int>> completions;
    juce::StringArray startErrors;
    std::vector<int> startErrorRunIds;
    bool holding;
    bool released {false};
};

/// Returns the given number of chains, each holding one gain-only test plugin.
std::vector<PluginChainInstances> makeChains(const int count)
{
    std::vector<PluginChainInstances> chains;
    for (int index = 0; index < count; ++index) {
        PluginChainInstances chain;
        chain.push_back(std::make_unique<TestPlugin>(TestPluginSettings {}));
        chains.push_back(std::move(chain));
    }
    return chains;
}

/// Returns options for one test plugin, which every chain of makeChains matches.
PluginProcessingOptions makeChainOptions()
{
    TestPlugin plugin(TestPluginSettings {});
    return makeTestPluginOptions(chainOf(plugin));
}

/// Points the application settings at a folder inside the temporary directory.
void useTemporarySettings(juce::ApplicationProperties& properties, const TemporaryDirectory& temporary)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "PluginCoordinatorTests";
    options.filenameSuffix = ".xml";
    options.folderName = temporary.getChildFile("properties").getFullPathName();
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    properties.setStorageParameters(options);
}

/// Returns an enabled chain entry for a VST3 plugin that is not installed.
EnabledChainPlugin missingPlugin(const juce::String& name)
{
    EnabledChainPlugin plugin;
    plugin.description.name = name;
    plugin.description.pluginFormatName = "VST3";
    plugin.description.fileOrIdentifier = utils::format("/Missing/{}.vst3", name);
    plugin.descriptorRef.name = name;
    return plugin;
}
}  // namespace

/// Checks how the plugin processing coordinator queues, publishes, cancels, and stops its runs.
class PluginCoordinatorTests final : public juce::UnitTest
{
public:
    PluginCoordinatorTests() : juce::UnitTest("Plugin processing coordinator", "AudioBatch") {}

    /// Runs the coordinator and chain instantiation checks.
    void runTest() override
    {
        const TemporaryDirectory directory;

        beginTest("Plugin worker budgets reserve one physical core and handle unavailable counts");
        {
            expectEquals(getPluginProcessingWorkerCount(16, 32), 15);
            expectEquals(getPluginProcessingWorkerCount(8, 16), 7);
            expectEquals(getPluginProcessingWorkerCount(1, 2), 1);
            expectEquals(getPluginProcessingWorkerCount(2, 4), 1);
            expectEquals(getPluginProcessingWorkerCount(0, 32), 15);
            expectEquals(getPluginProcessingWorkerCount(-1, 17), 7);
            expectEquals(getPluginProcessingWorkerCount(0, 1), 1);
            expectEquals(getPluginProcessingWorkerCount(0, -1), 1);
            expectEquals(getPluginProcessingWorkerCount(16, 16), 15);
        }

        beginTest("A chain lease returns its chain to the pool when it is destroyed");
        {
            PluginRunChains runChains(std::vector<PluginChainInstances>(1), true);
            expectEquals(runChains.getNumChains(), 1);
            {
                const auto lease = runChains.acquire([] { return true; });
                expect(lease.has_value());
                int checks = 0;
                expect(!runChains.acquire([&checks] { return ++checks <= 1; }).has_value());
            }
            expect(runChains.acquire([] { return true; }).has_value());
            expect(!PluginRunChains(std::vector<PluginChainInstances>(1), false).acquire([] { return false; }));
        }

        beginTest("Coordinator queues selected files in input order, skips duplicates, and fails missing files");
        testCoordinator(directory, 1);

        beginTest("Coordinator can start six independent files concurrently");
        testCoordinator(directory, 6);

        beginTest("An empty queue or invalid chain does not announce processing");
        {
            const ScopedLogCapture capture;
            PluginProcessingCoordinator coordinator(1);
            const auto options = makeChainOptions();
            expectEquals(coordinator.start({}, options, {}), 0);
            expectEquals(coordinator.start({}, options, makeChains(1)), 0);
            expect(capture.getMessages().isEmpty());
        }

        beginTest("Chains that do not match the options report a start error and complete with no files");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.wav", "mismatched.wav");
            CoordinatorRecorder recorder(false);
            PluginProcessingCoordinator coordinator(1);
            recorder.attach(coordinator);
            auto options = makeChainOptions();
            options.plugins.push_back(options.plugins.front());
            expectEquals(coordinator.start({analyzeFile(source)}, options, makeChains(1)), 0);
            const auto runId = coordinator.getCurrentRunId();
            expect(recorder.waitForCompletion(runId));
            expectEquals(recorder.getStartErrors().joinIntoString(","), juce::String(noChainsMessage));
            expect(recorder.getStartErrorRunIds() == std::vector {runId});
            expect(recorder.getCompletions() == std::vector {std::pair {0, runId}});
            expect(recorder.getResults().empty());
        }

        testCancellation(directory);
        testStop(directory);
        testReplacedRun(directory);
        testChainInstantiation();
    }

private:
    /// Checks FIFO pickup with one worker and a bounded start barrier with six workers.
    /// Existing outputs reject each job before plugin preparation, avoiding message-loop waits and file replacement.
    void testCoordinator(const TemporaryDirectory& directory, const int workerCount)
    {
        const ScopedLogCapture capture;
        const std::vector<juce::String> names {"zulu", "alpha", "mike", "bravo", "yankee", "charlie"};
        std::vector<AudioAnalysisRecord> records;
        std::vector<juce::String> expectedPaths;
        for (const auto& fileStem : names) {
            const auto source = directory.copyFixture("sine_stereo_44100_16.wav", fileStem + ".wav");
            const auto existingOutput = directory.copyFixture("sine_stereo_44100_16.aif", fileStem + ".aif");
            expect(existingOutput.existsAsFile());
            records.push_back(analyzeFile(source));
            expectedPaths.push_back(source.getFullPathName());
        }
        records.push_back(records.front());
        AudioAnalysisRecord missing;
        missing.file = directory.getChildFile("missing.wav");
        missing.fullPath = missing.file.getFullPathName();
        records.push_back(missing);
        expectedPaths.push_back(missing.fullPath);

        std::mutex mutex;
        std::condition_variable condition;
        std::vector<juce::String> startedPaths;
        int completedFiles = 0;
        int resultCount = 0;
        juce::String missingFileError;
        bool released = workerCount == 1;
        PluginProcessingCoordinator coordinator(workerCount);
        coordinator.setStartingCallback([&](const juce::File& file, int /*runId*/) {
            std::unique_lock lock(mutex);
            utils::logInfo("Test file starting: {}", file.getFileName().quoted());
            startedPaths.push_back(file.getFullPathName());
            condition.notify_all();
            condition.wait_for(lock, coordinatorTimeout, [&] { return released; });
        });
        coordinator.setResultCallback([&](const PluginProcessingResult& result, int /*runId*/) {
            const std::scoped_lock lock(mutex);
            ++resultCount;
            if (result.originalFile == missing.file) {
                missingFileError = outcomeError(result.outcome);
            }
        });
        coordinator.setCompletionCallback([&](const int totalFiles, int /*runId*/) {
            const std::scoped_lock lock(mutex);
            completedFiles = totalFiles;
            condition.notify_all();
        });

        auto chains = makeChains(workerCount);
        const auto options = makeTestPluginOptions({chains.front().front().get()});
        expectEquals(coordinator.start(records, options, std::move(chains)), 7);
        {
            std::unique_lock lock(mutex);
            if (workerCount > 1) {
                expect(condition.wait_for(lock, coordinatorTimeout, [&] { return startedPaths.size() >= 6; }));
                released = true;
                condition.notify_all();
            }
            expect(condition.wait_for(lock, coordinatorTimeout, [&] { return completedFiles == 7; }));
        }
        coordinator.cancelAndWait();
        expectEquals(resultCount, 7);
        expectEquals(missingFileError, juce::String("File does not exist"));
        const auto messages = capture.getMessages();
        expect(!messages.isEmpty());
        if (!messages.isEmpty()) {
            expect(
                messages[0].endsWith(
                    utils::format("[INFO]: Plugin processing started: 7 files, up to {} concurrent", workerCount)
                )
            );
        }
        expectEquals(
            static_cast<int>(std::ranges::count_if(
                messages, [](const auto& message) { return message.contains("Plugin processing started:"); }
            )),
            1
        );
        if (workerCount == 1) {
            expect(startedPaths == expectedPaths);
        }
    }

    /// Holds the first file of a run in its starting callback, cancels the run from another thread,
    /// and checks that the run publishes no result or completion, and leaves its files unchanged without outputs.
    void testCancellation(const TemporaryDirectory& directory)
    {
        beginTest("A cancelled run publishes nothing more and leaves its files unchanged");
        const auto records = copySources(directory, {"cancel_held.wav", "cancel_queued.wav"});
        const auto originals = loadContents(records);
        CoordinatorRecorder recorder(true);
        PluginProcessingCoordinator coordinator(1);
        recorder.attach(coordinator);

        expectEquals(coordinator.start(records, makeChainOptions(), makeChains(1)), 2);
        const auto runId = coordinator.getCurrentRunId();
        expect(recorder.waitForStartedFiles(1));
        {
            // The run id changes before cancelAndWait starts waiting for the held job,
            // so the job sees the cancellation as soon as it is released.
            const std::jthread canceller([&coordinator] { coordinator.cancelAndWait(); });
            expect(waitForRunIdChange(coordinator, runId), "the run id did not change");
            recorder.release();
        }

        expect(recorder.getStartedRunIds() == std::vector {runId});
        expect(recorder.getResults().empty(), utils::format("{} results", recorder.getResults().size()));
        expect(recorder.getCompletions().empty());
        expectUnchanged(records, originals);
    }

    /// Holds the first file of a run in its starting callback, asks the run to stop,
    /// and checks that every file is published as cancelled with the run id, and that the run still completes.
    void testStop(const TemporaryDirectory& directory)
    {
        beginTest("A stopped run reports its unfinished files as cancelled, leaves them unchanged, and completes");
        const auto records = copySources(directory, {"stop_held.wav", "stop_queued.wav"});
        const auto originals = loadContents(records);
        CoordinatorRecorder recorder(true);
        PluginProcessingCoordinator coordinator(1);
        recorder.attach(coordinator);

        expectEquals(coordinator.start(records, makeChainOptions(), makeChains(1)), 2);
        const auto runId = coordinator.getCurrentRunId();
        expect(recorder.waitForStartedFiles(1));
        coordinator.requestStop();
        expectEquals(coordinator.getCurrentRunId(), runId);
        recorder.release();
        expect(recorder.waitForCompletion(runId));
        coordinator.cancelAndWait();

        expect(recorder.getStartedRunIds() == std::vector {runId});
        const auto results = recorder.getResults();
        expectEquals(static_cast<int>(results.size()), 2);
        for (const auto& [result, resultRunId] : results) {
            const auto* failed = std::get_if<OperationFailed>(&result.outcome);
            expect(failed != nullptr && failed->cancelled, result.fileName);
            expectEquals(resultRunId, runId);
        }
        expect(recorder.getCompletions() == std::vector {std::pair {2, runId}});
        expectUnchanged(records, originals);
    }

    /// Starts a second run while the first file of the first run is held,
    /// and checks that only the second run publishes, each callback with the id of the second run.
    /// Starting waits for the running job, so another thread releases it once the first run is cancelled.
    void testReplacedRun(const TemporaryDirectory& directory)
    {
        beginTest("A run started while another one is processing replaces it, and only the new run publishes");
        const auto firstRecords = copySources(directory, {"replaced_held.wav", "replaced_queued.wav"});
        const auto secondSource = directory.copyFixture("sine_stereo_44100_16.wav", "replacing.wav");
        expect(directory.copyFixture("sine_stereo_44100_16.aif", "replacing.aif").existsAsFile());
        CoordinatorRecorder recorder(true);
        PluginProcessingCoordinator coordinator(1);
        recorder.attach(coordinator);

        expectEquals(coordinator.start(firstRecords, makeChainOptions(), makeChains(1)), 2);
        const auto firstRunId = coordinator.getCurrentRunId();
        expect(recorder.waitForStartedFiles(1));
        int secondRunId {-1};
        {
            // Releases the held file even after a timeout, so start cannot wait for it forever.
            const std::jthread releaser([&] {
                static_cast<void>(waitForRunIdChange(coordinator, firstRunId));
                recorder.release();
            });
            expectEquals(coordinator.start({analyzeFile(secondSource)}, makeChainOptions(), makeChains(1)), 1);
            secondRunId = coordinator.getCurrentRunId();
        }
        expect(secondRunId != firstRunId);
        expect(recorder.waitForCompletion(secondRunId));
        coordinator.cancelAndWait();

        expect(recorder.getStartedRunIds() == std::vector {firstRunId, secondRunId});
        const auto results = recorder.getResults();
        expectEquals(static_cast<int>(results.size()), 1);
        if (!results.empty()) {
            expect(results.front().result.originalFile == secondSource);
            expectEquals(results.front().runId, secondRunId);
            expect(outcomeError(results.front().result.outcome).contains("already exists"));
        }
        expect(recorder.getCompletions() == std::vector {std::pair {1, secondRunId}});
    }

    /// Checks that a chain that cannot be created reports the plugin that failed and hands out no chains,
    /// that a chain of the repository's test VST3 is created once per worker with its initial state,
    /// and that a processing run assembled from it carries the initial state of a plugin without saved state.
    void testChainInstantiation()
    {
        beginTest("A plugin that cannot be created fails the chain instantiation and names the plugin");
        const TemporaryDirectory temporary;
        juce::ApplicationProperties properties;
        useTemporarySettings(properties, temporary);
        const PluginLibrary library(properties);

        const auto missing = library.instantiateChains({missingPlugin("Missing Effect")}, 2);
        expect(missing.errorMessage.isNotEmpty());
        expectEquals(missing.failedPluginName, juce::String("Missing Effect"));
        expect(missing.chains.empty());

        const auto bundle = testPluginBundle();
        expect(bundle.isDirectory(), utils::format("Test plugin bundle {} is missing", bundle.getFullPathName()));
        juce::OwnedArray<juce::PluginDescription> types;
        juce::VST3PluginFormat format;
        format.findAllTypesForFile(types, bundle.getFullPathName());
        expectEquals(types.size(), 1);
        if (types.isEmpty()) {
            return;
        }
        EnabledChainPlugin testPlugin;
        testPlugin.description = *types.getFirst();
        testPlugin.descriptorRef.name = testPlugin.description.name;

        beginTest("A plugin that fails after others were created fails the whole instantiation");
        {
            const auto partial = library.instantiateChains({testPlugin, missingPlugin("Second Effect")}, 2);
            expect(partial.errorMessage.isNotEmpty());
            expectEquals(partial.failedPluginName, juce::String("Second Effect"));
            expect(partial.chains.empty());
        }

        beginTest("Every worker gets its own chain, and the first one records each plugin's initial state");
        {
            const auto created = library.instantiateChains({testPlugin, testPlugin}, 2);
            expect(created.errorMessage.isEmpty(), created.errorMessage);
            expectEquals(static_cast<int>(created.chains.size()), 2);
            for (const auto& chain : created.chains) {
                expectEquals(static_cast<int>(chain.size()), 2);
                expect(std::ranges::none_of(chain, [](const auto& instance) { return instance == nullptr; }));
            }
            if (created.chains.size() == 2) {
                expect(created.chains.front().front() != created.chains.back().front());
            }
            expectEquals(static_cast<int>(created.initialStates.size()), 2);
            expect(std::ranges::all_of(created.initialStates, [](const auto& state) { return state.getSize() > 0; }));
        }

        beginTest("A processing run keeps its settings, fills in missing states, and has no more chains than files");
        {
            auto savedPlugin = testPlugin;
            savedPlugin.descriptorRef.state = juce::MemoryBlock("saved", 5);
            PluginProcessingOptions settings;
            settings.overwriteExisting = true;
            const auto run = library.createProcessingRun({testPlugin, savedPlugin}, settings, 1);
            expect(run.has_value(), run.has_value() ? juce::String() : run.error());
            if (run.has_value()) {
                expect(run->options.overwriteExisting);
                expectEquals(static_cast<int>(run->chains.size()), 1);
                expectEquals(static_cast<int>(run->options.plugins.size()), 2);
                if (run->options.plugins.size() == 2) {
                    expect(run->options.plugins.front().state.getSize() > 0);
                    expect(run->options.plugins.back().state == savedPlugin.descriptorRef.state);
                }
            }

            const auto failed = library.createProcessingRun({missingPlugin("Missing Effect")}, settings, 2);
            expect(!failed.has_value());
            expect(!failed.has_value() && failed.error().contains("Missing Effect"));
        }
    }

    /// Copies the WAV tone under each name and returns the analysis records of the copies, which have no outputs.
    std::vector<AudioAnalysisRecord> copySources(
        const TemporaryDirectory& directory,
        const std::vector<juce::String>& names
    )
    {
        std::vector<AudioAnalysisRecord> records;
        for (const auto& copyName : names) {
            records.push_back(analyzeFile(directory.copyFixture("sine_stereo_44100_16.wav", copyName)));
            expect(!getProcessingOutputFile(records.back().file).exists(), copyName);
        }
        return records;
    }

    /// Returns the bytes of every record's file, for checking later that a run left them unchanged.
    std::vector<juce::MemoryBlock> loadContents(const std::vector<AudioAnalysisRecord>& records)
    {
        std::vector<juce::MemoryBlock> contents;
        for (const auto& record : records) {
            expect(record.file.loadFileAsData(contents.emplace_back()), record.fileName);
        }
        return contents;
    }

    /// Checks that every source still has its original bytes and that no output was written for it.
    void expectUnchanged(
        const std::vector<AudioAnalysisRecord>& records,
        const std::vector<juce::MemoryBlock>& originals
    )
    {
        for (const auto& [record, original] : std::views::zip(records, originals)) {
            juce::MemoryBlock remaining;
            expect(record.file.loadFileAsData(remaining), record.fileName);
            expect(remaining == original, record.fileName);
            expect(!getProcessingOutputFile(record.file).exists(), record.fileName);
        }
    }

    /// Waits until the coordinator has moved on from the given run, and returns false on a timeout.
    /// Safe to call from a helper thread, since it reports nothing to the test runner itself.
    static bool waitForRunIdChange(const PluginProcessingCoordinator& coordinator, const int runId)
    {
        const auto deadline = std::chrono::steady_clock::now() + coordinatorTimeout;
        while (coordinator.getCurrentRunId() == runId && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return coordinator.getCurrentRunId() != runId;
    }
};

/// Creates the plugin processing coordinator suite.
std::unique_ptr<juce::UnitTest> createPluginCoordinatorTests()
{
    return std::make_unique<PluginCoordinatorTests>();
}
}  // namespace audiobatch::tests
