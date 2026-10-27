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

#include <algorithm>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch
{
/// Resolves rows still marked pending or active after a run completes,
/// from the cache when possible and otherwise as failed, so no row is left waiting.
/// Never decodes audio, because it runs on the message thread.
void AudioBatchComponent::reconcilePendingAnalysisResults()
{
    const auto selectedPaths = getSelectedRecordPaths();
    bool resultsChanged = false;

    for (int row = 0; row < analysisResults.size(); ++row) {
        const auto& record = analysisResults[row];
        const auto activity = fileActivity.getActivity(record.fullPath);
        const auto needsRefresh = activity == FileActivity::analyzing || activity == FileActivity::waiting
            || record.status == AudioAnalysisStatus::pending;

        if (!needsRefresh) {
            continue;
        }

        resultsChanged = true;

        if (auto refreshedRecord = analysisCache.getAnalysis(record.file)) {
            analysisResults.setRecord(row, std::move(*refreshedRecord));
            continue;
        }

        auto failed = record;
        failed.status = AudioAnalysisStatus::failed;
        failed.errorMessage = failed.file.existsAsFile() ? "Analysis did not finish. Re-analyze the file to try again."
                                                         : "File does not exist";
        utils::logWarn("Analysis failed for {}: {}", failed.fullPath.quoted(), failed.errorMessage);
        analysisResults.setRecord(row, std::move(failed));
    }

    fileActivity.clearActiveWithActivities({FileActivity::analyzing, FileActivity::waiting});

    if (resultsChanged) {
        refreshTablePreservingSelection(selectedPaths);

        if (const auto previewFile = preview.getFile(); previewFile.existsAsFile()) {
            if (const auto currentRow = analysisResults.findRow(previewFile.getFullPathName())) {
                updateAudioInfo(analysisResults[*currentRow]);
            }
        }
    }

    resultsTable.repaint();
}

/// Merges a batch of analysis results into the table on the message thread,
/// preserving the custom gain set for a file while the file is unchanged on disk,
/// and keeping selection and preview in sync.
/// The whole batch is merged before one sort and table refresh,
/// so a large rescan does not re-sort the table once per file.
void AudioBatchComponent::analysisResultsReceived(std::vector<AudioAnalysisRecord> records)
{
    const auto selectedPaths = getSelectedRecordPaths();
    bool tableChanged = false;
    bool currentFileChanged = false;

    for (auto& record : records) {
        if (!record.file.exists()) {
            continue;
        }

        const auto merged = analysisResults.merge(std::move(record));
        const auto& row = analysisResults[merged.row];

        // A gain set while the file was still waiting may only live in the table,
        // because the cache had no row for this version of the file to store it in yet.
        // The coordinator stored the row before publishing the result, so the gain can be stored now.
        // The cache logs its own failures, and the table keeps the gain either way.
        if (merged.customGainCarriedOver) {
            static_cast<void>(analysisCache.storeCustomGain(row.file, row.customGain));
        }

        currentFileChanged = currentFileChanged || preview.getFile() == row.file;
        tableChanged = true;
    }

    if (tableChanged) {
        refreshTablePreservingSelection(selectedPaths);
    }

    if (currentFileChanged) {
        if (const auto currentRow = analysisResults.findRow(preview.getFile().getFullPathName())) {
            updateAudioInfo(analysisResults[*currentRow]);
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

    const auto merged = analysisResults.replaceAfterOperation(originalPath, newRecord);
    const auto mergedRecord = analysisResults[merged.row];

    // The cache logs its own failures, and a row it could not store or remove is only read again later.
    static_cast<void>(analysisCache.storeAnalysis(mergedRecord));

    // A converted original was moved to the trash, so its cached row is no longer needed.
    if (newRecord.fullPath != originalPath) {
        static_cast<void>(analysisCache.removeAnalysis(originalFile));
    }

    refreshTablePreservingSelection(selectedPaths);

    if (preview.getFile() == originalFile) {
        showAudioFile(newRecord.file);
        updateAudioInfo(mergedRecord);
        updateGainControlsForSelection();
        updateThumbnailDisplayGain();
    } else if (preview.getFile() == newRecord.file) {
        updateAudioInfo(mergedRecord);
    }
}

/// Applies one normalization result on the message thread.
/// Replaces the record, since the output path may differ from the source,
/// updates the cache, and reloads the preview when the current file was normalized.
void AudioBatchComponent::normalizeResultReceived(const AudioNormalizationResult& result)
{
    const auto reportFailure
        = [this](const juce::String& fileName, const juce::String& path, const juce::String& error) {
              backgroundWork.recordFailure(utils::format("{}: {}", fileName, error));
              fileActivity.setMessage(path, {.text = error, .isError = true});
              resultsTable.repaint();
          };

    std::visit(
        Overloaded {
            [&](const OperationFailed& failed) {
                // A cancelled file was left unchanged, so its row stays as it was.
                if (failed.cancelled) {
                    resultsTable.repaint();
                } else {
                    reportFailure(result.fileName, result.fullPath, failed.error);
                }
            },
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

/// Reports the end of the normalization pass and any accumulated failures to the user.
void AudioBatchComponent::normalizeCompleted(const NormalizingWork& pass, const int totalFiles)
{
    const auto& failures = pass.failures;
    utils::logInfo(
        "Normalization {}: {:.2f} s, {} files ({} failed)",
        pass.cancelling ? "cancelled" : "complete",
        utils::elapsedSecondsSince(pass.startedAtMilliseconds),
        totalFiles,
        failures.size()
    );

    utils::showFailureSummary(failures, "Normalize", "normalized", this);

    if (!preview.getFile().existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateStatusLabel();
    if (pass.cancelling) {
        statusBar.setStatusText("Normalization cancelled");
    }
}

/// Finalizes an analysis run on the message thread:
/// reconciles rows still pending, logs run statistics, and restores a sensible selection.
void AudioBatchComponent::analysisCompleted(const AnalyzingWork& run, const int totalFiles)
{
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
    } else if (!preview.getFile().existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        const juce::ScopedValueSetter<bool> automaticSelection(automaticSelectionChange, true);
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateProcessButtonState();
}

/// Starts normalization to the selected peak or loudness target for the selected files.
void AudioBatchComponent::normalizeSelectedRecords()
{
    normalizeRecords(getSelectedNormalizableRecords());
}

/// Asks for confirmation, then normalizes every analyzed file in the list whose format can be normalized,
/// like Normalize All Files in the Rust version.
void AudioBatchComponent::normalizeAllRecords()
{
    auto records = getAllReadyRecords();
    std::erase_if(records, [](const AudioAnalysisRecord& record) { return !canNormalizeRecords({record}); });

    if (records.empty()) {
        statusBar.setStatusText("No analyzed files to normalize");
        return;
    }

    const auto message = utils::format(
        "Normalize {} {} to {}?\n\nEach file is written as AIFF next to its source, "
        "and converted sources are moved to the system trash.",
        records.size(),
        records.size() == 1 ? "file" : "files",
        previewWorkspace.normalizationTargetControl.describeTarget()
    );
    const SafePointer safeThis(this);
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::WarningIcon, "Normalize All Files", message, "Normalize", "Cancel", this
        ),
        [safeThis, confirmedRecords = std::move(records)](const int result) {
            if (safeThis != nullptr && result == 1) {
                safeThis->normalizeRecords(confirmedRecords);
            }
        }
    );
}

/// Starts normalization to the selected peak or loudness target for the given files
/// after checking that no other pass is running and that every format is supported.
void AudioBatchComponent::normalizeRecords(const std::vector<AudioAnalysisRecord>& recordsToNormalize)
{
    if (const auto blockedReason = getNormalizationBlockedReason(backgroundWork.getWork()); blockedReason.isNotEmpty())
    {
        utils::logError("Normalization not started: {}", blockedReason);
        statusBar.setStatusText(blockedReason);
        return;
    }

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
        statusBar.setStatusText("Selected file type cannot be normalized in this build");
        return;
    }

    juce::Array<juce::File> filesToNormalize;
    for (const auto& record : recordsToNormalize) {
        filesToNormalize.add(record.file);

        if (record.file == preview.getFile()) {
            clearCurrentAudioPreview();
        }
    }

    auto normalizationOptions = previewWorkspace.normalizationTargetControl.getOptions();
    normalizationOptions.overwriteExisting = previewWorkspace.forceOverwriteToggle.getToggleState();
    if (backgroundWork.startNormalization(recordsToNormalize, normalizationOptions) <= 0) {
        updateStatusLabel();
        return;
    }

    // Each file switches to normalizing when a worker picks it up.
    fileActivity.markActive(filesToNormalize, FileActivity::waiting);

    updateStatusLabel();
}

/// Kicks off a background analysis run.
/// Pending placeholder rows for the files that are not already cached are added once the plan arrives.
/// A run that keeps the results replaces a running one and takes over its inputs.
/// Refuses to start while normalization or plugin processing is running,
/// since the analysis would read files that are being replaced or moved to the trash.
void AudioBatchComponent::startAnalysis(
    const juce::Array<juce::File>& inputPaths,
    const bool recursive,
    const bool forceRefresh,
    const AnalysisRows rows
)
{
    if (const auto blockedReason = getFileOperationBlockedReason(backgroundWork.getWork()); blockedReason.isNotEmpty())
    {
        statusBar.setStatusText(blockedReason);
        return;
    }

    if (rows == AnalysisRows::replace) {
        analysisResults.clear();
        fileActivity.clearAllActive();
        fileActivity.clearAllMessages();
        clearCurrentAudioPreview();
        previewWorkspace.audioInfo.clear();
        resultsTable.updateContent();
        updateResultsTableColumnWidths();
        resultsTable.repaint();
        updateCurrentRootLabel();
    }

    statusBar.setStatusText("Scanning for audio files...");
    backgroundWork.startAnalysis(inputPaths, recursive, forceRefresh, rows != AnalysisRows::add);
    updateProcessButtonState();
}

/// Adds pending rows for the files the run analyzes and shows the progress text.
/// A run whose files make up the whole list first drops the rows of the files it did not find,
/// such as files deleted since the last scan.
/// Called once per run on the message thread, after the coordinator has scanned the input folders.
void AudioBatchComponent::analysisPlanReceived(const AnalysisPlan& plan, const AnalyzingWork& run)
{
    if (run.replacesResults) {
        std::set<juce::String> foundPaths;
        for (const auto& file : plan.files) {
            foundPaths.insert(file.getFullPathName());
        }
        juce::StringArray goneRows;
        for (const auto& record : analysisResults) {
            if (!foundPaths.contains(record.fullPath)) {
                goneRows.add(record.fullPath);
            }
        }
        removeRecordsByPath(goneRows, 0);
    }

    if (run.expected == 0) {
        statusBar.setStatusText(run.replacesResults ? "No supported audio files found" : "No supported dropped files");
        updateProcessButtonState();
        return;
    }

    for (const auto& file : plan.staleFiles) {
        analysisResults.addPending(file);
    }

    analysisResults.sort(sortState);
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    fileActivity.markActive(plan.staleFiles, FileActivity::waiting);
    updatePreviewLoudnessHistory();

    statusBar.setStatusText(utils::format("Analyzing {}/{}", run.completed, run.expected));
}

/// True while normalization or plugin processing is rewriting files.
bool AudioBatchComponent::isFileOperationInProgress() const noexcept
{
    return backgroundWork.isFileOperationInProgress();
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

/// Stops playback and releases the preview file handles when the previewed file is about to be replaced.
/// Returns the files of the given records.
juce::Array<juce::File> AudioBatchComponent::releasePreviewBeforeProcessing(
    const std::vector<AudioAnalysisRecord>& records
)
{
    // Always stop playback before processing, since files may be replaced on disk.
    previewPlayer.stop();
    previewWorkspace.startStopButton.setColour(juce::TextButton::buttonColourId, CustomLookAndFeel::green());

    juce::Array<juce::File> filesToProcess;
    for (const auto& record : records) {
        filesToProcess.add(record.file);
    }

    if (!filesToProcess.contains(preview.getFile())) {
        return filesToProcess;
    }

    // Release file handles so the on-disk file can be replaced.
    // The released preview keeps its file,
    // so the result handler can load the produced output when processing completes.
    releasePreviewPlayback();

    return filesToProcess;
}

/// Runs the plugin chain over the currently selected files.
void AudioBatchComponent::processSelectedRecords()
{
    const auto records = getSelectedProcessableRecords();
    if (records.empty()) {
        statusBar.setStatusText("Select files that can be processed");
        return;
    }
    processRecords(records);
}

/// Asks for confirmation, then runs the plugin chain over every analyzed file in the list that can be processed,
/// like Process All Files in the Rust version.
void AudioBatchComponent::processAllRecords()
{
    if (liveChain->getNumEnabledValidSlots() == 0) {
        showPluginProcessingWarning("The plugin chain is empty. Use the Plugins button to add plugins.");
        return;
    }

    auto records = getAllReadyRecords();
    std::erase_if(records, [](const AudioAnalysisRecord& record) { return !canProcessFileWithPlugins(record.file); });

    if (records.empty()) {
        statusBar.setStatusText("No analyzed files to process");
        return;
    }

    const auto message = utils::format(
        "Process {} {} with the plugin chain ({})?\n\nEach file is written as 16-bit AIFF next to its source, "
        "and converted sources are moved to the system trash.",
        records.size(),
        records.size() == 1 ? "file" : "files",
        liveChain->getChainSummary()
    );
    const SafePointer safeThis(this);
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::WarningIcon, "Process All Files", message, "Process", "Cancel", this
        ),
        [safeThis, confirmedRecords = std::move(records)](const int result) {
            if (safeThis != nullptr && result == 1) {
                safeThis->processRecords(confirmedRecords);
            }
        }
    );
}

/// Runs the plugin chain over the given files.
void AudioBatchComponent::processRecords(const std::vector<AudioAnalysisRecord>& records)
{
    if (const auto blockedReason = getProcessingBlockedReason(backgroundWork.getWork()); blockedReason.isNotEmpty()) {
        statusBar.setStatusText(blockedReason);
        return;
    }

    // Captures live state from any open editors so the run uses the latest tweaks.
    const auto enabledPlugins = liveChain->getEnabledPlugins();
    if (enabledPlugins.empty()) {
        showPluginProcessingWarning("The plugin chain is empty. Use the Plugins button to add plugins.");
        return;
    }

    if (records.empty()) {
        return;
    }

    PluginProcessingOptions options;
    options.plugins
        = enabledPlugins | std::views::transform(&EnabledChainPlugin::descriptorRef) | std::ranges::to<std::vector>();
    if (previewWorkspace.normalizeBeforePluginToggle.getToggleState()) {
        options.normalizeBeforePlugin = previewWorkspace.normalizationTargetControl.getOptions();
    }
    options.overwriteExisting = previewWorkspace.forceOverwriteToggle.getToggleState();

    // Pre-instantiate one chain of plugin instances per worker on the message thread.
    const auto workerCount = juce::jmin(static_cast<int>(records.size()), getPluginProcessingWorkerCount());

    const auto startedAtMilliseconds = juce::Time::getMillisecondCounterHiRes();
    statusBar.setStatusText("Loading plugins...");

    auto [chains, initialStates, errorMessage, failedPluginName]
        = pluginLibrary->instantiateChains(enabledPlugins, workerCount);

    if (chains.empty() || errorMessage.isNotEmpty()) {
        // All-or-nothing: a chain missing any plugin must not run at all.
        showPluginProcessingWarning(
            utils::format(
                "Could not load plugin {}:\n\n{}",
                failedPluginName.quoted(),
                errorMessage.isNotEmpty() ? errorMessage : juce::String("Unknown error")
            )
        );
        statusBar.setStatusText("Plugin load failed");
        return;
    }

    // A plugin without saved state is reset to its initial state before every file instead.
    for (std::size_t index = 0; index < options.plugins.size(); ++index) {
        if (auto& state = options.plugins[index].state; state.isEmpty()) {
            state = initialStates[index];
        }
    }

    const auto filesToProcess = releasePreviewBeforeProcessing(records);

    const auto expected = backgroundWork.startProcessing(records, options, std::move(chains), startedAtMilliseconds);
    updateProcessButtonState();

    if (expected <= 0) {
        updateStatusLabel();
        return;
    }

    // Each file switches to processing when a worker picks it up with a free plugin chain.
    fileActivity.markActive(filesToProcess, FileActivity::waiting);
    updateStatusLabel();
}

/// Applies one plugin processing result on the message thread.
/// Replaces the record, drops stale cache rows and persisted gain, and reloads the preview when needed.
void AudioBatchComponent::processingResultReceived(const PluginProcessingResult& result)
{
    const auto reportFailure = [&](const juce::String& fileName, const juce::String& path, const juce::String& error) {
        backgroundWork.recordFailure(utils::format("{}: {}", fileName, error));
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
                // A cancelled file was left unchanged, so its row stays as it was.
                if (failed.cancelled) {
                    resultsTable.repaint();
                } else {
                    reportFailure(result.fileName, result.originalFullPath, failed.error);
                }
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

/// Reports the end of the plugin processing pass and any accumulated failures to the user.
void AudioBatchComponent::processingCompleted(const ProcessingWork& pass, const int totalFiles)
{
    const auto& failures = pass.failures;
    utils::logInfo(
        "Plugin processing {}: {:.2f} s, {} files ({} failed)",
        pass.cancelling ? "cancelled" : "complete",
        utils::elapsedSecondsSince(pass.startedAtMilliseconds),
        totalFiles,
        failures.size()
    );

    updateProcessButtonState();

    utils::showFailureSummary(failures, "Plugin Processing", "processed", this);

    if (!preview.getFile().existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateStatusLabel();
    if (pass.cancelling) {
        statusBar.setStatusText("Plugin processing cancelled");
    }
}

/// Shows why a plugin processing pass could not start its workers.
void AudioBatchComponent::processingStartFailed(const juce::String& errorMessage)
{
    statusBar.setStatusText(errorMessage);
    showPluginProcessingWarning(errorMessage);
}
}  // namespace audiobatch
