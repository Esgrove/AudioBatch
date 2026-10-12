/// AudioBatchComponent member functions for user actions on the results list:
/// keyboard shortcuts, the file context menu, revealing files, moving them to the trash,
/// and removing, clearing, or re-analyzing rows.

#include "AudioBatchComponent.h"
#include "AudioBatchComponentConstants.h"
#include "AudioInfoPanel.h"
#include "FileUtils.h"
#include "Log.h"
#include "StringFormat.h"

#include <algorithm>

using namespace audiobatch::gui;

bool AudioBatchComponent::keyPressed(const juce::KeyPress& key, juce::Component* /*originator*/)
{
    return keyPressed(key);
}

bool AudioBatchComponent::keyPressed(const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();

    // Cmd-Backspace is the Finder shortcut on macOS, and Ctrl-Backspace works on every platform.
    if (key.getKeyCode() == juce::KeyPress::backspaceKey && (modifiers.isCommandDown() || modifiers.isCtrlDown())) {
        moveSelectedRecordsToTrash(!modifiers.isShiftDown());
        return true;
    }

    if ((key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey)
        && !modifiers.isCommandDown() && !modifiers.isCtrlDown() && !modifiers.isAltDown())
    {
        removeSelectedRecords();
        return true;
    }

    if (key == juce::KeyPress('r', juce::ModifierKeys::commandModifier, 0)) {
        revealSelectedRecord();
        return true;
    }

    if (key == juce::KeyPress::spaceKey) {
        startOrStop();
        return true;
    }

    if (key == juce::KeyPress::F5Key) {
        refreshAnalysis(true);
        return true;
    }

    return false;
}

void AudioBatchComponent::clearAllRecords()
{
    if (analysisResults.empty()) {
        return;
    }

    if (const auto blockedReason = getFileOperationBlockedReason(); blockedReason.isNotEmpty()) {
        statusLabel.setText(blockedReason, juce::dontSendNotification);
        return;
    }

    // Results of a run still in progress would add the rows back.
    if (analysisRunning) {
        analysisCoordinator.cancelAndWait();
        analysisRunId = -1;
        analysisRunning = false;
    }

    pathsRemovedDuringAnalysis.clear();
    analysisResults.clear();
    fileActivity.clearAllActive();
    fileActivity.clearAllMessages();
    clearCurrentAudioPreview();
    audioInfo->clear();
    expectedResults = 0;
    completedResults = 0;
    analyzedFilesThisRun = 0;

    resultsTable.deselectAllRows();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    resultsTable.repaint();
    statusLabel.setText("No files", juce::dontSendNotification);
    updateProcessButtonState();
}

void AudioBatchComponent::removeSelectedRecords()
{
    auto selectedPaths = getSelectedRecordPaths();
    // A file being rewritten keeps its row, which the running operation updates with its output.
    for (int index = selectedPaths.size(); --index >= 0;) {
        if (isFileBeingRewritten(selectedPaths[index])) {
            selectedPaths.remove(index);
        }
    }

    if (selectedPaths.isEmpty()) {
        return;
    }

    const auto selectedRows = resultsTable.getSelectedRows();
    const int fallbackRow = selectedRows.isEmpty() ? 0 : selectedRows[0];

    removeRecordsByPath(selectedPaths, fallbackRow);
}

void AudioBatchComponent::reanalyzeSelectedRecords()
{
    if (isAnalysisInProgress() || isFileOperationInProgress()) {
        return;
    }

    const auto selectedFiles = getSelectedRecordFiles();

    if (selectedFiles.isEmpty()) {
        return;
    }

    startAnalysis(selectedFiles, false, true, false);
}

void AudioBatchComponent::showSupportedNormalizationFormats()
{
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOk(
            juce::MessageBoxIconType::InfoIcon,
            "Normalization Format Support",
            AudioNormalizationService::getFormatSupportSummary(),
            "OK",
            this
        ),
        nullptr
    );
}

void AudioBatchComponent::handleFileContextMenuRequested(const int row, int columnId, const juce::MouseEvent& event)
{
    juce::ignoreUnused(columnId);

    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    if (!resultsTable.isRowSelected(row)) {
        resultsTable.selectRow(row, true, true);
    }

    showFileContextMenu(row, event.getScreenPosition());
}

