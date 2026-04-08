#pragma once

#include "AnalysisCache.h"
#include "AnalysisCoordinator.h"
#include "AudioFileTableModel.h"
#include "NormalizeCoordinator.h"
#include "ThumbnailComponent.h"

#include <JuceHeader.h>

#include <map>
#include <vector>

class AudioInfoPanel;

/// Main application view that coordinates analysis, playback preview, and file-list interactions.
class AudioBatchComponent :
    public juce::Component,
    public juce::FileDragAndDropTarget,
    public juce::DragAndDropContainer,
    juce::ChangeListener,
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

    /// Opens the audio-device settings dialog.
    void showAudioSettingsWindow();

    /// Shows which file types can be normalized in the current build.
    void showSupportedNormalizationFormats();

    void filesDropped(const juce::StringArray& files, int x, int y) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void clearCurrentAudioPreview();
    void handleDroppedPaths(const juce::StringArray& paths);
    bool keyPressed(const juce::KeyPress& key) override;
    void handleAnalysisComplete(int totalFiles);
    void handleNormalizeComplete(int totalFiles);
    void handleNormalizeResult(const AudioNormalizationResult& result);

    /// Selects the clicked row and opens the per-file action menu.
    void handleFileContextMenuRequested(int row, int columnId, const juce::MouseEvent& event);

    void handleAnalysisResult(const AudioAnalysisRecord& record);
    void handleThumbnailFullyLoaded();
    void handleSelectionChanged(int lastRowSelected);
    void handleSortRequested(int columnId, bool isForwards);
    void handleSecondarySortRequested(int columnId);
    [[nodiscard]] juce::String getActiveStatusLabel(const AudioAnalysisRecord& record) const;
    [[nodiscard]] float getActivityPhase() const;
    [[nodiscard]] bool isAnalysisInProgress() const;
    [[nodiscard]] bool isAnyFileProcessing() const;
    bool loadURLIntoTransport(const juce::URL& audioUrl);
    void normalizeSelectedRecords();

    /// Moves the selected files to the OS trash, optionally asking for confirmation first.
    void moveSelectedRecordsToTrash(bool promptForConfirmation);

    int findRecordIndex(const juce::String& fullPath) const;
    juce::Array<juce::File> getSelectedRecordFiles() const;
    std::vector<AudioAnalysisRecord> getSelectedNormalizableRecords() const;
    juce::StringArray getSelectedRecordPaths() const;
    int getSelectionDisplayRow(const juce::SparseSet<int>& selectedRows, int lastRowSelected) const;
    void markFilesProcessing(const juce::Array<juce::File>& files, const juce::String& statusLabel);
    [[nodiscard]] static bool canNormalizeRecords(const std::vector<AudioAnalysisRecord>& records);
    [[nodiscard]] juce::String buildNormalizationUnavailableMessage(
        const std::vector<AudioAnalysisRecord>& records
    ) const;
    void reconcilePendingAnalysisResults();
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
    static juce::File getInitialRootDirectory();
    void refreshAnalysis(bool forceRefresh);
    void restoreSelectionByPaths(const juce::StringArray& selectedPaths);

    /// Updates the header text so the secondary sort key remains visible.
    void refreshSortIndicators();

    /// Shows the context menu for the given row at the supplied screen position.
    void showFileContextMenu(int row, juce::Point<int> screenPosition);

    void sortResults();
    void startAnalysis(const juce::Array<juce::File>& inputPaths, bool recursive, bool forceRefresh, bool clearResults);
    void updateResultsTableColumnWidths() const;
    void updateAudioInfo(const AudioAnalysisRecord& record);
    void updateStatusLabel();
    bool shouldDropFilesWhenDraggedExternally(
        const juce::DragAndDropTarget::SourceDetails& sourceDetails,
        juce::StringArray& files,
        bool& canMoveFiles
    ) override;

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void openDialogWindow(
        SafePointer<juce::DialogWindow>& window,
        const SafePointer<juce::Component>& component,
        const juce::String& title
    );
    void showAudioResource(juce::URL resource);
    void syncActivityTimer();
    void startOrStop();
    void timerCallback() override;
    void zoomLevelChanged(double zoomLevel);
    void mouseMagnify(const juce::MouseEvent&, float scaleFactor) override;

    AnalysisCache analysisCache;
    AnalysisCoordinator analysisCoordinator;
    NormalizeCoordinator normalizeCoordinator;
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

    std::map<juce::String, juce::String> activeFileStatusLabels;
    juce::StringArray normalizationFailures;

    juce::Label currentRootLabel {"CurrentRootLabel"};
    juce::Label statusLabel {"StatusLabel"};
    juce::Label zoomLabel {"Zoom"};
    juce::Slider zoomSlider {"ZoomSlider"};
    juce::TextButton settingsButton {"Settings"};
    juce::TextButton startStopButton {"Play/Stop"};
    std::unique_ptr<AudioInfoPanel> audioInfo;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioBatchComponent)
};
