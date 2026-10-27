/// AudioBatchComponent member functions for choosing what to analyze:
/// the folder chooser, the root folders, drag and drop, and starting a rescan.

#include "gui/AudioBatchComponent.h"
#include "gui/AudioBatchComponentConstants.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>

namespace audiobatch
{
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
    const auto startDirectory = lastRoot.isDirectory() ? lastRoot : utils::getDefaultBrowseDirectory();
    directoryChooser = std::make_unique<juce::FileChooser>(
        appendToRoots ? "Add Audio Folder" : "Choose Audio Folder", startDirectory, supportedAudioFilePatterns, true
    );

    directoryChooser->launchAsync(
        static_cast<int>(
            static_cast<unsigned int>(juce::FileBrowserComponent::openMode)
            | static_cast<unsigned int>(juce::FileBrowserComponent::canSelectDirectories)
        ),
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
/// Refuses before changing the roots while files are being rewritten,
/// since startAnalysis would refuse the scan and leave roots that were never scanned.
void AudioBatchComponent::setRoots(const juce::Array<juce::File>& folders)
{
    if (const auto blockedReason = getFileOperationBlockedReason(backgroundWork.getWork()); blockedReason.isNotEmpty())
    {
        statusBar.setStatusText(blockedReason);
        return;
    }

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
    refreshAnalysis(AnalysisRows::replace);
}

/// Appends folders to the root folders and analyzes them without clearing the current results.
/// Folders that already are a root or sit inside one are skipped,
/// and existing roots inside a newly added folder are absorbed into it.
/// Refuses before changing the roots while files are being rewritten, like setRoots.
void AudioBatchComponent::addRoots(const juce::Array<juce::File>& folders)
{
    if (const auto blockedReason = getFileOperationBlockedReason(backgroundWork.getWork()); blockedReason.isNotEmpty())
    {
        statusBar.setStatusText(blockedReason);
        return;
    }

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
        statusBar.setStatusText("Folder is already included");
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

    startAnalysis(existingRoots, true, false, AnalysisRows::add);
}

/// Shows the root folders on the left of the status bar:
/// the full path for a single root, or the first path and the number of others for several,
/// with every root listed in the tooltip.
void AudioBatchComponent::updateCurrentRootLabel()
{
    if (currentRoots.isEmpty()) {
        statusBar.setPath({}, {});
        return;
    }

    const auto firstPath = currentRoots.getFirst().getFullPathName();
    const auto labelText
        = currentRoots.size() == 1 ? firstPath : utils::format("{} (+{} more)", firstPath, currentRoots.size() - 1);

    juce::StringArray rootPaths;
    for (const auto& root : currentRoots) {
        rootPaths.add(root.getFullPathName());
    }

    statusBar.setPath(labelText, rootPaths.joinIntoString("\n"));
}

/// Scans the root folders again, analyzing only new and changed files and keeping the rows of the others,
/// like the Rust version. Re-analyze Selected is the action that ignores the cache.
void AudioBatchComponent::rescanCurrentRoot()
{
    refreshAnalysis(AnalysisRows::sync);
}

/// Opens dropped paths, appending dropped directories to the root folders when Shift is held.
void AudioBatchComponent::handleDroppedPaths(const juce::StringArray& paths)
{
    // The drop callback carries no modifier state, so read the live keyboard state instead.
    openPaths(paths, juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown());
}

/// Routes paths to a root folder scan when they are all directories,
/// otherwise analyzes the files among them individually.
/// The directories replace the root folders, or are appended to them.
/// Paths that do not exist are logged and skipped.
void AudioBatchComponent::openPaths(const juce::StringArray& paths, const bool appendToRoots)
{
    juce::Array<juce::File> files;
    juce::Array<juce::File> directories;

    for (const auto& path : paths) {
        const juce::File file(path);

        if (file.isDirectory()) {
            directories.addIfNotAlreadyThere(file);
        } else if (file.existsAsFile()) {
            files.addIfNotAlreadyThere(file);
        } else {
            utils::logWarn("Path not found: {}", path.quoted());
        }
    }

    if (files.isEmpty() && !directories.isEmpty()) {
        for (const auto& directory : directories) {
            utils::logInfo(
                "Opening directory{}: {}", appendToRoots ? " to add" : "", directory.getFullPathName().quoted()
            );
        }

        if (appendToRoots) {
            addRoots(directories);
        } else {
            setRoots(directories);
        }

        return;
    }

    if (files.isEmpty()) {
        statusBar.setStatusText("No supported files to open");
        return;
    }

    utils::logInfo("Opening {} files", files.size());
    startAnalysis(files, false, false, AnalysisRows::add);
}

/// Analyzes all root folders that still exist, or the files already in the table when no root folder has been set.
/// Cached results of unchanged files are used.
void AudioBatchComponent::refreshAnalysis(const AnalysisRows rows)
{
    juce::Array<juce::File> existingRoots;
    for (const auto& root : currentRoots) {
        if (root.isDirectory()) {
            existingRoots.add(root);
        }
    }

    if (!existingRoots.isEmpty()) {
        startAnalysis(existingRoots, true, false, rows);
        return;
    }

    // No root directory set, so analyze the individual files already in the results table.
    juce::Array<juce::File> existingFiles;
    for (const auto& record : analysisResults) {
        if (record.file.existsAsFile()) {
            existingFiles.addIfNotAlreadyThere(record.file);
        }
    }

    if (existingFiles.isEmpty()) {
        return;
    }

    startAnalysis(existingFiles, false, false, rows);
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
}  // namespace audiobatch
