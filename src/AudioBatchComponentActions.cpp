/// AudioBatchComponent member functions for user actions on the results list:
/// keyboard shortcuts, the file context menu, revealing files, moving them to the trash,
/// and removing, clearing, or re-analyzing rows.

#include "AppInfo.h"
#include "AudioAnalysisService.h"
#include "AudioBatchComponent.h"
#include "AudioBatchComponentConstants.h"
#include "AudioInfoPanel.h"
#include "CustomLookAndFeel.h"
#include "FileUtils.h"
#include "Log.h"
#include "PluginProcessingService.h"
#include "ResultsSorting.h"
#include "StringFormat.h"

#include <algorithm>
#include <cstdint>
#include <map>

/// Helpers local to the action handlers.
namespace audiobatch::gui
{
/// Returns the platform-appropriate label for the reveal-in-file-manager menu item.
static juce::String getRevealFileMenuLabel()
{
#if JUCE_MAC
    return "Reveal in Finder";
#elif JUCE_WINDOWS
    return "Reveal in Explorer";
#else
    return "Reveal in File Manager";
#endif
}
}  // namespace audiobatch::gui

using namespace audiobatch::gui;

bool AudioBatchComponent::keyPressed(const juce::KeyPress& key, juce::Component* /*originator*/)
{
    return keyPressed(key);
}

bool AudioBatchComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::backspaceKey && key.getModifiers().isCtrlDown()) {
        moveSelectedRecordsToTrash(!key.getModifiers().isShiftDown());
        return true;
    }

    if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey) {
        removeSelectedRecords();
        return true;
    }

    if (key == juce::KeyPress('r', juce::ModifierKeys::commandModifier, 0)) {
        const auto selectedRows = resultsTable.getSelectedRows();
        if (const auto row = getSelectionDisplayRow(selectedRows, -1); row >= 0) {
            revealRecordInFileManager(row);
        }
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

    analysisResults.clear();
    fileActivity.clearAllActive();
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
    const auto selectedPaths = getSelectedRecordPaths();

    if (selectedPaths.isEmpty()) {
        return;
    }

    const auto selectedRows = resultsTable.getSelectedRows();
    const int fallbackRow = selectedRows.isEmpty() ? 0 : selectedRows[0];

    for (const auto& path : selectedPaths) {
        fileActivity.clearActive(path);
    }

    removeRecordsByPath(selectedPaths, fallbackRow);
}

void AudioBatchComponent::reanalyzeSelectedRecords()
{
    if (isAnalysisInProgress() || normalizeInProgress) {
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
    const auto canNormalize = !normalizeInProgress && !isAnalysisInProgress() && canNormalizeRecords(selectedRecords);

    juce::PopupMenu menu;
    menu.addItem(revealFileMenuItemId, getRevealFileMenuLabel(), record.file.exists());
    menu.addItem(openParentDirectoryMenuItemId, "Open Parent Folder", parentDirectory.isDirectory());
    menu.addSeparator();
    menu.addItem(reanalyzeMenuItemId, "Re-analyze Selected", !isAnalysisInProgress() && !normalizeInProgress);
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
        [safeThis, row](const int result) {
            if (safeThis == nullptr) {
                return;
            }

            switch (result) {
                case revealFileMenuItemId:
                    safeThis->revealRecordInFileManager(row);
                    break;
                case openParentDirectoryMenuItemId:
                    safeThis->revealRecordParentDirectory(row);
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

void AudioBatchComponent::revealRecordInFileManager(const int row) const
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    if (const auto& record = analysisResults[static_cast<std::size_t>(row)]; record.file.exists()) {
        record.file.revealToUser();
    } else {
        record.file.getParentDirectory().revealToUser();
    }
}

void AudioBatchComponent::revealRecordParentDirectory(const int row) const
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto parentDirectory = analysisResults[static_cast<std::size_t>(row)].file.getParentDirectory();

    if (parentDirectory.isDirectory()) {
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

        if (!record.file.exists()) {
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

    if (currentAudioFile.exists()) {
        for (const auto& file : filesToTrash) {
            if (file == currentAudioFile) {
                clearCurrentAudioPreview();
                break;
            }
        }
    }

    for (int index = 0; index < filesToTrash.size(); ++index) {
        const auto& file = filesToTrash.getReference(index);

        if (!utils::moveToTrash(file)) {
            failedPaths.add(file.getFullPathName());
            continue;
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

    analysisResults.erase(
        std::ranges::remove_if(
            analysisResults,
            [&removedPaths](const AudioAnalysisRecord& record) { return removedPaths.contains(record.fullPath); }
        ).begin(),
        analysisResults.end()
    );

    expectedResults = juce::jmax(0, expectedResults - removedPaths.size());
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
