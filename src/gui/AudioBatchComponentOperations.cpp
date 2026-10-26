/// AudioBatchComponent member functions that run and finish the background operations:
/// analysis runs and their plans, normalization, and plugin processing,
/// including the result handlers that update the table, cache, and preview.

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisFormatting.h"
#include "gui/AudioBatchComponent.h"
#include "gui/AudioBatchComponentConstants.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/components/CustomLookAndFeel.h"
#include "utils/GuiUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include <unordered_map>

#include <algorithm>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch
{
namespace
{
/// Copies the custom gain of the record in the table onto a new record for the same path,
/// when the file still has the size and modification time the gain was set for.
/// A custom gain only applies to the version of the file it was set for, so a changed file starts without one.
/// Returns true when the gain was carried over.
bool keepCustomGainOfUnchangedFile(const AudioAnalysisRecord& existing, AudioAnalysisRecord& incoming)
{
    if (!existing.customGain || existing.fileSize != incoming.fileSize
        || existing.modifiedTimeMs != incoming.modifiedTimeMs)
    {
        return false;
    }

    incoming.customGain = existing.customGain;
    return true;
}
}  // namespace

/// Resolves rows still marked pending or active after a run completes,
/// from the cache when possible and otherwise as failed, so no row is left waiting.
/// Never decodes audio, because it runs on the message thread.
void AudioBatchComponent::reconcilePendingAnalysisResults()
{
    const auto selectedPaths = getSelectedRecordPaths();
    bool resultsChanged = false;

    for (auto& record : analysisResults) {
        const auto activity = fileActivity.getActivity(record.fullPath);
        const auto needsRefresh = activity == FileActivity::analyzing || activity == FileActivity::waiting
            || record.status == AudioAnalysisStatus::pending;

        if (!needsRefresh) {
            continue;
        }

        if (auto refreshedRecord = analysisCache.getAnalysis(record.file)) {
            record = std::move(*refreshedRecord);
            resultsChanged = true;
            continue;
        }

        record.status = AudioAnalysisStatus::failed;
        record.errorMessage = record.file.existsAsFile() ? "Analysis did not finish. Re-analyze the file to try again."
                                                         : "File does not exist";
        utils::logWarn("Analysis failed for {}: {}", record.fullPath.quoted(), record.errorMessage);
        resultsChanged = true;
    }

    fileActivity.clearActiveWithActivities({FileActivity::analyzing, FileActivity::waiting});

    if (resultsChanged) {
        refreshTablePreservingSelection(selectedPaths);

        if (currentAudioFile.existsAsFile()) {
            if (const auto currentIndex = findRecordIndex(currentAudioFile.getFullPathName()); currentIndex >= 0) {
                updateAudioInfo(analysisResults[static_cast<std::size_t>(currentIndex)]);
            }
        }
    }

    resultsTable.repaint();
}

/// Merges a batch of analysis results into the table on the message thread,
/// preserving the custom gain set for a file while the file is unchanged on disk,
/// and keeping selection and preview in sync.
/// The whole batch is merged through one index of the rows by path and shown with one sort and table refresh,
/// so a large rescan no longer re-sorts the table once per file.
/// Results from a replaced run are ignored, since their queued messages can arrive after a new run started.
void AudioBatchComponent::handleAnalysisResults(std::vector<AnalysisResultMessage> messages)
{
    const auto selectedPaths = getSelectedRecordPaths();
    std::unordered_map<juce::String, std::size_t> rowsByPath;
    rowsByPath.reserve(analysisResults.size());

    for (std::size_t row = 0; row < analysisResults.size(); ++row) {
        rowsByPath.try_emplace(analysisResults[row].fullPath, row);
    }

    auto* analyzing = std::get_if<AnalyzingWork>(&backgroundWork);
    if (analyzing == nullptr) {
        return;
    }

    bool tableChanged = false;
    bool currentFileChanged = false;

    for (auto& [record, runId] : messages) {
        if (runId != analyzing->runId) {
            continue;
        }

        fileActivity.clearActive(record.fullPath);

        // The expected count already went down when the row was removed.
        if (analyzing->removedPaths.contains(record.fullPath)) {
            continue;
        }

        ++analyzing->completed;

        if (!record.file.exists()) {
            continue;
        }

        if (const auto existing = rowsByPath.find(record.fullPath); existing != rowsByPath.end()) {
            auto& row = analysisResults[existing->second];
            const auto incomingGain = record.customGain;

            // A gain set while the file was still waiting may only live in the table,
            // because the cache had no row for this version of the file to store it in yet.
            // The coordinator stored the row before publishing the result, so the gain can be stored now.
            // The cache logs its own failures, and the table keeps the gain either way.
            if (keepCustomGainOfUnchangedFile(row, record) && !sameCustomGain(incomingGain, record.customGain)) {
                static_cast<void>(analysisCache.storeCustomGain(record.file, record.customGain));
            }

            row = std::move(record);
            currentFileChanged = currentFileChanged || currentAudioFile == row.file;
        } else {
            currentFileChanged = currentFileChanged || currentAudioFile == record.file;
            rowsByPath.try_emplace(record.fullPath, analysisResults.size());
            analysisResults.push_back(std::move(record));
        }

        tableChanged = true;
    }

    if (tableChanged) {
        refreshTablePreservingSelection(selectedPaths);
    }

    if (currentFileChanged) {
        if (const auto currentIndex = findRecordIndex(currentAudioFile.getFullPathName()); currentIndex >= 0) {
            updateAudioInfo(analysisResults[static_cast<std::size_t>(currentIndex)]);
            updateGainControlsForSelection();
            updateThumbnailDisplayGain();
        }
    }

    updateStatusLabel();
    updateProcessButtonState();
}

/// Replaces the row of a file that a normalize or process run rewrote with the record of its output,
/// which may have a new path when the source was converted.
/// Updates the selection, the cache, and the preview to follow the file to its new path.
void AudioBatchComponent::replaceRecordAfterOperation(
    const juce::File& originalFile,
    const AudioAnalysisRecord& newRecord
)
{
    const auto& originalPath = originalFile.getFullPathName();
    auto selectedPaths = getSelectedRecordPaths();

    for (int index = 0; index < selectedPaths.size(); ++index) {
        if (selectedPaths[index] == originalPath) {
            selectedPaths.set(index, newRecord.fullPath);
        }
    }

    // A converted original has a different path than its output, so its row is removed.
    std::erase_if(analysisResults, [&originalPath, &newRecord](const AudioAnalysisRecord& record) {
        return record.fullPath == originalPath && record.fullPath != newRecord.fullPath;
    });

    auto mergedRecord = newRecord;

    if (const auto existingIndex = findRecordIndex(newRecord.fullPath); existingIndex >= 0) {
        // An operation that left the file unchanged, such as normalizing a file already at the target, keeps its gain.
        keepCustomGainOfUnchangedFile(analysisResults[static_cast<std::size_t>(existingIndex)], mergedRecord);
        analysisResults[static_cast<std::size_t>(existingIndex)] = mergedRecord;
    } else {
        analysisResults.push_back(mergedRecord);
    }

    // The cache logs its own failures, and a row it could not store or remove is only read again later.
    static_cast<void>(analysisCache.storeAnalysis(mergedRecord));

    // A converted original was moved to the trash, so its cached row is no longer needed.
    if (newRecord.fullPath != originalPath) {
        static_cast<void>(analysisCache.removeAnalysis(originalFile));
    }

    refreshTablePreservingSelection(selectedPaths);

    if (currentAudioFile == originalFile) {
        currentAudioFile = newRecord.file;
        showAudioResource(juce::URL(currentAudioFile));
        updateAudioInfo(mergedRecord);
        updateGainControlsForSelection();
        updateThumbnailDisplayGain();
    } else if (currentAudioFile == newRecord.file) {
        updateAudioInfo(mergedRecord);
    }
}

/// Applies one normalization result on the message thread.
/// Replaces the record, since the output path may differ from the source,
/// updates the cache, and reloads the preview when the current file was normalized.
void AudioBatchComponent::handleNormalizeResult(const AudioNormalizationResult& result)
{
    auto* normalizing = std::get_if<NormalizingWork>(&backgroundWork);
    if (normalizing != nullptr) {
        ++normalizing->completed;
    }
    fileActivity.clearActive(result.fullPath);

    const auto reportFailure
        = [this, normalizing](const juce::String& fileName, const juce::String& path, const juce::String& error) {
              if (normalizing != nullptr) {
                  normalizing->failures.add(utils::format("{}: {}", fileName, error));
              }
              fileActivity.setMessage(path, {.text = error, .isError = true});
              resultsTable.repaint();
          };

    std::visit(
        Overloaded {
            [&](const OperationFailed& failed) { reportFailure(result.fileName, result.fullPath, failed.error); },
            [&](const CommittedButUnanalyzed& unanalyzed) {
                // A committed output replaces its source even when its re-analysis failed,
                // since a converted source is already in the trash.
                replaceRecordAfterOperation(result.file, unanalyzed.output);
                reportFailure(unanalyzed.output.fileName, unanalyzed.output.fullPath, unanalyzed.error);
            },
            [&](const OperationSucceeded& succeeded) {
                if (succeeded.notice.isNotEmpty()) {
                    fileActivity.setMessage(succeeded.output.fullPath, {.text = succeeded.notice, .isError = false});
                }
                // A successful in-place no-op is applied too, since it carries a refreshed analysis of a changed file.
                replaceRecordAfterOperation(result.file, succeeded.output);
            },
        },
        result.outcome
    );

    updateStatusLabel();
}

/// Ends the normalization pass and reports any accumulated failures to the user.
void AudioBatchComponent::handleNormalizeComplete(const int totalFiles)
{
    // A pass that started no files has already ended, and analysis may run by the time its completion arrives.
    const auto* normalizing = std::get_if<NormalizingWork>(&backgroundWork);
    if (normalizing == nullptr) {
        return;
    }

    const auto failures = normalizing->failures;
    utils::logInfo(
        "Normalization complete: {:.2f} s, {} files ({} succeeded, {} failed)",
        utils::elapsedSecondsSince(normalizing->startedAtMilliseconds),
        totalFiles,
        totalFiles - failures.size(),
        failures.size()
    );
    backgroundWork = IdleWork {};

    utils::showFailureSummary(failures, "Normalize", "normalized", this);

    if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateStatusLabel();
}

/// Finalizes an analysis run on the message thread:
/// reconciles rows still pending, logs run statistics, and restores a sensible selection.
/// Completion of a replaced run is ignored.
void AudioBatchComponent::handleAnalysisComplete(const int totalFiles, const int runId)
{
    if (!isCurrentAnalysisRun(runId)) {
        return;
    }

    // Results are batched separately from this message, so the ones still queued are merged first,
    // otherwise reconciling would mark their rows as unfinished.
    analysisResultBatcher.deliverPending();

    const auto run = std::get<AnalyzingWork>(backgroundWork);
    backgroundWork = IdleWork {};
    reconcilePendingAnalysisResults();

    // The plan handler already reported that no files were found.
    if (totalFiles == 0) {
        updateProcessButtonState();
        return;
    }

    updateStatusLabel();

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - run.startedAtMs;
    const auto cachedFileCount = juce::jmax(0, totalFiles - run.analyzedFiles);
    const auto failedFileCount
        = std::ranges::count_if(analysisResults, [](const auto& record) { return record.hasError(); });
    utils::logInfo(
        "Analysis complete: {:.2f} s, {} files ({} analyzed, {} from cache, {} failed)",
        elapsedMs / utils::millisecondsPerSecond,
        totalFiles,
        run.analyzedFiles,
        cachedFileCount,
        failedFileCount
    );

    if (resultsTable.getSelectedRow() < 0 && !analysisResults.empty()) {
        const juce::ScopedValueSetter<bool> automaticSelection(automaticSelectionChange, true);
        resultsTable.selectRow(0);
    } else if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        const juce::ScopedValueSetter<bool> automaticSelection(automaticSelectionChange, true);
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateProcessButtonState();
}

/// Starts normalization to the selected peak or loudness target for the selected files
/// after checking that no other pass is running and that every selected format is supported.
void AudioBatchComponent::normalizeSelectedRecords()
{
    if (const auto blockedReason = getNormalizationBlockedReason(); blockedReason.isNotEmpty()) {
        utils::logError("Normalization not started: {}", blockedReason);
        statusLabel.setText(blockedReason, juce::dontSendNotification);
        return;
    }

    const auto recordsToNormalize = getSelectedNormalizableRecords();

    if (recordsToNormalize.empty()) {
        return;
    }

    if (!canNormalizeRecords(recordsToNormalize)) {
        utils::logError("Normalization unavailable for the selected files");
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Normalization Unavailable",
                buildNormalizationUnavailableMessage(recordsToNormalize),
                "OK",
                this
            ),
            nullptr
        );
        statusLabel.setText("Selected file type cannot be normalized in this build", juce::dontSendNotification);
        return;
    }

    juce::Array<juce::File> filesToNormalize;
    for (const auto& record : recordsToNormalize) {
        filesToNormalize.add(record.file);

        if (record.file == currentAudioFile) {
            clearCurrentAudioPreview();
        }
    }

    auto normalizationOptions = normalizationTargetControl.getOptions();
    normalizationOptions.overwriteExisting = forceOverwriteToggle.getToggleState();
    auto& normalizing = backgroundWork.emplace<NormalizingWork>();
    normalizing.startedAtMilliseconds = juce::Time::getMillisecondCounterHiRes();
    normalizing.expected = normalizeCoordinator.start(recordsToNormalize, normalizationOptions);

    if (normalizing.expected <= 0) {
        backgroundWork = IdleWork {};
        updateStatusLabel();
        return;
    }

    // Each file switches to normalizing when a worker picks it up.
    fileActivity.markActive(filesToNormalize, FileActivity::waiting);

    updateStatusLabel();
}

