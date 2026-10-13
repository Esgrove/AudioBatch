/// AudioBatchComponent member functions that run and finish the background operations:
/// analysis runs and their plans, normalization, and plugin processing,
/// including the result handlers that update the table, cache, and preview.

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisFormatting.h"
#include "gui/AudioBatchComponent.h"
#include "gui/AudioBatchComponentConstants.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/components/CustomLookAndFeel.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>

namespace audiobatch
{
/// Copies the custom gain of the record in the table onto a new record for the same path,
/// when the file still has the size and modification time the gain was set for.
/// A custom gain only applies to the version of the file it was set for, so a changed file starts without one.
/// Returns true when the gain was carried over.
static bool keepCustomGainOfUnchangedFile(const AudioAnalysisRecord& existing, AudioAnalysisRecord& incoming)
{
    if (!existing.hasCustomGain || existing.fileSize != incoming.fileSize
        || existing.modifiedTimeMs != incoming.modifiedTimeMs)
    {
        return false;
    }

    incoming.customGainDb = existing.customGainDb;
    incoming.hasCustomGain = true;
    return true;
}

/// Formats a bounded list of failures for an alert dialog,
/// truncating with a "...and N more" line so the dialog stays readable.
static juce::String buildFailureListMessage(const juce::StringArray& failures)
{
    /// Most failures listed before the rest are summarized.
    constexpr int maximumListedFailures = 8;
    juce::StringArray lines;
    const auto linesToShow = juce::jmin(maximumListedFailures, failures.size());

    for (int index = 0; index < linesToShow; ++index) {
        lines.add(utils::format("- {}", failures[index]));
    }

    if (failures.size() > linesToShow) {
        lines.add(utils::format("- ...and {} more", failures.size() - linesToShow));
    }

    // The message is only shown inside the app, so plain '\n' line breaks are fine.
    return lines.joinIntoString("\n");
}

/// Shows a warning listing the files that failed during a normalization or processing pass.
/// A single failure is shown as is, and several are summarized with buildFailureListMessage.
/// Does nothing when there were no failures.
static void showFailureSummary(
    const juce::StringArray& failures,
    const juce::String& title,
    const juce::String& failedAction,
    juce::Component* associatedComponent
)
{
    if (failures.isEmpty()) {
        return;
    }

    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOk(
            juce::MessageBoxIconType::WarningIcon,
            title,
            failures.size() == 1
                ? failures[0]
                : utils::format("Some files could not be {}:\n\n{}", failedAction, buildFailureListMessage(failures)),
            "OK",
            associatedComponent
        ),
        nullptr
    );
}

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

        AudioAnalysisRecord refreshedRecord;

        if (analysisCache.getAnalysis(record.file, refreshedRecord)) {
            record = std::move(refreshedRecord);
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

    analysisRun.completed = analysisRun.expected;

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

/// Merges one analysis result into the table on the message thread,
/// preserving the custom gain set for the file while the file is unchanged on disk,
/// and keeping selection and preview in sync.
/// Results from a replaced run are ignored, since their queued messages can arrive after a new run started.
void AudioBatchComponent::handleAnalysisResult(const AudioAnalysisRecord& record, const int runId)
{
    if (runId != analysisRun.runId) {
        return;
    }

    fileActivity.clearActive(record.fullPath);

    // The expected count already went down when the row was removed.
    if (analysisRun.removedPaths.contains(record.fullPath)) {
        updateStatusLabel();
        return;
    }

    ++analysisRun.completed;

    if (!record.file.exists()) {
        updateStatusLabel();
        return;
    }

    const auto selectedPaths = getSelectedRecordPaths();

    auto mergedRecord = record;
    if (const auto existingIndex = findRecordIndex(record.fullPath); existingIndex >= 0) {
        // A gain set while the file was still waiting may only live in the table,
        // because the cache had no row for this version of the file to store it in yet.
        // The coordinator stored the row before publishing the result, so the gain can be stored now.
        if (keepCustomGainOfUnchangedFile(analysisResults[static_cast<std::size_t>(existingIndex)], mergedRecord)
            && (!record.hasCustomGain || !juce::approximatelyEqual(record.customGainDb, mergedRecord.customGainDb)))
        {
            analysisCache.storeCustomGain(mergedRecord.file, mergedRecord.customGainDb, true);
        }

        analysisResults[static_cast<std::size_t>(existingIndex)] = mergedRecord;
    } else {
        analysisResults.push_back(mergedRecord);
    }

    refreshTablePreservingSelection(selectedPaths);

    if (currentAudioFile == mergedRecord.file) {
        updateAudioInfo(mergedRecord);
        updateGainControlsForSelection();
        updateThumbnailDisplayGain();
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

    analysisCache.storeAnalysis(mergedRecord);

    // A converted original was moved to the trash, so its cached row is no longer needed.
    if (newRecord.fullPath != originalPath) {
        analysisCache.removeAnalysis(originalFile);
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
    ++normalizeProgress.completed;
    fileActivity.clearActive(result.fullPath);

    if (result.succeeded && result.notice.isNotEmpty()) {
        fileActivity.setMessage(result.analysisRecord.fullPath, {.text = result.notice, .isError = false});
    }

    // A committed output replaces its source even when its re-analysis failed,
    // since a converted source is already in the trash.
    // A successful in-place no-op is applied too, since it carries a refreshed analysis of a changed file.
    if (result.committed || result.succeeded) {
        replaceRecordAfterOperation(result.file, result.analysisRecord);
    }

    if (!result.succeeded) {
        normalizeProgress.failures.add(utils::format("{}: {}", result.fileName, result.errorMessage));
        const auto& failedPath = result.committed ? result.analysisRecord.fullPath : result.fullPath;
        fileActivity.setMessage(failedPath, {.text = result.errorMessage, .isError = true});
        resultsTable.repaint();
    }

    updateStatusLabel();
}

/// Ends the normalization pass and reports any accumulated failures to the user.
void AudioBatchComponent::handleNormalizeComplete(const int totalFiles)
{
    normalizeProgress.expected = totalFiles;
    normalizeProgress.inProgress = false;

    utils::logInfo(
        "{:8.2f} s: Normalization complete: {} succeeded, {} failed ({} files)",
        (juce::Time::getMillisecondCounterHiRes() - normalizeProgress.startedAtMilliseconds) / 1000.0,
        totalFiles - normalizeProgress.failures.size(),
        normalizeProgress.failures.size(),
        totalFiles
    );

    showFailureSummary(normalizeProgress.failures, "Normalize", "normalized", this);

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
    if (runId != analysisRun.runId) {
        return;
    }

    analysisRun.running = false;
    analysisRun.expected = juce::jmin(analysisRun.expected, totalFiles);
    reconcilePendingAnalysisResults();

    // The plan handler already reported that no files were found.
    if (totalFiles == 0) {
        updateProcessButtonState();
        return;
    }

    updateStatusLabel();

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - analysisRun.startedAtMs;
    const auto cachedFileCount = juce::jmax(0, totalFiles - analysisRun.analyzedFiles);
    const auto failedFileCount
        = std::ranges::count_if(analysisResults, [](const auto& record) { return record.hasError(); });
    utils::logInfo(
        "Analysis complete: loaded {} files ({} analyzed, {} from cache, {} failed) in {:.2f} s",
        totalFiles,
        analysisRun.analyzedFiles,
        cachedFileCount,
        failedFileCount,
        elapsedMs / millisecondsPerSecond
    );

    if (resultsTable.getSelectedRow() < 0 && !analysisResults.empty()) {
        resultsTable.selectRow(0);
    } else if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateProcessButtonState();
}

/// Starts normalization to the selected peak or loudness target for the selected files
/// after checking that no other pass is running and that every selected format is supported.
void AudioBatchComponent::normalizeSelectedRecords()
{
    if (normalizeProgress.inProgress) {
        utils::logError("Normalization requested while another normalization pass is already in progress");
        statusLabel.setText("Normalization already in progress", juce::dontSendNotification);
        return;
    }

    if (processingProgress.inProgress) {
        utils::logError("Normalization requested while plugin processing is in progress");
        statusLabel.setText("Wait for plugin processing to finish before normalizing", juce::dontSendNotification);
        return;
    }

    if (isAnalysisInProgress()) {
        utils::logError("Normalization requested before analysis finished");
        statusLabel.setText("Wait for analysis to finish before normalizing", juce::dontSendNotification);
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

    normalizeProgress.failures.clear();
    normalizeProgress.completed = 0;
    normalizeProgress.inProgress = true;
    normalizeProgress.startedAtMilliseconds = juce::Time::getMillisecondCounterHiRes();
    auto normalizationOptions = normalizationTargetControl.getOptions();
    normalizationOptions.overwriteExisting = forceOverwriteToggle.getToggleState();
    normalizeProgress.expected = normalizeCoordinator.start(recordsToNormalize, normalizationOptions);

    if (normalizeProgress.expected <= 0) {
        normalizeProgress.inProgress = false;
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

    // Starting a run cancels the running one, whose placeholder rows would then never get a result.
    if (!clearResults && analysisRun.running) {
        for (const auto& path : analysisRun.options.inputPaths) {
            options.inputPaths.addIfNotAlreadyThere(path);
        }

        options.recursive = options.recursive || analysisRun.options.recursive;
        options.refresh = options.refresh || analysisRun.options.refresh;

        // A file the user adds again is wanted back in the table.
        for (const auto& path : inputPaths) {
            analysisRun.removedPaths.erase(path.getFullPathName());
        }
    } else {
        analysisRun.removedPaths.clear();
    }

    analysisRun.options = options;

    analysisRun.startedAtMs = juce::Time::getMillisecondCounterHiRes();
    analysisRun.completed = 0;
    analysisRun.expected = 0;
    analysisRun.replacesResults = clearResults;
    analysisRun.running = true;
    analysisRun.planReceived = false;
    statusLabel.setText("Scanning for audio files...", juce::dontSendNotification);

    if (clearResults) {
        updateCurrentRootLabel();
    }

    analysisRun.runId = analysisCoordinator.start(options);
    updateProcessButtonState();
}

/// Adds pending rows for the files the run analyzes and shows the progress text.
/// Called once per run on the message thread, after the coordinator has scanned the input folders.
void AudioBatchComponent::handleAnalysisPlan(const AnalysisPlan& plan)
{
    if (plan.runId != analysisRun.runId) {
        return;
    }

    analysisRun.planReceived = true;
    analysisRun.expected = plan.files.size();
    analysisRun.analyzedFiles = plan.staleFiles.size();
    utils::logInfo("Found {} audio files", analysisRun.expected);

    if (analysisRun.expected == 0) {
        statusLabel.setText(
            analysisRun.replacesResults ? "No supported audio files found" : "No supported dropped files",
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

    statusLabel.setText(
        utils::format("Analyzing {}/{}", analysisRun.completed, analysisRun.expected), juce::dontSendNotification
    );
}

/// True from the start of an analysis run until the coordinator reports its completion.
bool AudioBatchComponent::isAnalysisInProgress() const
{
    return analysisRun.running;
}

/// True while normalization or plugin processing is rewriting files.
bool AudioBatchComponent::isFileOperationInProgress() const noexcept
{
    return normalizeProgress.inProgress || processingProgress.inProgress;
}

/// Returns the reason analysis or a change to the list cannot happen right now
/// because files are being normalized or processed, or an empty string when it can.
juce::String AudioBatchComponent::getFileOperationBlockedReason() const
{
    if (normalizeProgress.inProgress) {
        return "Normalization in progress";
    }

    if (processingProgress.inProgress) {
        return "Plugin processing in progress";
    }

    return {};
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
    if (processingProgress.inProgress) {
        return "Processing already in progress";
    }

    if (normalizeProgress.inProgress) {
        return "Wait for normalization to finish before processing";
    }

    if (isAnalysisInProgress()) {
        return "Wait for analysis to finish before processing";
    }

    return {};
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

    if (pluginChain == nullptr) {
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
    options.plugins.reserve(enabledPlugins.size());
    for (const auto& [description, descriptorRef] : enabledPlugins) {
        options.plugins.push_back(descriptorRef);
    }
    if (normalizeBeforePluginToggle.getToggleState()) {
        options.normalizeBeforePlugin = normalizationTargetControl.getOptions();
    }
    options.overwriteExisting = forceOverwriteToggle.getToggleState();

    // Pre-instantiate one chain of plugin instances per worker on the message thread.
    const auto workerCount
        = juce::jlimit(1, 4, juce::jmin(static_cast<int>(records.size()), juce::SystemStats::getNumCpus()));

    processingProgress.startedAtMilliseconds = juce::Time::getMillisecondCounterHiRes();
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

    processingProgress.failures.clear();
    processingProgress.completed = 0;
    processingProgress.inProgress = true;
    updateProcessButtonState();

    processingProgress.expected = pluginCoordinator.start(records, options, std::move(chains));

    if (processingProgress.expected <= 0) {
        processingProgress.inProgress = false;
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
    ++processingProgress.completed;
    fileActivity.clearActive(result.originalFullPath);

    if (result.succeeded && result.notice.isNotEmpty()) {
        fileActivity.setMessage(result.analysisRecord.fullPath, {.text = result.notice, .isError = false});
    }

    // A committed output replaces its source even when its re-analysis failed,
    // since a converted source is already in the trash.
    if (result.committed) {
        replaceRecordAfterOperation(result.originalFile, result.analysisRecord);
        // The custom gain is baked into the processed audio, so the new output starts without one.
        analysisCache.storeCustomGain(result.analysisRecord.file, 0.0f, false);
    }

    if (!result.succeeded) {
        processingProgress.failures.add(utils::format("{}: {}", result.fileName, result.errorMessage));
        const auto& failedPath = result.committed ? result.analysisRecord.fullPath : result.originalFullPath;
        fileActivity.setMessage(failedPath, {.text = result.errorMessage, .isError = true});
        resultsTable.repaint();
        utils::logError("Plugin processing failed for {}: {}", result.originalFullPath.quoted(), result.errorMessage);
    }

    updateStatusLabel();
}

/// Ends the plugin processing pass and reports any accumulated failures to the user.
void AudioBatchComponent::handleProcessingComplete(const int totalFiles)
{
    processingProgress.expected = totalFiles;
    processingProgress.inProgress = false;

    utils::logInfo(
        "{:8.2f} s: Plugin processing complete: {} succeeded, {} failed ({} files)",
        (juce::Time::getMillisecondCounterHiRes() - processingProgress.startedAtMilliseconds) / 1000.0,
        totalFiles - processingProgress.failures.size(),
        processingProgress.failures.size(),
        totalFiles
    );

    if (pluginChain != nullptr) {
        updateProcessButtonState();
    }

    showFailureSummary(processingProgress.failures, "Plugin Processing", "processed", this);

    if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateStatusLabel();
}
}  // namespace audiobatch
