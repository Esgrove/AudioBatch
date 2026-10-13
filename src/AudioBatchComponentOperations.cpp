/// AudioBatchComponent member functions that run and finish the background operations:
/// analysis runs and their plans, normalization, and plugin processing,
/// including the result handlers that update the table, cache, and preview.

#include "AudioAnalysisService.h"
#include "AudioBatchComponent.h"
#include "AudioBatchComponentConstants.h"
#include "AudioInfoPanel.h"
#include "CustomLookAndFeel.h"
#include "Log.h"
#include "StringFormat.h"

#include <algorithm>

/// Helpers local to the operation handlers.
namespace audiobatch::gui
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
}  // namespace audiobatch::gui

using namespace audiobatch::gui;

bool AudioBatchComponent::canNormalizeRecords(const std::vector<AudioAnalysisRecord>& records)
{
    if (records.empty()) {
        return false;
    }

    return std::ranges::all_of(records, [](const auto& record) {
        return AudioNormalizationService::canNormalizeFile(record.file);
    });
}

juce::String AudioBatchComponent::buildNormalizationUnavailableMessage(const std::vector<AudioAnalysisRecord>& records)
{
    juce::StringArray unsupportedLines;

    for (const auto& record : records) {
        const auto reason = AudioNormalizationService::getNormalizationSupportMessage(record.file);

        if (reason.isEmpty()) {
            continue;
        }

        unsupportedLines.add(
            utils::format("{} ({}): {}", record.fileName, AudioAnalysisService::formatTypeLabel(record), reason)
        );
    }

    if (unsupportedLines.isEmpty()) {
        return {};
    }

    return utils::format(
        "The selected files cannot be normalized with the current build:\n\n{}\n\n{}",
        unsupportedLines.joinIntoString(juce::newLine),
        AudioNormalizationService::getFormatSupportSummary()
    );
}

void AudioBatchComponent::reconcilePendingAnalysisResults()
{
    const auto selectedPaths = getSelectedRecordPaths();
    bool resultsChanged = false;

    for (auto& record : analysisResults) {
        const auto activeStatus = fileActivity.getActiveLabel(record.fullPath);
        const auto needsRefresh
            = activeStatus == "Analyzing" || activeStatus == "Waiting" || record.status == AudioAnalysisStatus::pending;

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

    fileActivity.clearActiveWithLabels({"Analyzing", "Waiting"});

    completedResults = expectedResults;

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

void AudioBatchComponent::handleAnalysisResult(const AudioAnalysisRecord& record, const int runId)
{
    if (runId != analysisRunId) {
        return;
    }

    fileActivity.clearActive(record.fullPath);

    // The expected count already went down when the row was removed.
    if (pathsRemovedDuringAnalysis.contains(record.fullPath)) {
        updateStatusLabel();
        return;
    }

    ++completedResults;

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

void AudioBatchComponent::handleNormalizeResult(const AudioNormalizationResult& result)
{
    ++normalizedResultsCompleted;
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
        normalizationFailures.add(utils::format("{}: {}", result.fileName, result.errorMessage));
        const auto& failedPath = result.committed ? result.analysisRecord.fullPath : result.fullPath;
        fileActivity.setMessage(failedPath, {.text = result.errorMessage, .isError = true});
        resultsTable.repaint();
    }

    updateStatusLabel();
}

void AudioBatchComponent::handleNormalizeComplete(const int totalFiles)
{
    normalizedResultsExpected = totalFiles;
    normalizeInProgress = false;

    if (!normalizationFailures.isEmpty()) {
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Normalize",
                normalizationFailures.size() == 1
                    ? normalizationFailures[0]
                    : utils::format(
                          "Some files could not be normalized:\n\n{}", buildFailureListMessage(normalizationFailures)
                      ),
                "OK",
                this
            ),
            nullptr
        );
    }

    if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateStatusLabel();
}