/// Kicks off a background analysis run.
/// Pending placeholder rows for the files that are not already cached are added once the plan arrives.
/// A run that keeps the results replaces a running one, so it also takes over the inputs of that run,
/// which would otherwise leave its placeholder rows waiting forever.
/// Refuses to start while normalization or plugin processing is running,
/// since the analysis would read files that are being replaced or moved to the trash.
void AudioBatchComponent::startAnalysis(
    const juce::Array<juce::File>& inputPaths,
    const bool recursive,
    const bool forceRefresh,
    const bool clearResults
)
{
    if (const auto blockedReason = getFileOperationBlockedReason(); blockedReason.isNotEmpty()) {
        statusLabel.setText(blockedReason, juce::dontSendNotification);
        return;
    }

    if (clearResults) {
        analysisResults.clear();
        fileActivity.clearAllActive();
        fileActivity.clearAllMessages();
        clearCurrentAudioPreview();
        audioInfo->clear();
        resultsTable.updateContent();
        updateResultsTableColumnWidths();
        resultsTable.repaint();
    }

    AudioAnalysisOptions options;
    options.inputPaths = inputPaths;
    options.recursive = recursive;
    options.refresh = forceRefresh;

    AnalyzingWork run;

    // Starting a run cancels the running one, whose placeholder rows would then never get a result.
    if (const auto* replaced = std::get_if<AnalyzingWork>(&backgroundWork); replaced != nullptr && !clearResults) {
        for (const auto& path : replaced->options.inputPaths) {
            options.inputPaths.addIfNotAlreadyThere(path);
        }

        options.recursive = options.recursive || replaced->options.recursive;
        options.refresh = options.refresh || replaced->options.refresh;
        run.removedPaths = replaced->removedPaths;

        // A file the user adds again is wanted back in the table.
        for (const auto& path : inputPaths) {
            run.removedPaths.erase(path.getFullPathName());
        }
    }

    run.options = options;
    run.startedAtMs = juce::Time::getMillisecondCounterHiRes();
    run.replacesResults = clearResults;
    statusLabel.setText("Scanning for audio files...", juce::dontSendNotification);

    if (clearResults) {
        updateCurrentRootLabel();
    }

    run.runId = analysisCoordinator.start(options);
    backgroundWork = std::move(run);
    updateProcessButtonState();
}

