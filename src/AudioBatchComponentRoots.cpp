/// AudioBatchComponent member functions for choosing what to analyze:
/// the folder chooser, the root folders, drag and drop, and starting a rescan.

#include "AudioBatchComponent.h"
#include "AudioBatchComponentConstants.h"
#include "Log.h"
#include "StringFormat.h"

#include <algorithm>

using namespace audiobatch::gui;

/// Accepts any external drag that contains at least one file.
bool AudioBatchComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    return !files.isEmpty();
}

/// Starts analysis for dropped files, or switches the root folders when only directories were dropped.
/// Holding Shift while dropping directories appends them to the current root folders instead.
void AudioBatchComponent::filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/)
{
    handleDroppedPaths(files);
}

/// Opens the folder chooser and replaces the current root folders with the chosen folder.
void AudioBatchComponent::chooseRootFolder()
{
    launchFolderChooser(false);
}

/// Opens the folder chooser and appends the chosen folder to the current root folders,
/// analyzing its files without clearing the results that are already in the table.
void AudioBatchComponent::addRootFolder()
{
    launchFolderChooser(true);
}

/// Opens the asynchronous folder chooser.
/// The chosen folder either replaces the root folders or is appended to them.
void AudioBatchComponent::launchFolderChooser(const bool appendToRoots)
{
    const auto lastRoot = currentRoots.isEmpty() ? juce::File() : currentRoots.getLast();
    const auto startDirectory = lastRoot.isDirectory() ? lastRoot : getDefaultBrowseDirectory();
    directoryChooser = std::make_unique<juce::FileChooser>(
        appendToRoots ? "Add Audio Folder" : "Choose Audio Folder", startDirectory, supportedAudioFilePatterns, true
    );

    directoryChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = SafePointer(this), appendToRoots](const juce::FileChooser& chooser) {
            if (safeThis == nullptr) {
                return;
            }

            if (const auto selectedFolder = chooser.getResult(); selectedFolder.isDirectory()) {
                utils::logInfo(
                    "{} directory: {}", appendToRoots ? "Added" : "Opened", selectedFolder.getFullPathName().quoted()
                );

                if (appendToRoots) {
                    safeThis->addRoots({selectedFolder});
                } else {
                    safeThis->setRoots({selectedFolder});
                }
            }

            safeThis->directoryChooser.reset();
        }
    );
}

/// Replaces the root folders and re-analyzes them, clearing the current results.
void AudioBatchComponent::setRoots(const juce::Array<juce::File>& folders)
{
    currentRoots.clear();

    for (const auto& folder : folders) {
        // Keep only the outermost folders, so no file is collected twice.
        const auto isInsideOtherFolder
            = std::ranges::any_of(folders, [&folder](const juce::File& other) { return folder.isAChildOf(other); });

        if (!isInsideOtherFolder) {
            currentRoots.addIfNotAlreadyThere(folder);
        }
    }

    updateCurrentRootLabel();
    refreshAnalysis(false);
}

/// Appends folders to the root folders and analyzes them without clearing the current results.
/// Folders that already are a root or sit inside one are skipped,
/// and existing roots inside a newly added folder are absorbed into it.
void AudioBatchComponent::addRoots(const juce::Array<juce::File>& folders)
{
    bool rootsChanged = false;

    for (const auto& folder : folders) {
        const auto alreadyIncluded = std::ranges::any_of(currentRoots, [&folder](const juce::File& root) {
            return folder == root || folder.isAChildOf(root);
        });

        if (alreadyIncluded) {
            utils::logInfo("Folder is already included in the roots: {}", folder.getFullPathName().quoted());
            continue;
        }

        // A new parent folder absorbs the existing roots inside it.
        currentRoots.removeIf([&folder](const juce::File& root) { return root.isAChildOf(folder); });
        currentRoots.add(folder);
        rootsChanged = true;
    }

    if (!rootsChanged) {
        statusLabel.setText("Folder is already included", juce::dontSendNotification);
        return;
    }

    updateCurrentRootLabel();

    // Re-collect every root instead of only the new folder.
    // Rows already in the table and cached files are skipped quickly,
    // and an analysis run that is still in progress gets restarted with all of its files included.
    juce::Array<juce::File> existingRoots;
    for (const auto& root : currentRoots) {
        if (root.isDirectory()) {
            existingRoots.add(root);
        }
    }

    startAnalysis(existingRoots, true, false, false);
}

