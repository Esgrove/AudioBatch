/// AudioBatchComponent member functions for user actions on the results list:
/// keyboard shortcuts, the file context menu, revealing files, moving them to the trash,
/// and removing, clearing, or re-analyzing rows.

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponent.h"
#include "gui/AudioBatchComponentConstants.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/AudioFileTableModel.h"
#include "gui/components/AudioInfoPanel.h"
#include "normalization/AudioNormalization.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include <unordered_set>

#include <optional>
#include <utility>

namespace audiobatch
{
/// Forwards key presses from the results table to the shared list and preview shortcuts.
bool AudioBatchComponent::keyPressed(const juce::KeyPress& key, juce::Component* /*originator*/)
{
    return keyPressed(key);
}

/// Shares list navigation and cue jumps between the table and waveform without moving keyboard focus.
/// Also handles the application shortcuts for trash, remove, reveal, play/stop, and rescan.
/// Command or Ctrl with Backspace moves the selection to the trash,
/// as does Ctrl with Delete on Windows. Shift skips the confirmation.
/// Delete or Backspace without those modifiers removes it from the list.
bool AudioBatchComponent::keyPressed(const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    if (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey)) {
        return resultsTable.keyPressed(key);
    }

    if (previewWorkspace.thumbnail.keyPressed(key)) {
        return true;
    }

#if JUCE_WINDOWS
    const auto isTrashKey
        = key.getKeyCode() == juce::KeyPress::backspaceKey || key.getKeyCode() == juce::KeyPress::deleteKey;
#else
    const auto isTrashKey = key.getKeyCode() == juce::KeyPress::backspaceKey;
#endif

    if (isTrashKey && (modifiers.isCommandDown() || modifiers.isCtrlDown()) && !modifiers.isAltDown()) {
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
        previewPlayer.togglePlayback();
        return true;
    }

    if (key == juce::KeyPress::F5Key) {
        rescanCurrentRoot();
        return true;
    }

    return false;
}

/// Removes every analyzed file from the table without touching the files on disk.
/// Stops any running analysis, normalization, or processing first,
/// since their results would add rows back. Files a run already committed stay on disk.
void AudioBatchComponent::clearAllRecords()
{
    if (analysisResults.empty() && backgroundWork.isIdle()) {
        return;
    }

    // Ending the run here drops the results that were already queued too.
    backgroundWork.stopAllWork();

    analysisResults.clear();
    fileActivity.clearAllActive();
    fileActivity.clearAllMessages();
    clearCurrentAudioPreview();
    previewWorkspace.audioInfo.clear();

    resultsTable.deselectAllRows();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    resultsTable.repaint();
    statusBar.setStatusText("No files");
    updateProcessButtonState();
}

/// Cancels the running analysis, normalization, or processing pass, from the Cancel button or the menu.
/// Files that a pass has not committed yet keep their rows as they were.
void AudioBatchComponent::cancelCurrentWork()
{
    const bool wasAnalyzing = backgroundWork.isAnalysisInProgress();
    if (!backgroundWork.cancelCurrentWork()) {
        return;
    }

    if (wasAnalyzing) {
        // A cancelled analysis ends at once without a completion, so its waiting rows are settled here.
        utils::logInfo("Analysis cancelled");
        reconcilePendingAnalysisResults();
        updateStatusLabel();
        statusBar.setStatusText("Analysis cancelled");
        return;
    }

    updateStatusLabel();
}

/// Returns true while an analysis, normalization, or processing pass is running.
bool AudioBatchComponent::isBackgroundWorkRunning() const noexcept
{
    return !backgroundWork.isIdle();
}

/// Returns the normalization target as display text, for the menu items that name it.
juce::String AudioBatchComponent::describeNormalizationTarget() const
{
    return previewWorkspace.normalizationTargetControl.describeTarget();
}

/// Removes the currently selected files from the table without touching the files on disk.
/// Files that are being normalized or processed stay in the table.
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

/// Forces re-analysis of the currently selected files, bypassing the analysis cache.
/// Refused while analysis, normalization, or plugin processing is running.
void AudioBatchComponent::reanalyzeSelectedRecords()
{
    if (!backgroundWork.isIdle()) {
        return;
    }

    const auto selectedFiles = getSelectedRecordFiles();

    if (selectedFiles.isEmpty()) {
        return;
    }

    startAnalysis(selectedFiles, false, true, AnalysisRows::add);
}