/// Adds pending rows for the files the run analyzes and shows the progress text.
/// Called once per run on the message thread, after the coordinator has scanned the input folders.
void AudioBatchComponent::handleAnalysisPlan(const AnalysisPlan& plan)
{
    auto* analyzing = std::get_if<AnalyzingWork>(&backgroundWork);
    if (analyzing == nullptr || plan.runId != analyzing->runId) {
        return;
    }

    analyzing->planReceived = true;
    analyzing->expected = plan.files.size();
    analyzing->analyzedFiles = plan.staleFiles.size();
    utils::logInfo("Found {} audio files", analyzing->expected);

    if (analyzing->expected == 0) {
        statusLabel.setText(
            analyzing->replacesResults ? "No supported audio files found" : "No supported dropped files",
            juce::dontSendNotification
        );
        updateProcessButtonState();
        return;
    }

    for (const auto& file : plan.staleFiles) {
        if (findRecordIndex(file.getFullPathName()) >= 0) {
            continue;
        }

        auto placeholder = AudioAnalysisRecord::fromFile(file);
        placeholder.status = AudioAnalysisStatus::pending;
        analysisResults.push_back(std::move(placeholder));
    }

    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    fileActivity.markActive(plan.staleFiles, FileActivity::waiting);
    updatePreviewLoudnessHistory();

    statusLabel.setText(
        utils::format("Analyzing {}/{}", analyzing->completed, analyzing->expected), juce::dontSendNotification
    );
}

