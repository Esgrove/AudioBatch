/// Runs the CLI's processing workflow with plugin lifecycle work on the main event-loop thread.
/// Rendering uses the GUI's coordinator, preserving the common writer, identity, metadata, and cache contracts.

#include "cli/PluginProcessCli.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysisTypes.h"
#include "cli/AudioAnalysisCli.h"
#include "normalization/AudioNormalization.h"
#include "plugins/PluginChainPresets.h"
#include "plugins/PluginLibrary.h"
#include "plugins/PluginProcessing.h"
#include "plugins/PluginProcessingCoordinator.h"
#include "utils/AppInfo.h"
#include "utils/ConsoleProgress.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <exception>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch::cli
{
namespace
{
/// Interval for cooperative interruption while processing workers finish.
constexpr int INTERRUPT_POLL_MILLISECONDS {20};

/// Resolves a named native preset or an explicit file without converting preset formats.
std::expected<std::vector<SavedChainSlot>, juce::String> loadChain(const juce::String& preset)
{
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(preset);
    if (file.existsAsFile() || preset.containsAnyOf("/\\") || preset.endsWithIgnoreCase(".xml")) {
        return plugin_presets::loadPreset(file, true);
    }
    const auto directory
        = plugin_presets::getPresetDirectory(utils::createApplicationSettingsOptions().getDefaultFile());
    const auto named = plugin_presets::getPresetFile(directory, preset);
    if (!named) {
        return std::unexpected(utils::format("The preset {} does not exist", preset.quoted()));
    }
    return plugin_presets::loadPreset(*named, true);
}
}  // namespace

/// Runs discovery and reporting on a worker while the main thread dispatches plugin lifecycle and native events.
/// Joining happens only after the worker posted completion, including all queued plugin teardown.
int runProcess(const ProcessCommand& command, const utils::CancellationCheck& isInterrupted)
{
    const juce::ScopedJuceInitialiser_GUI initialiser;
    int exitCode {1};
    std::exception_ptr exception;
    std::jthread worker([&] {
        try {
            AnalysisCache cache;
            exitCode = runAnalysis(command.analysis, cache, isInterrupted, &command);
        } catch (...) {
            exception = std::current_exception();
        }
        juce::MessageManager::callAsync([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    worker.join();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    return exitCode;
}

/// Creates chains on the message thread, renders through the shared coordinator, and returns completed outcomes.
/// Cancellation drains committed results and destroys the coordinator and its chains on the message thread.
std::expected<std::map<juce::String, AudioNormalizationResult>, juce::String> processWithProgress(
    const std::vector<AudioAnalysisRecord>& records,
    const ProcessCommand& command,
    AnalysisCache& cache,
    const utils::CancellationCheck& isInterrupted
)
{
    auto slots = loadChain(command.preset);
    if (!slots) {
        return std::unexpected(slots.error());
    }
    std::vector<EnabledChainPlugin> enabled;
    for (const auto& slot : *slots) {
        if (slot.enabled) {
            PluginDescriptorRef descriptor;
            descriptor.state = slot.state;
            descriptor.identifierString = slot.description.createIdentifierString();
            descriptor.name = slot.description.name;
            descriptor.manufacturer = slot.description.manufacturerName;
            descriptor.pluginFormatName = slot.description.pluginFormatName;
            enabled.push_back({.description = slot.description, .descriptorRef = std::move(descriptor)});
        }
    }
    if (enabled.empty()) {
        return std::unexpected("The plugin chain has no enabled plugins");
    }
    std::map<juce::String, AudioNormalizationResult> results;
    if (records.empty() || utils::isCancellationRequested(isInterrupted)) {
        return results;
    }
    console::ProgressLine progress;
    progress.setTotal(static_cast<int>(records.size()));
    std::mutex mutex;
    juce::WaitableEvent completed;
    std::unique_ptr<PluginProcessingCoordinator> coordinator;
    juce::String error;
    // Every exit, including a plugin exception during setup, tears down chains on the event-loop thread.
    const juce::ScopeGuard releaseCoordinator {
        [&] { static_cast<void>(utils::callOnMessageThread([&] { coordinator.reset(); }, {})); },
    };
    const bool started = utils::callOnMessageThread(
        [&] {
            juce::ApplicationProperties properties;
            properties.setStorageParameters(utils::createApplicationSettingsOptions());
            const PluginLibrary library(properties);
            auto setup = library.createProcessingRun(
                enabled, command.options, static_cast<int>(records.size()), command.analysis.workerCount
            );
            if (!setup) {
                error = setup.error();
                completed.signal();
                return;
            }
            coordinator = std::make_unique<PluginProcessingCoordinator>(command.analysis.workerCount);
            coordinator->setResultPersistenceCallback([&](PluginProcessingResult& result) {
                persistOperationOutput(cache, result.outcome);
            });
            coordinator->setStartingCallback([&](const juce::File& file, int) {
                progress.setMessage(file.getFileName());
            });
            coordinator->setResultCallback([&](const PluginProcessingResult& result, int) {
                const auto* failed = std::get_if<OperationFailed>(&result.outcome);
                if (failed == nullptr || !failed->cancelled) {
                    const std::scoped_lock lock(mutex);
                    results.insert_or_assign(
                        result.originalFullPath, AudioNormalizationResult::forFile(result.originalFile, result.outcome)
                    );
                }
                progress.increment();
            });
            coordinator->setCompletionCallback([&](int, int) { completed.signal(); });
            coordinator->setStartErrorCallback([&](juce::String message, int) { error = std::move(message); });
            static_cast<void>(coordinator->start(records, setup->options, std::move(setup->chains)));
        },
        {}
    );
    if (!started) {
        return std::unexpected("The plugin event loop is unavailable");
    }
    while (!completed.wait(INTERRUPT_POLL_MILLISECONDS)) {
        if (utils::isCancellationRequested(isInterrupted)) {
            static_cast<void>(utils::callOnMessageThread(
                [&] {
                    if (coordinator != nullptr) {
                        coordinator->requestStop();
                    }
                },
                {}
            ));
        }
    }
    progress.finish();
    if (error.isNotEmpty()) {
        return std::unexpected(error);
    }
    return results;
}
}  // namespace audiobatch::cli