/// Shows which file types can be normalized in the current build.
void AudioBatchComponent::showSupportedNormalizationFormats()
{
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOk(
            juce::MessageBoxIconType::InfoIcon, "Normalization Format Support", getFormatSupportSummary(), "OK", this
        ),
        nullptr
    );
}

/// Selects the clicked row and opens the per-file action menu.
void AudioBatchComponent::handleFileContextMenuRequested(const int row, const juce::MouseEvent& event)
{
    if (!analysisResults.containsRow(row)) {
        return;
    }

    if (!resultsTable.isRowSelected(row)) {
        resultsTable.selectRow(row, true, true);
    }

    showFileContextMenu(row, event.getScreenPosition());
}

/// Shows the context menu for the given row at the supplied screen position.
void AudioBatchComponent::showFileContextMenu(const int row, const juce::Point<int> screenPosition)
{
    if (!analysisResults.containsRow(row)) {
        return;
    }

    const auto& record = analysisResults[row];
    const auto parentDirectory = record.file.getParentDirectory();
    const auto selectedRecords = getSelectedNormalizableRecords();
    const auto canNormalize = backgroundWork.isIdle() && canNormalizeRecords(selectedRecords);

    juce::PopupMenu menu;
    const auto addItem = [&menu](const FileMenuItemId item, const juce::String& text, const bool isEnabled) {
        menu.addItem(std::to_underlying(item), text, isEnabled);
    };
    addItem(FileMenuItemId::revealFile, utils::revealInFileManagerLabel(), record.file.exists());
    addItem(FileMenuItemId::openParentDirectory, "Open Parent Folder", parentDirectory.isDirectory());
    menu.addSeparator();
    addItem(FileMenuItemId::reanalyze, "Re-analyze Selected", backgroundWork.isIdle());
    addItem(
        FileMenuItemId::normalize,
        utils::format("Normalize to {}", previewWorkspace.normalizationTargetControl.describeTarget()),
        canNormalize
    );
    addItem(FileMenuItemId::normalizeSupport, "Normalization Format Support...", true);
    menu.addSeparator();
    const bool canProcess = backgroundWork.isIdle() && liveChain->getNumEnabledValidSlots() > 0;
    addItem(FileMenuItemId::process, "Process With Plugin Chain (output AIFF)", canProcess);
    // Every selected row counts, since a gain can be cleared from a row that cannot be normalized.
    const auto selectedRows = resultsTable.getSelectedRows();
    bool anyHasGain {false};
    for (int index = 0; index < selectedRows.size() && !anyHasGain; ++index) {
        const auto rowNumber = selectedRows[index];
        anyHasGain = analysisResults.containsRow(rowNumber) && analysisResults[rowNumber].customGain.has_value();
    }
    addItem(FileMenuItemId::clearCustomGain, "Clear Per-file Gain", anyHasGain);
    menu.addSeparator();
    addItem(FileMenuItemId::removeFromList, "Remove from List", true);
    addItem(FileMenuItemId::moveToTrash, "Move to Trash", record.file.exists());

    const auto safeThis = SafePointer(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetScreenArea(juce::Rectangle(screenPosition.x, screenPosition.y, 1, 1))
            .withParentComponent(this),
        // The file is captured instead of the row, because results arriving while the menu is open re-sort the table.
        [safeThis, file = record.file](const int result) {
            const auto item = fileMenuItemFromId(result);
            if (safeThis == nullptr || !item.has_value()) {
                return;
            }

            switch (*item) {
                case FileMenuItemId::revealFile:
                    utils::revealFileInFileManager(file);
                    break;
                case FileMenuItemId::openParentDirectory:
                    utils::revealParentDirectory(file);
                    break;
                case FileMenuItemId::normalize:
                    safeThis->normalizeSelectedRecords();
                    break;
                case FileMenuItemId::normalizeSupport:
                    safeThis->showSupportedNormalizationFormats();
                    break;
                case FileMenuItemId::moveToTrash:
                    safeThis->moveSelectedRecordsToTrash(true);
                    break;
                case FileMenuItemId::removeFromList:
                    safeThis->removeSelectedRecords();
                    break;
                case FileMenuItemId::reanalyze:
                    safeThis->reanalyzeSelectedRecords();
                    break;
                case FileMenuItemId::process:
                    safeThis->processSelectedRecords();
                    break;
                case FileMenuItemId::clearCustomGain:
                    safeThis->previewWorkspace.gainSlider.setValue(0.0, juce::dontSendNotification);
                    safeThis->applyCustomGainToSelection(std::nullopt);
                    break;
            }
        }
    );
}