/// True from the start of an analysis run until the coordinator reports its completion.
bool AudioBatchComponent::isAnalysisInProgress() const noexcept
{
    return std::holds_alternative<AnalyzingWork>(backgroundWork);
}

/// True while the analysis run with the given id is running,
/// which drops the queued messages of runs that were replaced or have ended.
bool AudioBatchComponent::isCurrentAnalysisRun(const int runId) const noexcept
{
    const auto* analyzing = std::get_if<AnalyzingWork>(&backgroundWork);
    return analyzing != nullptr && analyzing->runId == runId;
}

/// True while no analysis, normalization, or plugin processing is running.
bool AudioBatchComponent::isIdle() const noexcept
{
    return std::holds_alternative<IdleWork>(backgroundWork);
}

/// True while normalization or plugin processing is rewriting files.
bool AudioBatchComponent::isFileOperationInProgress() const noexcept
{
    return std::holds_alternative<NormalizingWork>(backgroundWork)
        || std::holds_alternative<ProcessingWork>(backgroundWork);
}

/// Returns the reason analysis or a change to the list cannot happen right now
/// because files are being normalized or processed, or an empty string when it can.
juce::String AudioBatchComponent::getFileOperationBlockedReason() const
{
    return std::visit(
        Overloaded {
            [](const IdleWork& /*idle*/) { return juce::String(); },
            [](const AnalyzingWork& /*analyzing*/) { return juce::String(); },
            [](const NormalizingWork& /*normalizing*/) { return juce::String("Normalization in progress"); },
            [](const ProcessingWork& /*processing*/) { return juce::String("Plugin processing in progress"); },
        },
        backgroundWork
    );
}

