/// Main GUI component of the application.
/// Declares AudioBatchComponent, which owns the analysis results table, waveform preview and transport,
/// folder scanning, gain controls, and the plugin chain button and processing flow.
/// It also hosts the coordinators for background analysis, normalization, and plugin processing.

#pragma once

#include "AnalysisCache.h"
#include "AnalysisCoordinator.h"
#include "AudioFileTableModel.h"
#include "IntervalStepSlider.h"
#include "NormalizeCoordinator.h"
#include "PluginChain.h"
#include "PluginProcessing.h"
#include "PluginProcessingCoordinator.h"
#include "ThumbnailComponent.h"

#include <JuceHeader.h>

#include <map>
#include <memory>
#include <vector>

class AudioInfoPanel;

/// Main application view that coordinates analysis, playback preview, and file-list interactions.
class AudioBatchComponent :
    public juce::Component,
    public juce::FileDragAndDropTarget,
    public juce::DragAndDropContainer,
    juce::ChangeListener,
    juce::KeyListener,
    juce::Timer
{
public:
    /// Creates the UI, opens the analysis cache, and starts the initial scan.
    AudioBatchComponent();

    /// Stops background work and releases audio and dialog resources.
    ~AudioBatchComponent() override;

    /// Opens the root-folder chooser used for analysis.
    void chooseRootFolder();

    /// Re-runs analysis for the current root folder.
    void rescanCurrentRoot();

    /// Removes every analyzed file from the table without touching the files on disk.
    void clearAllRecords();

    /// Removes the currently selected files from the table without touching the files on disk.
    void removeSelectedRecords();

    /// Forces re-analysis of the currently selected files, bypassing the analysis cache.
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

    /// Starts analysis for dropped files, or switches the root folder when only a directory was dropped.
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    /// Accepts any external drag that contains at least one file.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;

    /// Fills the background with the look-and-feel window colour.
    void paint(juce::Graphics& graphics) override;

    /// Lays out the top bar, results table, resize bar, waveform preview, and the control column.
    void resized() override;

private:
    /// Stops playback and releases the preview file, transport source, and waveform so the file can be replaced.
    void clearCurrentAudioPreview();

    /// Routes dropped paths to a root-folder scan when only a directory was dropped,
    /// otherwise analyzes the dropped files individually.
    void handleDroppedPaths(const juce::StringArray& paths);

    /// Handles the application shortcuts for trash, remove, reveal, play/stop, and rescan.
    bool keyPressed(const juce::KeyPress& key) override;

    /// Forwards key presses from the results table to the main shortcut handler.
    bool keyPressed(const juce::KeyPress& key, juce::Component* originatingComponent) override;

    /// Finalizes an analysis run on the message thread:
    /// reconciles rows still pending, logs run statistics, and restores a sensible selection.
    void handleAnalysisComplete(int totalFiles);

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
    /// preserving any custom gain already set for the file and keeping selection and preview in sync.
    void handleAnalysisResult(const AudioAnalysisRecord& record);

    /// Stores freshly computed waveform data in the analysis cache,
    /// unless the current waveform was itself loaded from the cache.
    void handleThumbnailFullyLoaded();

    /// Loads the newly selected file into the preview and refreshes the info panel and gain controls.
    void handleSelectionChanged(int lastRowSelected);

    /// Applies a new primary sort and clears the secondary sort when it targets the same column.
    void handleSortRequested(int columnId, bool isForwards);

    /// Sets, toggles, or clears the secondary sort column requested with a ctrl or cmd click on a header.
    void handleSecondarySortRequested(int columnId);

    /// Returns the transient activity label for a record, for example "Analyzing" or "Normalizing",
    /// or an empty string when the file is idle.
    [[nodiscard]] juce::String getActiveStatusLabel(const AudioAnalysisRecord& record) const;

    /// Returns the shared animation phase in the range 0 to 1 that drives the spinning activity indicators.
    [[nodiscard]] static float getActivityPhase();

    /// True while analysis results are still expected from the coordinator.
    [[nodiscard]] bool isAnalysisInProgress() const;

    /// True while any file carries a transient activity label, which is what keeps the activity timer running.
    [[nodiscard]] bool isAnyFileProcessing() const;

    /// Creates a reader for the URL and attaches it to the transport, replacing any previous source.
    /// Returns false when no reader could be created, leaving the transport empty.
    bool loadURLIntoTransport(const juce::URL& audioUrl);

    /// Starts peak normalization for the selected files
    /// after checking that no other pass is running and that every selected format is supported.
    void normalizeSelectedRecords();

    /// Moves the selected files to the OS trash, optionally asking for confirmation first.
    void moveSelectedRecordsToTrash(bool promptForConfirmation);

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

    /// Tags the files with a transient activity label and starts the activity animation.
    void markFilesProcessing(const juce::Array<juce::File>& files, const juce::String& activityLabel);

    /// True when the list is non-empty and every record's format can be normalized in this build.
    [[nodiscard]] static bool canNormalizeRecords(const std::vector<AudioAnalysisRecord>& records);

    /// Builds a user-facing explanation of why the selected files cannot be normalized,
    /// listing each unsupported file with its reason.
    [[nodiscard]] static juce::String buildNormalizationUnavailableMessage(
        const std::vector<AudioAnalysisRecord>& records
    );

    /// Resolves rows still marked pending or active after a run completes,
    /// falling back to the cache or a synchronous re-analysis so no row is left in a stale state.
    void reconcilePendingAnalysisResults();

    /// Clears a file's transient activity label and stops the activity animation when nothing is left running.
    void unmarkFileProcessing(const juce::String& fullPath);

    /// Removes trashed files from the table and keeps selection and preview state coherent.
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
    /// Returns the starting directory for the folder chooser, favouring a Dropbox folder when one exists.
    static juce::File getDefaultBrowseDirectory();

    /// Re-analyzes the current root folder,
    /// or the files already in the table when no root directory has been set.
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
    /// Refuses to start while a normalization pass is running.
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

    /// Lets rows dragged out of the application drop as file copies into other applications,
    /// never as moves, so the originals stay in place.
    bool shouldDropFilesWhenDraggedExternally(
        const juce::DragAndDropTarget::SourceDetails& sourceDetails,
        juce::StringArray& files,
        bool& canMoveFiles
    ) override;

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

    /// Starts or stops the repaint timer based on whether any file shows an activity indicator.
    void syncActivityTimer();

    /// Toggles preview playback of the current file.
    void startOrStop();

    /// Recolours the play button to match whether the transport is playing.
    void updatePlaybackButtonForTransportState();

    /// Repaints the table to animate the activity indicators, and stops itself once all files are idle.
    void timerCallback() override;

    /// Forwards the zoom slider value to the waveform preview.
    void zoomLevelChanged(double zoomLevel);

    /// Maps pinch-zoom gestures onto the waveform zoom slider.
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;

    AnalysisCache analysisCache;
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
    juce::AudioDeviceSelectorComponent audioSetupComp;
    juce::AudioFormatManager formatManager;
    juce::AudioSourcePlayer audioSourcePlayer;
    juce::AudioTransportSource transportSource;
    juce::File currentAudioFile;
    juce::File currentRoot;
    juce::TableListBox resultsTable;
    juce::TimeSliceThread thread {"audio file preview"};
    juce::URL currentAudioUrl;
    double analysisStartedAtMs = 0.0;
    int analyzedFilesThisRun = 0;
    int completedResults = 0;
    int expectedResults = 0;
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

    std::map<juce::String, juce::String> activeFileStatusLabels;
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
    std::unique_ptr<AudioInfoPanel> audioInfo;
    juce::Viewport audioInfoViewport;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioBatchComponent)
};