void AudioBatchComponent::showFileContextMenu(int row, const juce::Point<int> screenPosition)
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(row)];
    const auto parentDirectory = record.file.getParentDirectory();
    const auto selectedRecords = getSelectedNormalizableRecords();
    const auto canNormalize = !normalizeInProgress && !pluginProcessingInProgress && !isAnalysisInProgress()
        && canNormalizeRecords(selectedRecords);

    juce::PopupMenu menu;
    menu.addItem(revealFileMenuItemId, utils::revealInFileManagerLabel(), record.file.exists());
    menu.addItem(openParentDirectoryMenuItemId, "Open Parent Folder", parentDirectory.isDirectory());
    menu.addSeparator();
    menu.addItem(reanalyzeMenuItemId, "Re-analyze Selected", !isAnalysisInProgress() && !isFileOperationInProgress());
    menu.addItem(
        normalizeMenuItemId, utils::format("Normalize to {}", normalizationTargetControl.describeTarget()), canNormalize
    );
    menu.addItem(normalizeSupportMenuItemId, "Normalization Format Support...");
    menu.addSeparator();
    const bool canProcess = !pluginProcessingInProgress && !isAnalysisInProgress() && !normalizeInProgress
        && pluginChain != nullptr && pluginChain->getNumEnabledValidSlots() > 0;
    menu.addItem(processMenuItemId, "Process With Plugin Chain (output AIFF)", canProcess);
    const bool anyHasGain = std::ranges::any_of(selectedRecords, [](const AudioAnalysisRecord& selectedRecord) {
        return selectedRecord.hasCustomGain;
    });
    menu.addItem(clearCustomGainMenuItemId, "Clear Per-file Gain", anyHasGain);
    menu.addSeparator();
    menu.addItem(removeFromListMenuItemId, "Remove from List");
    menu.addItem(moveToTrashMenuItemId, "Move to Trash", record.file.exists());

    const auto safeThis = SafePointer(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetScreenArea(juce::Rectangle(screenPosition.x, screenPosition.y, 1, 1))
            .withParentComponent(this),
        // The file is captured instead of the row,
        // because results arriving while the menu is open re-sort the table.
        [safeThis, file = record.file](const int result) {
            if (safeThis == nullptr) {
                return;
            }

            switch (result) {
                case revealFileMenuItemId:
                    revealFileInFileManager(file);
                    break;
                case openParentDirectoryMenuItemId:
                    revealParentDirectory(file);
                    break;
                case normalizeMenuItemId:
                    safeThis->normalizeSelectedRecords();
                    break;
                case normalizeSupportMenuItemId:
                    safeThis->showSupportedNormalizationFormats();
                    break;
                case moveToTrashMenuItemId:
                    safeThis->moveSelectedRecordsToTrash(true);
                    break;
                case removeFromListMenuItemId:
                    safeThis->removeSelectedRecords();
                    break;
                case reanalyzeMenuItemId:
                    safeThis->reanalyzeSelectedRecords();
                    break;
                case processMenuItemId:
                    safeThis->processSelectedRecords();
                    break;
                case clearCustomGainMenuItemId:
                    safeThis->gainSlider.setValue(0.0, juce::dontSendNotification);
                    safeThis->applyCustomGainToSelection(0.0f, false);
                    break;
                default:
                    break;
            }
        }
    );
}

void AudioBatchComponent::revealSelectedRecord()
{
    const auto selectedRows = resultsTable.getSelectedRows();

    if (const auto row = getSelectionDisplayRow(selectedRows, -1);
        juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size())))
    {
        revealFileInFileManager(analysisResults[static_cast<std::size_t>(row)].file);
    }
}

void AudioBatchComponent::revealFileInFileManager(const juce::File& file)
{
    if (file.exists()) {
        file.revealToUser();
    } else {
        file.getParentDirectory().revealToUser();
    }
}

void AudioBatchComponent::revealParentDirectory(const juce::File& file)
{
    if (const auto parentDirectory = file.getParentDirectory(); parentDirectory.isDirectory()) {
        parentDirectory.revealToUser();
    }
}