/// Returns the reason a normalization pass cannot start right now because other work is running,
/// or an empty string when a pass may start.
juce::String AudioBatchComponent::getNormalizationBlockedReason() const
{
    return std::visit(
        Overloaded {
            [](const IdleWork& /*idle*/) { return juce::String(); },
            [](const AnalyzingWork& /*analyzing*/) {
                return juce::String("Wait for analysis to finish before normalizing");
            },
            [](const NormalizingWork& /*normalizing*/) { return juce::String("Normalization already in progress"); },
            [](const ProcessingWork& /*processing*/) {
                return juce::String("Wait for plugin processing to finish before normalizing");
            },
        },
        backgroundWork
    );
}

/// True when the file is being normalized or processed right now, or is queued to be.
/// Analysis never runs alongside normalization or processing, so a waiting file during either pass is queued for it.
bool AudioBatchComponent::isFileBeingRewritten(const juce::String& fullPath) const
{
    const auto activity = fileActivity.getActivity(fullPath);
    return activity == FileActivity::normalizing || activity == FileActivity::processing
        || (activity == FileActivity::waiting && isFileOperationInProgress());
}

/// Shows a non-blocking warning dialog titled "Plugin Processing" with the given message.
void AudioBatchComponent::showPluginProcessingWarning(const juce::String& message)
{
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOk(
            juce::MessageBoxIconType::WarningIcon, "Plugin Processing", message, "OK", this
        ),
        nullptr
    );
}