void AudioBatchComponent::handleAnalysisComplete(const int totalFiles, const int runId)
{
    if (runId != analysisRunId) {
        return;
    }

    analysisRunning = false;
    expectedResults = juce::jmin(expectedResults, totalFiles);
    reconcilePendingAnalysisResults();

    // The plan handler already reported that no files were found.
    if (totalFiles == 0) {
        updateProcessButtonState();
        return;
    }

    updateStatusLabel();

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - analysisStartedAtMs;
    const auto cachedFileCount = juce::jmax(0, totalFiles - analyzedFilesThisRun);
    const auto failedFileCount
        = std::ranges::count_if(analysisResults, [](const auto& record) { return record.hasError(); });
    utils::logInfo(
        "Analysis complete: loaded {} files ({} analyzed, {} from cache, {} failed) in {:.2f} s",
        totalFiles,
        analyzedFilesThisRun,
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

void AudioBatchComponent::normalizeSelectedRecords()
{
    if (normalizeInProgress) {
        utils::logError("Normalization requested while another normalization pass is already in progress");
        statusLabel.setText("Normalization already in progress", juce::dontSendNotification);
        return;
    }

    if (pluginProcessingInProgress) {
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

    normalizationFailures.clear();
    normalizedResultsCompleted = 0;
    normalizeInProgress = true;
    auto normalizationOptions = normalizationTargetControl.getOptions();
    normalizationOptions.overwriteExisting = forceOverwriteToggle.getToggleState();
    normalizedResultsExpected = normalizeCoordinator.start(recordsToNormalize, normalizationOptions);

    if (normalizedResultsExpected <= 0) {
        normalizeInProgress = false;
        updateStatusLabel();
        return;
    }

    fileActivity.markActive(filesToNormalize, "Normalizing");

    updateStatusLabel();
}

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
    if (!clearResults && analysisRunning) {
        for (const auto& path : currentAnalysisOptions.inputPaths) {
            options.inputPaths.addIfNotAlreadyThere(path);
        }

        options.recursive = options.recursive || currentAnalysisOptions.recursive;
        options.refresh = options.refresh || currentAnalysisOptions.refresh;

        // A file the user adds again is wanted back in the table.
        for (const auto& path : inputPaths) {
            pathsRemovedDuringAnalysis.erase(path.getFullPathName());
        }
    } else {
        pathsRemovedDuringAnalysis.clear();
    }

    currentAnalysisOptions = options;

    analysisStartedAtMs = juce::Time::getMillisecondCounterHiRes();
    completedResults = 0;
    expectedResults = 0;
    analysisReplacesResults = clearResults;
    analysisRunning = true;
    analysisPlanReceived = false;
    statusLabel.setText("Scanning for audio files...", juce::dontSendNotification);

    if (clearResults) {
        updateCurrentRootLabel();
    }

    analysisRunId = analysisCoordinator.start(options);
    updateProcessButtonState();
}

void AudioBatchComponent::handleAnalysisPlan(const AnalysisCoordinator::AnalysisPlan& plan)
{
    if (plan.runId != analysisRunId) {
        return;
    }

    analysisPlanReceived = true;
    expectedResults = plan.files.size();
    analyzedFilesThisRun = plan.staleFiles.size();
    utils::logInfo("Found {} audio files", expectedResults);

    if (expectedResults == 0) {
        statusLabel.setText(
            analysisReplacesResults ? "No supported audio files found" : "No supported dropped files",
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
    fileActivity.markActive(plan.staleFiles, "Waiting");

    statusLabel.setText(
        utils::format("Analyzing {}/{}", completedResults, expectedResults), juce::dontSendNotification
    );
}

bool AudioBatchComponent::isAnalysisInProgress() const
{
    return analysisRunning;
}

bool AudioBatchComponent::isFileOperationInProgress() const noexcept
{
    return normalizeInProgress || pluginProcessingInProgress;
}

juce::String AudioBatchComponent::getFileOperationBlockedReason() const
{
    if (normalizeInProgress) {
        return "Normalization in progress";
    }

    if (pluginProcessingInProgress) {
        return "Plugin processing in progress";
    }

    return {};
}

bool AudioBatchComponent::isFileBeingRewritten(const juce::String& fullPath) const
{
    const auto activeLabel = fileActivity.getActiveLabel(fullPath);
    return activeLabel == "Normalizing" || activeLabel == "Processing";
}

void AudioBatchComponent::showPluginProcessingWarning(const juce::String& message)
{
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOk(
            juce::MessageBoxIconType::WarningIcon, "Plugin Processing", message, "OK", this
        ),
        nullptr
    );
}

juce::String AudioBatchComponent::getProcessingBlockedReason() const
{
    if (pluginProcessingInProgress) {
        return "Processing already in progress";
    }

    if (normalizeInProgress) {
        return "Wait for normalization to finish before processing";
    }

    if (isAnalysisInProgress()) {
        return "Wait for analysis to finish before processing";
    }

    return {};
}

juce::Array<juce::File> AudioBatchComponent::releasePreviewBeforeProcessing(
    const std::vector<AudioAnalysisRecord>& records
)
{
    // Always stop playback before processing, since files may be replaced on disk.
    if (transportSource.isPlaying()) {
        transportSource.stop();
    }
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

    processingFailures.clear();
    processedResultsCompleted = 0;
    pluginProcessingInProgress = true;
    updateProcessButtonState();

    processedResultsExpected = pluginCoordinator.start(records, options, std::move(chains));

    if (processedResultsExpected <= 0) {
        pluginProcessingInProgress = false;
        updateProcessButtonState();
        updateStatusLabel();
        return;
    }

    fileActivity.markActive(filesToProcess, "Processing");
    updateStatusLabel();
}

void AudioBatchComponent::handleProcessingResult(const PluginProcessingResult& result)
{
    ++processedResultsCompleted;
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
        processingFailures.add(utils::format("{}: {}", result.fileName, result.errorMessage));
        const auto& failedPath = result.committed ? result.analysisRecord.fullPath : result.originalFullPath;
        fileActivity.setMessage(failedPath, {.text = result.errorMessage, .isError = true});
        resultsTable.repaint();
        utils::logError("Plugin processing failed for {}: {}", result.originalFullPath.quoted(), result.errorMessage);
    }

    updateStatusLabel();
}

void AudioBatchComponent::handleProcessingComplete(const int totalFiles)
{
    processedResultsExpected = totalFiles;
    pluginProcessingInProgress = false;

    if (pluginChain != nullptr) {
        updateProcessButtonState();
    }

    if (!processingFailures.isEmpty()) {
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Plugin Processing",
                processingFailures.size() == 1
                    ? processingFailures[0]
                    : utils::format(
                          "Some files could not be processed:\n\n{}", buildFailureListMessage(processingFailures)
                      ),
                "OK",
                this
            ),
            nullptr
        );
    }

    if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateStatusLabel();
}