/// Shows the root folders in the top bar label:
/// the full path for a single root, or the first path and the number of others for several,
/// with every root listed in the tooltip.
void AudioBatchComponent::updateCurrentRootLabel()
{
    if (currentRoots.isEmpty()) {
        currentRootLabel.setText("", juce::dontSendNotification);
        currentRootLabel.setTooltip("");
        return;
    }

    const auto firstPath = currentRoots.getFirst().getFullPathName();
    const auto labelText
        = currentRoots.size() == 1 ? firstPath : utils::format("{} (+{} more)", firstPath, currentRoots.size() - 1);

    juce::StringArray rootPaths;
    for (const auto& root : currentRoots) {
        rootPaths.add(root.getFullPathName());
    }

    currentRootLabel.setText(labelText, juce::dontSendNotification);
    currentRootLabel.setTooltip(rootPaths.joinIntoString("\n"));
}

/// Re-runs analysis for all current root folders.
void AudioBatchComponent::rescanCurrentRoot()
{
    refreshAnalysis(true);
}

/// Returns the starting directory for the folder chooser,
/// favouring a Dropbox folder, then the system Music folder, then the home folder.
juce::File AudioBatchComponent::getDefaultBrowseDirectory()
{
    auto homeDirectory = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    // Prefer the Dropbox folder where the music library is usually kept,
    // then the system Music folder, and finally the home folder.
    // A user config file for this is listed in the README TODO.
#if JUCE_WINDOWS
    auto dropboxDirectory = juce::File("D:\\Dropbox");
#else
    auto dropboxDirectory = homeDirectory.getChildFile("Dropbox");
#endif

    if (dropboxDirectory.isDirectory()) {
        return dropboxDirectory;
    }

    if (const auto musicDirectory = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
        musicDirectory.isDirectory())
    {
        return musicDirectory;
    }

    return homeDirectory;
}

/// Routes dropped paths to a root folder scan when only directories were dropped,
/// otherwise analyzes the dropped files individually.
/// Dropped directories replace the root folders, or are appended to them when Shift is held.
void AudioBatchComponent::handleDroppedPaths(const juce::StringArray& paths)
{
    juce::Array<juce::File> droppedFiles;
    juce::Array<juce::File> droppedDirectories;

    for (const auto& path : paths) {
        const juce::File droppedPath(path);

        if (droppedPath.isDirectory()) {
            droppedDirectories.addIfNotAlreadyThere(droppedPath);
            continue;
        }

        if (droppedPath.existsAsFile()) {
            droppedFiles.addIfNotAlreadyThere(droppedPath);
        }
    }

    if (droppedFiles.isEmpty() && !droppedDirectories.isEmpty()) {
        // The drop callback carries no modifier state, so read the live keyboard state instead.
        const auto appendToRoots = juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();

        for (const auto& directory : droppedDirectories) {
            utils::logInfo(
                "Dropped directory{}: {}", appendToRoots ? " to add" : "", directory.getFullPathName().quoted()
            );
        }

        if (appendToRoots) {
            addRoots(droppedDirectories);
        } else {
            setRoots(droppedDirectories);
        }

        return;
    }

    if (droppedFiles.isEmpty()) {
        statusLabel.setText("No supported dropped files", juce::dontSendNotification);
        return;
    }

    utils::logInfo("Dropped {} files", droppedFiles.size());
    startAnalysis(droppedFiles, false, false, false);
}

/// Re-analyzes all root folders that still exist, or the files already in the table when no root folder has been set.
void AudioBatchComponent::refreshAnalysis(const bool forceRefresh)
{
    juce::Array<juce::File> existingRoots;
    for (const auto& root : currentRoots) {
        if (root.isDirectory()) {
            existingRoots.add(root);
        }
    }

    if (!existingRoots.isEmpty()) {
        startAnalysis(existingRoots, true, forceRefresh, true);
        return;
    }

    // No root directory set, so re-analyze individual files already in the results table.
    juce::Array<juce::File> existingFiles;
    for (const auto& record : analysisResults) {
        if (record.file.existsAsFile()) {
            existingFiles.addIfNotAlreadyThere(record.file);
        }
    }

    if (existingFiles.isEmpty()) {
        return;
    }

    startAnalysis(existingFiles, false, forceRefresh, true);
}

/// Lets rows dragged out of the application drop as file copies into other applications,
/// never as moves, so the originals stay in place.
bool AudioBatchComponent::shouldDropFilesWhenDraggedExternally(
    const juce::DragAndDropTarget::SourceDetails& sourceDetails,
    juce::StringArray& files,
    bool& canMoveFiles
)
{
    files = juce::StringArray::fromLines(sourceDetails.description.toString());
    files.removeEmptyStrings();
    files.removeDuplicates(false);

    for (int index = files.size(); --index >= 0;) {
        if (!juce::File(files[index]).existsAsFile()) {
            files.remove(index);
        }
    }

    canMoveFiles = false;
    return !files.isEmpty();
}