/// Returns the reason a processing run cannot start right now because other work is running,
/// or an empty string when a run may start.
juce::String AudioBatchComponent::getProcessingBlockedReason() const
{
    return std::visit(
        Overloaded {
            [](const IdleWork& /*idle*/) { return juce::String(); },
            [](const AnalyzingWork& /*analyzing*/) {
                return juce::String("Wait for analysis to finish before processing");
            },
            [](const NormalizingWork& /*normalizing*/) {
                return juce::String("Wait for normalization to finish before processing");
            },
            [](const ProcessingWork& /*processing*/) { return juce::String("Processing already in progress"); },
        },
        backgroundWork
    );
}

/// Stops playback and releases the preview file handles when the previewed file is about to be replaced.
/// Returns the files of the given records.
juce::Array<juce::File> AudioBatchComponent::releasePreviewBeforeProcessing(
    const std::vector<AudioAnalysisRecord>& records
)
{
    // Always stop playback before processing, since files may be replaced on disk.
    previewPlayer.stop();
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::green());

    juce::Array<juce::File> filesToProcess;
    for (const auto& record : records) {
        filesToProcess.add(record.file);
    }

    if (!filesToProcess.contains(currentAudioFile)) {
        return filesToProcess;
    }

    // Release file handles so the on-disk file can be replaced.
    // Keep currentAudioFile so the result handler can re-load the produced output when processing completes.
    releasePreviewPlayback();

    return filesToProcess;
}

/// Runs the configured plugin (if any) over the currently-selected files.
void AudioBatchComponent::processSelectedRecords()
{
    if (const auto blockedReason = getProcessingBlockedReason(); blockedReason.isNotEmpty()) {
        statusLabel.setText(blockedReason, juce::dontSendNotification);
        return;
    }

    // Captures live state from any open editors so the run uses the latest tweaks.
    const auto enabledPlugins = pluginChain->getEnabledPlugins();
    if (enabledPlugins.empty()) {
        showPluginProcessingWarning("The plugin chain is empty. Use the Plugins button to add plugins.");
        return;
    }

    const auto records = getSelectedProcessableRecords();
    if (records.empty()) {
        statusLabel.setText("Select files that can be processed", juce::dontSendNotification);
        return;
    }

    PluginProcessingOptions options;
    options.plugins
        = enabledPlugins | std::views::transform(&EnabledChainPlugin::descriptorRef) | std::ranges::to<std::vector>();
    if (normalizeBeforePluginToggle.getToggleState()) {
        options.normalizeBeforePlugin = normalizationTargetControl.getOptions();
    }
    options.overwriteExisting = forceOverwriteToggle.getToggleState();

    // Pre-instantiate one chain of plugin instances per worker on the message thread.
    const auto workerCount = juce::jmin(static_cast<int>(records.size()), getPluginProcessingWorkerCount());

    const auto startedAtMilliseconds = juce::Time::getMillisecondCounterHiRes();
    statusLabel.setText("Loading plugins...", juce::dontSendNotification);

    auto [chains, initialStates, errorMessage, failedPluginName]
        = pluginChain->instantiateChains(enabledPlugins, workerCount);

    if (chains.empty() || errorMessage.isNotEmpty()) {
        // All-or-nothing: a chain missing any plugin must not run at all.
        showPluginProcessingWarning(
            utils::format(
                "Could not load plugin {}:\n\n{}",
                failedPluginName.quoted(),
                errorMessage.isNotEmpty() ? errorMessage : juce::String("Unknown error")
            )
        );
        statusLabel.setText("Plugin load failed", juce::dontSendNotification);
        return;
    }

    // A plugin without saved state is reset to its initial state before every file instead.
    for (std::size_t index = 0; index < options.plugins.size(); ++index) {
        if (auto& state = options.plugins[index].state; state.isEmpty()) {
            state = initialStates[index];
        }
    }

    const auto filesToProcess = releasePreviewBeforeProcessing(records);

    auto& processing = backgroundWork.emplace<ProcessingWork>();
    processing.startedAtMilliseconds = startedAtMilliseconds;
    updateProcessButtonState();

    processing.expected = pluginCoordinator.start(records, options, std::move(chains));

    if (processing.expected <= 0) {
        backgroundWork = IdleWork {};
        updateProcessButtonState();
        updateStatusLabel();
        return;
    }

    // Each file switches to processing when a worker picks it up with a free plugin chain.
    fileActivity.markActive(filesToProcess, FileActivity::waiting);
    updateStatusLabel();
}