void AudioBatchComponent::moveSelectedRecordsToTrash(const bool promptForConfirmation)
{
    const auto selectedRows = resultsTable.getSelectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    juce::Array<juce::File> filesToTrash;
    juce::StringArray removedPaths;

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        const auto& record = analysisResults[static_cast<std::size_t>(rowNumber)];

        // The running operation would write its output back to the path of a file being rewritten.
        if (!record.file.exists() || isFileBeingRewritten(record.fullPath)) {
            continue;
        }

        filesToTrash.add(record.file);
        removedPaths.addIfNotAlreadyThere(record.fullPath);
    }

    if (filesToTrash.isEmpty()) {
        return;
    }

    const auto fileCount = filesToTrash.size();
    const auto message = fileCount == 1 ? utils::format("Move\n\n{}\n\nto the system trash?", filesToTrash.getFirst())
                                        : utils::format("Move {} selected files to the system trash?", fileCount);

    if (!promptForConfirmation) {
        runMoveToTrash(filesToTrash, removedPaths, selectedRows[0]);
        return;
    }

    const auto safeThis = SafePointer(this);
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::WarningIcon, "Move to Trash", message, "Move to Trash", "Cancel", this
        ),
        [safeThis, filesToTrash, removedPaths, fallbackRow = selectedRows[0]](const int result) {
            if (safeThis == nullptr || result == 0) {
                return;
            }
            safeThis->runMoveToTrash(filesToTrash, removedPaths, fallbackRow);
        }
    );
}

void AudioBatchComponent::runMoveToTrash(
    const juce::Array<juce::File>& filesToTrash,
    const juce::StringArray& removedPaths,
    const int fallbackRow
)
{
    juce::StringArray failedPaths;
    juce::StringArray successfullyRemovedPaths;

    for (int index = 0; index < filesToTrash.size(); ++index) {
        const auto& file = filesToTrash.getReference(index);

        if (!utils::moveToTrash(file)) {
            failedPaths.add(file.getFullPathName());
            continue;
        }

        if (file == currentAudioFile) {
            clearCurrentAudioPreview();
        }

        if (juce::isPositiveAndBelow(index, removedPaths.size())) {
            successfullyRemovedPaths.addIfNotAlreadyThere(removedPaths[index]);
        }
    }

    removeRecordsByPath(successfullyRemovedPaths, fallbackRow);

    if (!failedPaths.isEmpty()) {
        for (const auto& failedPath : failedPaths) {
            utils::logError("Move to trash failed for {}", failedPath.quoted());
        }

        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Move to Trash Failed",
                failedPaths.size() == 1
                    ? utils::format("Could not move this file to the system trash:\n\n{}", failedPaths[0])
                    : utils::format("Could not move {} files to the system trash.", failedPaths.size()),
                "OK",
                this
            ),
            nullptr
        );
    }
}

void AudioBatchComponent::removeRecordsByPath(const juce::StringArray& removedPaths, const int fallbackRow)
{
    if (removedPaths.isEmpty()) {
        return;
    }

    auto remainingSelectedPaths = getSelectedRecordPaths();
    for (int index = remainingSelectedPaths.size(); --index >= 0;) {
        if (removedPaths.contains(remainingSelectedPaths[index])) {
            remainingSelectedPaths.remove(index);
        }
    }

    const auto currentFileRemoved
        = currentAudioFile.exists() && removedPaths.contains(currentAudioFile.getFullPathName());

    int removedPendingCount = 0;

    for (const auto& removedPath : removedPaths) {
        if (analysisRunning) {
            const auto activeLabel = fileActivity.getActiveLabel(removedPath);
            const auto rowIndex = findRecordIndex(removedPath);
            const auto pending = activeLabel == "Waiting" || activeLabel == "Analyzing"
                || (rowIndex >= 0
                    && analysisResults[static_cast<std::size_t>(rowIndex)].status == AudioAnalysisStatus::pending);

            if (pending) {
                ++removedPendingCount;
            }

            // A carried over run could deliver a result for a finished row again, so every removed path is kept.
            pathsRemovedDuringAnalysis.insert(removedPath);
        }

        fileActivity.clearActive(removedPath);
        fileActivity.clearMessage(removedPath);
    }

    analysisResults.erase(
        std::ranges::remove_if(
            analysisResults,
            [&removedPaths](const AudioAnalysisRecord& record) { return removedPaths.contains(record.fullPath); }
        ).begin(),
        analysisResults.end()
    );

    // Only rows still waiting for a result lower the expected count, since finished rows were already counted.
    expectedResults = juce::jmax(0, expectedResults - removedPendingCount);
    completedResults = juce::jmin(completedResults, expectedResults);

    if (currentFileRemoved) {
        clearCurrentAudioPreview();
    }

    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(remainingSelectedPaths);

    if (resultsTable.getNumSelectedRows() == 0 && !analysisResults.empty()) {
        resultsTable.selectRow(juce::jlimit(0, static_cast<int>(analysisResults.size()) - 1, fallbackRow));
    }

    if (analysisResults.empty()) {
        audioInfo->clear();
    }

    updateStatusLabel();
    resultsTable.repaint();
    updateProcessButtonState();
}
