/// Main GUI component of the application.
/// Declares AudioBatchComponent, which owns the analysis results table, waveform preview and transport,
/// folder scanning, gain controls, and the plugin chain button and processing flow.
/// It also hosts the coordinators for background analysis, normalization, and plugin processing.

#pragma once

#include "AnalysisCache.h"
#include "AnalysisCoordinator.h"
#include "AudioFileTableModel.h"
#include "FileActivityTracker.h"
#include "IntervalStepSlider.h"
#include "NormalizationTargetControl.h"
#include "NormalizeCoordinator.h"
#include "PluginChain.h"
#include "PluginProcessing.h"
#include "PluginProcessingCoordinator.h"
#include "ThumbnailComponent.h"

#include <JuceHeader.h>

#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

class AudioInfoPanel;

/// Main application view that coordinates analysis, playback preview, and file-list interactions.
class AudioBatchComponent :
    public juce::Component,
    public juce::FileDragAndDropTarget,
    public juce::DragAndDropContainer,
    juce::ChangeListener,
    juce::KeyListener
{
public:
    /// Creates the UI, opens the analysis cache, and starts the initial scan.
    AudioBatchComponent();

    /// Stops background work and releases audio and dialog resources.
    ~AudioBatchComponent() override;

    AudioBatchComponent(const AudioBatchComponent&) = delete;
    AudioBatchComponent& operator=(const AudioBatchComponent&) = delete;
    AudioBatchComponent(AudioBatchComponent&&) = delete;
    AudioBatchComponent& operator=(AudioBatchComponent&&) = delete;

    /// Opens the folder chooser and replaces the current root folders with the chosen folder.
    void chooseRootFolder();

    /// Opens the folder chooser and appends the chosen folder to the current root folders,
    /// analyzing its files without clearing the results that are already in the table.
    void addRootFolder();

    /// Re-runs analysis for all current root folders.
    void rescanCurrentRoot();

    /// Removes every analyzed file from the table without touching the files on disk.
    /// Cancels a running analysis, and is refused while files are normalized or processed,
    /// since their results would add rows back.
    void clearAllRecords();

    /// Removes the currently selected files from the table without touching the files on disk.
    /// Files that are being normalized or processed stay in the table.
    void removeSelectedRecords();

    /// Reveals the file of the selected row in the platform file manager.
    /// With several rows selected, the row shown in the info panel is used.
    /// Does nothing when no row is selected.
    void revealSelectedRecord();

    /// Moves the selected files to the OS trash, optionally asking for confirmation first.
    /// The rows of the files that were trashed are removed from the table.
    /// Files that are being normalized or processed are skipped,
    /// because the running operation would write its output back to the same path.
    void moveSelectedRecordsToTrash(bool promptForConfirmation);

    /// Forces re-analysis of the currently selected files, bypassing the analysis cache.
    /// Refused while analysis, normalization, or plugin processing is running.
    void reanalyzeSelectedRecords();

    /// Opens the audio-device settings dialog.
    void showAudioSettingsWindow();

    /// Shows which file types can be normalized in the current build.
    void showSupportedNormalizationFormats();

    /// Runs the configured plugin (if any) over the currently-selected files.
    void processSelectedRecords();

    /// Returns the plugin selection controller, or nullptr before the component finishes constructing.
    [[nodiscard]] PluginChain* getPluginChain() const noexcept
    {
        return pluginChain.get();
    }

    /// Starts analysis for dropped files, or switches the root folders when only directories were dropped.
    /// Holding Shift while dropping directories appends them to the current root folders instead.
    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;

    /// Accepts any external drag that contains at least one file.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;

    /// Fills the background with the look-and-feel window colour.
    void paint(juce::Graphics& graphics) override;

    /// Lays out the top bar, results table, resize bar, waveform preview, and the control column.
    void resized() override;

    /// Handles the application shortcuts for trash, remove, reveal, play/stop, and rescan.
    bool keyPressed(const juce::KeyPress& key) override;

    /// Maps pinch-zoom gestures onto the waveform zoom slider.
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;

protected:
    /// Lets rows dragged out of the application drop as file copies into other applications,
    /// never as moves, so the originals stay in place.
    bool shouldDropFilesWhenDraggedExternally(
        const juce::DragAndDropTarget::SourceDetails& sourceDetails,
        juce::StringArray& files,
        bool& canMoveFiles
    ) override;

private:
    /// Stops playback and releases the preview file, transport source, and waveform so the file can be replaced.
    void clearCurrentAudioPreview();

    /// Stops playback, closes the preview file, and clears the waveform, keeping the current file selected,
    /// so the file on disk can be replaced or the preview reset.
    void releasePreviewPlayback();

    /// Routes dropped paths to a root folder scan when only directories were dropped,
    /// otherwise analyzes the dropped files individually.
    /// Dropped directories replace the root folders, or are appended to them when Shift is held.
    void handleDroppedPaths(const juce::StringArray& paths);

    /// Opens the asynchronous folder chooser.
    /// The chosen folder either replaces the root folders or is appended to them.
    void launchFolderChooser(bool appendToRoots);

    /// Replaces the root folders and re-analyzes them, clearing the current results.
    void setRoots(const juce::Array<juce::File>& folders);

    /// Appends folders to the root folders and analyzes them without clearing the current results.
    /// Folders that already are a root or sit inside one are skipped,
    /// and existing roots inside a newly added folder are absorbed into it.
    void addRoots(const juce::Array<juce::File>& folders);

    /// Shows a non-blocking warning dialog titled "Plugin Processing" with the given message.
    void showPluginProcessingWarning(const juce::String& message);

    /// Returns the reason a processing run cannot start right now because other work is running,
    /// or an empty string when a run may start.
    [[nodiscard]] juce::String getProcessingBlockedReason() const;

    /// Stops playback and releases the preview file handles when the previewed file is about to be replaced.
    /// Returns the files of the given records.
    [[nodiscard]] juce::Array<juce::File> releasePreviewBeforeProcessing(
        const std::vector<AudioAnalysisRecord>& records
    );

    /// Creates the results table header, model, and selection behavior.
    void configureResultsTable();

    /// Creates the waveform preview, transport and zoom controls, and the metadata info panel.
    void configurePreviewControls();

    /// Creates the plugin chain, per-file gain, normalization toggle, process, and settings controls.
    void configureProcessingControls();

    /// Connects the analysis, normalization, and plugin coordinators to the message thread handlers.
    void connectCoordinatorCallbacks();

    /// Requests audio input permission and initialises the audio device manager once it is answered.
    void requestAudioDeviceAccess();

    /// Shows the root folders in the top bar label:
    /// the full path for a single root, or the first path and the number of others for several,
    /// with every root listed in the tooltip.
    void updateCurrentRootLabel();

    /// Forwards key presses from the results table to the main shortcut handler.
    bool keyPressed(const juce::KeyPress& key, juce::Component* originatingComponent) override;

    /// Finalizes an analysis run on the message thread:
    /// reconciles rows still pending, logs run statistics, and restores a sensible selection.
    /// Completion of a replaced run is ignored.
    void handleAnalysisComplete(int totalFiles, int runId);

    /// Ends the normalization pass and reports any accumulated failures to the user.
    void handleNormalizeComplete(int totalFiles);

    /// Applies one normalization result on the message thread.
    /// Replaces the record, since the output path may differ from the source,
    /// updates the cache, and reloads the preview when the current file was normalized.
    void handleNormalizeResult(const AudioNormalizationResult& result);

    /// Ends the plugin processing pass and reports any accumulated failures to the user.
    void handleProcessingComplete(int totalFiles);

    /// Applies one plugin processing result on the message thread.
    /// Replaces the record, drops stale cache rows and persisted gain, and reloads the preview when needed.
    void handleProcessingResult(const PluginProcessingResult& result);

    /// Updates the current selection's custom gain in-memory, in the cache, and the waveform preview.
    void applyCustomGainToSelection(float gainDb, bool hasGain);

    /// Syncs the gain slider and clear button with the record shown for the current selection.
    void updateGainControlsForSelection();

    /// Applies the current file's custom gain to the waveform preview as a display-only scale factor.
    void updateThumbnailDisplayGain();

    /// Returns the selected records that exist on disk, are fully analyzed, and have a processable format.
    [[nodiscard]] std::vector<AudioAnalysisRecord> getSelectedProcessableRecords() const;

    /// Selects the clicked row and opens the per-file action menu.
    void handleFileContextMenuRequested(int row, int columnId, const juce::MouseEvent& event);

    /// Merges one analysis result into the table on the message thread,
    /// preserving the custom gain set for the file while the file is unchanged on disk,
    /// and keeping selection and preview in sync.
    /// Results from a replaced run are ignored, since their queued messages can arrive after a new run started.
    void handleAnalysisResult(const AudioAnalysisRecord& record, int runId);

    /// Adds pending rows for the files the run analyzes and shows the progress text.
    /// Called once per run on the message thread, after the coordinator has scanned the input folders.
    void handleAnalysisPlan(const AnalysisCoordinator::AnalysisPlan& plan);

    /// Stores freshly computed waveform data in the analysis cache,
    /// unless the current waveform was itself loaded from the cache.
    void handleThumbnailFullyLoaded();

    /// Loads the newly selected file into the preview and refreshes the info panel and gain controls.
    void handleSelectionChanged(int lastRowSelected);

    /// Applies a new primary sort and clears the secondary sort when it targets the same column.
    void handleSortRequested(int columnId, bool isForwards);

    /// Sets, toggles, or clears the secondary sort column requested with a ctrl or cmd click on a header.
    void handleSecondarySortRequested(int columnId);

    /// True from the start of an analysis run until the coordinator reports its completion.
    [[nodiscard]] bool isAnalysisInProgress() const;

    /// True while normalization or plugin processing is rewriting files.
    [[nodiscard]] bool isFileOperationInProgress() const noexcept;

    /// Returns the reason analysis or a change to the list cannot happen right now
    /// because files are being normalized or processed, or an empty string when it can.
    [[nodiscard]] juce::String getFileOperationBlockedReason() const;

    /// True when the file is being normalized or processed right now.
    [[nodiscard]] bool isFileBeingRewritten(const juce::String& fullPath) const;

    /// Creates a reader for the URL and attaches it to the transport, replacing any previous source.
    /// Returns false when no reader could be created, leaving the transport empty.
    bool loadURLIntoTransport(const juce::URL& audioUrl);

    /// Starts peak normalization for the selected files
    /// after checking that no other pass is running and that every selected format is supported.
    void normalizeSelectedRecords();

    /// True when the table has at least one selected row.
    [[nodiscard]] bool hasSelectedRecords() const;

    /// True when the table currently holds any analyzed records.
    [[nodiscard]] bool hasAnyRecords() const;

    /// Returns the current row index of the record with the given full path, or -1 when it is not in the table.
    int findRecordIndex(const juce::String& fullPath) const;

    /// Returns the selected files that still exist on disk.
    juce::Array<juce::File> getSelectedRecordFiles() const;

    /// Returns the selected records when every one of them exists on disk and is fully analyzed.
    /// Returns an empty list as soon as any selected record is missing or unfinished,
    /// so normalization is all-or-nothing.
    std::vector<AudioAnalysisRecord> getSelectedNormalizableRecords() const;

    /// Returns the full paths of the selected rows, used to restore the selection after re-sorting.
    juce::StringArray getSelectedRecordPaths() const;

    /// Picks the row whose details should be shown for a multi-selection:
    /// the current preview file when it is selected, otherwise the last clicked or the first selected row.
    int getSelectionDisplayRow(const juce::SparseSet<int>& selectedRows, int lastRowSelected) const;

    /// True when the list is non-empty and every record's format can be normalized in this build.
    [[nodiscard]] static bool canNormalizeRecords(const std::vector<AudioAnalysisRecord>& records);

    /// Builds a user-facing explanation of why the selected files cannot be normalized,
    /// listing each unsupported file with its reason.
    [[nodiscard]] static juce::String buildNormalizationUnavailableMessage(
        const std::vector<AudioAnalysisRecord>& records
    );

    /// Resolves rows still marked pending or active after a run completes,
    /// from the cache when possible and otherwise as failed, so no row is left waiting.
    /// Never decodes audio, because it runs on the message thread.
    void reconcilePendingAnalysisResults();

    /// Re-sorts the results, refreshes the table, and selects the given paths again.
    void refreshTablePreservingSelection(const juce::StringArray& selectedPaths);

    /// Replaces the row of a file that a normalize or process run rewrote with the record of its output,
    /// which may have a new path when the source was converted.
    /// Updates the selection, the cache, and the preview to follow the file to its new path.
    void replaceRecordAfterOperation(const juce::File& originalFile, const AudioAnalysisRecord& newRecord);

    /// Removes trashed files from the table and keeps selection and preview state coherent.
    /// Clears the activity labels and messages of the removed files,
    /// and while analysis runs, makes sure their results do not add the rows back.
    void removeRecordsByPath(const juce::StringArray& removedPaths, int fallbackRow);

    /// Opens the selected file's parent folder in the platform file manager.
    void revealRecordParentDirectory(int row) const;

    /// Reveals the selected file in the platform file manager when supported.
    void revealRecordInFileManager(int row) const;

    /// Executes the trash operation and reports any failures back to the user.
    void runMoveToTrash(
        const juce::Array<juce::File>& filesToTrash,
        const juce::StringArray& removedPaths,
        int fallbackRow
    );

    /// Returns the starting directory for the folder chooser,
    /// favouring a Dropbox folder, then the system Music folder, then the home folder.
    static juce::File getDefaultBrowseDirectory();

    /// Re-analyzes all root folders that still exist,
    /// or the files already in the table when no root folder has been set.
    void refreshAnalysis(bool forceRefresh);

    /// Re-selects rows by file path after the table order or contents have changed.
    void restoreSelectionByPaths(const juce::StringArray& selectedPaths);

    /// Updates the header text so the secondary sort key remains visible.
    void refreshSortIndicators();

    /// Shows the context menu for the given row at the supplied screen position.
    void showFileContextMenu(int row, juce::Point<int> screenPosition);

    /// Sorts the records by the primary and secondary sort columns.
    /// Failed files always sort last, and file name plus full path act as the final tie breakers,
    /// keeping the order stable across incremental result updates.
    void sortResults();

    /// Kicks off a background analysis run,
    /// adding pending placeholder rows for files that are not already cached.
    /// A run that keeps the results replaces a running one, so it also takes over the inputs of that run,
    /// which would otherwise leave its placeholder rows waiting forever.
    /// Refuses to start while normalization or plugin processing is running,
    /// since the analysis would read files that are being replaced or moved to the trash.
    void startAnalysis(const juce::Array<juce::File>& inputPaths, bool recursive, bool forceRefresh, bool clearResults);

    /// Distributes leftover table width between the name and path columns while preserving their current ratio.
    void updateResultsTableColumnWidths() const;

    /// Rebuilds the info panel rows for a record, with dedicated layouts for pending and failed files.
    void updateAudioInfo(const AudioAnalysisRecord& record);

    /// Shows the most relevant progress or summary text for the current background activity.
    void updateStatusLabel();

    /// Enables the process button only when an enabled plugin chain exists,
    /// records are loaded, and no processing or analysis is running.
    void updateProcessButtonState();

    /// Reacts to transport state changes and to files dropped onto the waveform preview.
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    /// Shows a reusable dialog window, creating it lazily on first use.
    /// The window does not take ownership of the content component.
    void openDialogWindow(
        SafePointer<juce::DialogWindow>& window,
        const SafePointer<juce::Component>& component,
        const juce::String& title
    );

    /// Loads a file URL into the preview player and waveform,
    /// preferring cached waveform data over decoding the file again.
    void showAudioResource(juce::URL resource);

    /// Toggles preview playback of the current file.
    void startOrStop();

    /// Recolours the play button to match whether the transport is playing.
    void updatePlaybackButtonForTransportState();

    /// Forwards the zoom slider value to the waveform preview.
    void zoomLevelChanged(double zoomLevel);

    AnalysisCache analysisCache;
    /// Removes cached rows of deleted files in the background after startup.
    /// Declared after the cache, so it is stopped and joined before the cache is destroyed.
    std::jthread cachePruneThread;
    AnalysisCoordinator analysisCoordinator;
    NormalizeCoordinator normalizeCoordinator;
    PluginProcessingCoordinator pluginCoordinator;
    juce::ApplicationProperties pluginAppProperties;
    std::unique_ptr<PluginChain> pluginChain;
    std::vector<AudioAnalysisRecord> analysisResults;
    juce::StretchableLayoutManager mainVerticalLayout;
    juce::StretchableLayoutResizerBar waveformResizeBar {&mainVerticalLayout, 1, false};
    AudioFileTableModel fileTableModel;
    std::unique_ptr<juce::AudioFormatReaderSource> currentAudioFileSource;
    std::unique_ptr<juce::FileChooser> directoryChooser;
    std::unique_ptr<ThumbnailComponent> thumbnail;

    SafePointer<juce::DialogWindow> settingsWindow;

    juce::Array<juce::Component::SafePointer<juce::DialogWindow>> childWindows;
    juce::AudioDeviceManager audioDeviceManager;
    juce::AudioDeviceSelectorComponent audioSetupComponent;
    juce::AudioFormatManager formatManager;
    juce::AudioSourcePlayer audioSourcePlayer;
    juce::AudioTransportSource transportSource;
    juce::File currentAudioFile;
    juce::Array<juce::File> currentRoots;
    juce::TableListBox resultsTable;
    juce::TimeSliceThread thread {"audio file preview"};
    juce::URL currentAudioUrl;
    double analysisStartedAtMs = 0.0;
    int analyzedFilesThisRun = 0;
    int completedResults = 0;
    int expectedResults = 0;
    /// Id of the current analysis run, used to drop queued messages from runs that were replaced.
    int analysisRunId = -1;
    /// True from the start of an analysis run until its completion arrives.
    /// Kept separate from the progress counters, which only drive the status text.
    bool analysisRunning = false;
    /// True once the current run's plan arrived, so the status shows progress instead of the folder scan.
    bool analysisPlanReceived = false;
    /// Options of the current analysis run.
    /// A run that keeps the results and replaces a running one carries these inputs over.
    AudioAnalysisOptions currentAnalysisOptions;
    /// Full paths of the rows removed while analysis was running.
    /// Their results are ignored, so a running or carried over run does not add the rows back.
    std::set<juce::String> pathsRemovedDuringAnalysis;
    /// True when the current run replaces the results list, which picks the message shown when no files are found.
    bool analysisReplacesResults = false;
    int normalizedResultsCompleted = 0;
    int normalizedResultsExpected = 0;
    int currentSortColumnId = AudioFileTableModel::columnOverallPeak;
    bool currentSortForwards = true;
    int secondarySortColumnId = 0;
    bool secondarySortForwards = true;
    bool currentWaveformLoadedFromCache = false;
    bool normalizeInProgress = false;
    bool pluginProcessingInProgress = false;
    int processedResultsCompleted = 0;
    int processedResultsExpected = 0;

    /// Activity labels and last operation messages per file, shown in the status column.
    FileActivityTracker fileActivity {[this] { resultsTable.repaint(); }};
    juce::StringArray normalizationFailures;
    juce::StringArray processingFailures;

    juce::Label currentRootLabel {"CurrentRootLabel"};
    juce::Label statusLabel {"StatusLabel"};
    juce::Label zoomLabel {"Zoom"};
    juce::Label gainLabel {"GainLabel"};
    juce::Slider zoomSlider {"ZoomSlider"};

    IntervalStepSlider gainSlider {"GainSlider"};
    juce::TextButton gainClearButton {"x"};
    juce::TextButton settingsButton {"Audio Settings"};
    juce::TextButton pluginButton {"Plugins"};
    juce::TextButton startStopButton {"Play/Stop"};
    juce::TextButton processButton {"Process"};
    juce::ToggleButton normalizeBeforePluginToggle {"Normalize"};
    /// Lets normalization and plugin processing replace an existing output file.
    /// Not persisted, so every session starts with overwriting disabled.
    juce::ToggleButton forceOverwriteToggle {"Force overwrite"};
    /// Normalization mode and target, persisted in pluginAppProperties, which is declared before it.
    NormalizationTargetControl normalizationTargetControl {pluginAppProperties};
    std::unique_ptr<AudioInfoPanel> audioInfo;
    juce::Viewport audioInfoViewport;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    JUCE_LEAK_DETECTOR(AudioBatchComponent)
};