/// Applies one plugin processing result on the message thread.
/// Replaces the record, drops stale cache rows and persisted gain, and reloads the preview when needed.
void AudioBatchComponent::handleProcessingResult(const PluginProcessingResult& result)
{
    auto* processing = std::get_if<ProcessingWork>(&backgroundWork);
    if (processing != nullptr) {
        ++processing->completed;
    }
    fileActivity.clearActive(result.originalFullPath);

    const auto reportFailure = [&](const juce::String& fileName, const juce::String& path, const juce::String& error) {
        if (processing != nullptr) {
            processing->failures.add(utils::format("{}: {}", fileName, error));
        }
        fileActivity.setMessage(path, {.text = error, .isError = true});
        resultsTable.repaint();
        utils::logError("Plugin processing failed for {}: {}", result.originalFullPath.quoted(), error);
    };
    // A committed output replaces its source even when its re-analysis failed,
    // since a converted source is already in the trash.
    const auto replaceSource = [&](const AudioAnalysisRecord& output) {
        replaceRecordAfterOperation(result.originalFile, output);
        // The custom gain is baked into the processed audio, so the new output starts without one.
        // The cache logs its own failures, and the row in the table is already correct.
        static_cast<void>(analysisCache.storeCustomGain(output.file, std::nullopt));
    };

    std::visit(
        Overloaded {
            [&](const OperationFailed& failed) {
                reportFailure(result.fileName, result.originalFullPath, failed.error);
            },
            [&](const CommittedButUnanalyzed& unanalyzed) {
                replaceSource(unanalyzed.output);
                reportFailure(unanalyzed.output.fileName, unanalyzed.output.fullPath, unanalyzed.error);
            },
            [&](const OperationSucceeded& succeeded) {
                if (succeeded.notice.isNotEmpty()) {
                    fileActivity.setMessage(succeeded.output.fullPath, {.text = succeeded.notice, .isError = false});
                }
                replaceSource(succeeded.output);
            },
        },
        result.outcome
    );

    updateStatusLabel();
}

/// Ends the plugin processing pass and reports any accumulated failures to the user.
void AudioBatchComponent::handleProcessingComplete(const int totalFiles)
{
    // A pass that started no files or failed to start has already ended.
    const auto* processing = std::get_if<ProcessingWork>(&backgroundWork);
    if (processing == nullptr) {
        return;
    }

    const auto failures = processing->failures;
    utils::logInfo(
        "Plugin processing complete: {:.2f} s, {} files ({} succeeded, {} failed)",
        utils::elapsedSecondsSince(processing->startedAtMilliseconds),
        totalFiles,
        totalFiles - failures.size(),
        failures.size()
    );
    backgroundWork = IdleWork {};

    updateProcessButtonState();

    utils::showFailureSummary(failures, "Plugin Processing", "processed", this);

    if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateStatusLabel();
}
}  // namespace audiobatch
