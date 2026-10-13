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
#include "SeratoPreviewLoader.h"
#include "ThumbnailComponent.h"

#include <JuceHeader.h>

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
    AudioBatchComponent();

    ~AudioBatchComponent() override;

    AudioBatchComponent(const AudioBatchComponent&) = delete;
    AudioBatchComponent& operator=(const AudioBatchComponent&) = delete;
    AudioBatchComponent(AudioBatchComponent&&) = delete;
    AudioBatchComponent& operator=(AudioBatchComponent&&) = delete;

    void chooseRootFolder();

    void addRootFolder();

    void rescanCurrentRoot();

    void clearAllRecords();

    void removeSelectedRecords();

    void revealSelectedRecord() const;

    void moveSelectedRecordsToTrash(bool promptForConfirmation);

    void reanalyzeSelectedRecords();

    void showAudioSettingsWindow();

    void showSupportedNormalizationFormats();

    void processSelectedRecords();

    [[nodiscard]] bool hasSelectedRecords() const;

    [[nodiscard]] bool isFileOperationInProgress() const noexcept;

    /// Returns the plugin selection controller, or nullptr before the component finishes constructing.
    [[nodiscard]] PluginChain* getPluginChain() const noexcept
    {
        return pluginChain.get();
    }

    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;

    void paint(juce::Graphics& graphics) override;

    void resized() override;

    bool keyPressed(const juce::KeyPress& key) override;

    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;

protected:
    bool shouldDropFilesWhenDraggedExternally(
        const juce::DragAndDropTarget::SourceDetails& sourceDetails,
        juce::StringArray& files,
        bool& canMoveFiles
    ) override;

private:
    bool keyPressed(const juce::KeyPress& key, juce::Component* originatingComponent) override;

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    [[nodiscard]] bool isAnalysisInProgress() const;

    [[nodiscard]] bool isFileBeingRewritten(const juce::String& fullPath) const;

    [[nodiscard]] bool hasAnyRecords() const;

    bool loadURLIntoTransport(const juce::URL& audioUrl);

    [[nodiscard]] static bool canNormalizeRecords(const std::vector<AudioAnalysisRecord>& records);

    [[nodiscard]] juce::String getProcessingBlockedReason() const;

    [[nodiscard]] juce::String getFileOperationBlockedReason() const;

    [[nodiscard]] static juce::String buildNormalizationUnavailableMessage(
        const std::vector<AudioAnalysisRecord>& records
    );

    [[nodiscard]] juce::Array<juce::File> releasePreviewBeforeProcessing(
        const std::vector<AudioAnalysisRecord>& records
    );

    [[nodiscard]] std::vector<AudioAnalysisRecord> getSelectedProcessableRecords() const;

    juce::Array<juce::File> getSelectedRecordFiles() const;

    std::vector<AudioAnalysisRecord> getSelectedNormalizableRecords() const;

    juce::StringArray getSelectedRecordPaths() const;

    static juce::File getDefaultBrowseDirectory();

    int findRecordIndex(const juce::String& fullPath) const;

    int getSelectionDisplayRow(const juce::SparseSet<int>& selectedRows, int lastRowSelected) const;

    void configureResultsTable();

    void configurePreviewControls();

    void configureProcessingControls();

    void connectCoordinatorCallbacks();

    void requestAudioDeviceAccess();

    void clearCurrentAudioPreview();

    void releasePreviewPlayback();

    void showAudioResource(juce::URL resource);

    void requestSeratoPreview();

    void adoptSeratoPreview(
        const AnalysisCache::FileIdentity& identity,
        audiobatch::serato::SeratoPreviewLoader::Generation generation,
        audiobatch::serato::SeratoPreviewLoader::Data data
    );

    void startOrStop();

    void updatePlaybackButtonForTransportState();

    void zoomLevelChanged(double zoomLevel) const;

    void updateThumbnailDisplayGain() const;

    void handleThumbnailFullyLoaded();

    void handleDroppedPaths(const juce::StringArray& paths);

    void launchFolderChooser(bool appendToRoots);

    void setRoots(const juce::Array<juce::File>& folders);

    void addRoots(const juce::Array<juce::File>& folders);

    void updateCurrentRootLabel();

    void startAnalysis(const juce::Array<juce::File>& inputPaths, bool recursive, bool forceRefresh, bool clearResults);

    void refreshAnalysis(bool forceRefresh);

    void handleAnalysisPlan(const AnalysisCoordinator::AnalysisPlan& plan);

    void handleAnalysisResult(const AudioAnalysisRecord& record, int runId);

    void handleAnalysisComplete(int totalFiles, int runId);

    void reconcilePendingAnalysisResults();

    void normalizeSelectedRecords();

    void handleNormalizeResult(const AudioNormalizationResult& result);

    void handleNormalizeComplete(int totalFiles);

    void handleProcessingResult(const PluginProcessingResult& result);

    void handleProcessingComplete(int totalFiles);

    void showPluginProcessingWarning(const juce::String& message);

    void replaceRecordAfterOperation(const juce::File& originalFile, const AudioAnalysisRecord& newRecord);

    void handleSelectionChanged(int lastRowSelected);

    void handleSortRequested(int columnId, bool isForwards);

    void handleSecondarySortRequested(int columnId);

    void sortResults();

    void refreshSortIndicators() const;

    void refreshTablePreservingSelection(const juce::StringArray& selectedPaths);

    void restoreSelectionByPaths(const juce::StringArray& selectedPaths);

    void updateResultsTableColumnWidths() const;

    void handleFileContextMenuRequested(int row, int columnId, const juce::MouseEvent& event);

    void showFileContextMenu(int row, juce::Point<int> screenPosition);

    void removeRecordsByPath(const juce::StringArray& removedPaths, int fallbackRow);

    void runMoveToTrash(
        const juce::Array<juce::File>& filesToTrash,
        const juce::StringArray& removedPaths,
        int fallbackRow
    );

    static void revealParentDirectory(const juce::File& file);

    static void revealFileInFileManager(const juce::File& file);

    void applyCustomGainToSelection(float gainDb, bool hasGain);

    void updateGainControlsForSelection();

    void updateAudioInfo(const AudioAnalysisRecord& record);

    void updateStatusLabel();

    void updateProcessButtonState();

    void openDialogWindow(
        SafePointer<juce::DialogWindow>& window,
        const SafePointer<juce::Component>& component,
        const juce::String& title
    );

    AnalysisCache analysisCache;
    /// Owns the tag worker, stopped explicitly before GUI cleanup and before the cache is destroyed.
    std::unique_ptr<audiobatch::serato::SeratoPreviewLoader> seratoPreviewLoader;
    /// Holds the selected preview's metadata so analysis refreshes retain BPM and key until the selection changes.
    audiobatch::serato::SeratoPreviewLoader::Data seratoPreviewData;
    /// Records the requested or adopted file version, including stable negative outcomes.
    std::optional<AnalysisCache::FileIdentity> seratoPreviewIdentity;
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

    juce::Array<SafePointer<juce::DialogWindow>> childWindows;
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