/// Reveals the file of the selected row in the platform file manager.
/// With several rows selected, the row shown in the info panel is used.
/// Does nothing when no row is selected.
void AudioBatchComponent::revealSelectedRecord() const
{
    const auto selectedRows = resultsTable.getSelectedRows();

    if (const auto row = getSelectionDisplayRow(selectedRows, -1); analysisResults.containsRow(row)) {
        utils::revealFileInFileManager(analysisResults[row].file);
    }
}

/// Moves the selected files to the OS trash, optionally asking for confirmation first.
/// The rows of the files that were trashed are removed from the table.
/// Files that are being normalized or processed are skipped,
/// because the running operation would write its output back to the same path.
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

        if (!analysisResults.containsRow(rowNumber)) {
            continue;
        }

        const auto& record = analysisResults[rowNumber];

        // The running operation would write its output back to the path of a file being rewritten.
        if (!record.file.exists() || isFileBeingRewritten(record.fullPath)) {
            continue;
        }

        filesToTrash.add(record.file);
        removedPaths.add(record.fullPath);
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
            if (safeThis == nullptr || result != 1) {
                return;
            }
            safeThis->runMoveToTrash(filesToTrash, removedPaths, fallbackRow);
        }
    );
}

/// Executes the trash operation and reports any failures back to the user.
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
        const auto isPreviewFile = file == preview.getFile();

        if (isPreviewFile) {
            releasePreviewPlayback();
        }

        if (utils::moveToTrash(file) == utils::TrashResult::failed) {
            failedPaths.add(file.getFullPathName());
            if (isPreviewFile) {
                showAudioFile(file);
            }
            continue;
        }

        if (isPreviewFile) {
            clearCurrentAudioPreview();
        }

        if (juce::isPositiveAndBelow(index, removedPaths.size())) {
            successfullyRemovedPaths.add(removedPaths[index]);
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

/// Removes trashed files from the table and keeps selection and preview state coherent.
/// Clears the activity labels and messages of the removed files,
/// and while analysis runs, makes sure their results do not add the rows back.
void AudioBatchComponent::removeRecordsByPath(const juce::StringArray& removedPaths, const int fallbackRow)
{
    if (removedPaths.isEmpty()) {
        return;
    }

    const std::unordered_set<juce::String> removedPathSet(removedPaths.begin(), removedPaths.end());
    juce::StringArray remainingSelectedPaths;
    for (const auto& path : getSelectedRecordPaths()) {
        if (!removedPathSet.contains(path)) {
            remainingSelectedPaths.add(path);
        }
    }

    const auto previewFile = preview.getFile();
    const auto currentFileRemoved = previewFile.exists() && removedPathSet.contains(previewFile.getFullPathName());

    int removedPendingCount = 0;

    for (const auto& removedPath : removedPaths) {
        const auto activity = fileActivity.getActivity(removedPath);
        const auto row = analysisResults.findRow(removedPath);
        const auto pending = activity == FileActivity::waiting || activity == FileActivity::analyzing
            || (row.has_value() && analysisResults[*row].status == AudioAnalysisStatus::pending);

        if (pending) {
            ++removedPendingCount;
        }

        fileActivity.clearActive(removedPath);
        fileActivity.clearMessage(removedPath);
    }

    analysisResults.removePaths(removedPaths);
    backgroundWork.forgetRemovedPaths(removedPaths, removedPendingCount);

    if (currentFileRemoved) {
        clearCurrentAudioPreview();
    }

    // The old row numbers point at different files now, and updateContent would report them as the selection.
    resultsTable.setSelectedRows({}, juce::dontSendNotification);
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(remainingSelectedPaths);

    if (resultsTable.getNumSelectedRows() == 0 && !analysisResults.empty()) {
        const juce::ScopedValueSetter<bool> automaticSelection(automaticSelectionChange, true);
        resultsTable.selectRow(juce::jlimit(0, analysisResults.size() - 1, fallbackRow));
    }

    if (analysisResults.empty()) {
        previewWorkspace.audioInfo.clear();
    }

    updateStatusLabel();
    resultsTable.repaint();
    updateProcessButtonState();
}
}  // namespace audiobatch
