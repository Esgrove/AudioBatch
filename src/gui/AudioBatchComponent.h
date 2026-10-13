/// Main GUI component of the application.
/// Declares AudioBatchComponent, which owns the analysis results table, waveform preview and transport,
/// folder scanning, gain controls, and the plugin chain button and processing flow.
/// It also hosts the coordinators for background analysis, normalization, and plugin processing.

#pragma once

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCoordinator.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/AudioFileTableModel.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/IntervalStepSlider.h"
#include "gui/components/NormalizationTargetControl.h"
#include "gui/components/ThumbnailComponent.h"
#include "gui/FileActivityTracker.h"
#include "gui/ResultsSorting.h"
#include "normalization/NormalizeCoordinator.h"
#include "plugins/PluginChain.h"
#include "plugins/PluginProcessing.h"
#include "plugins/PluginProcessingCoordinator.h"
#include "serato/SeratoPreviewLoader.h"

#include <JuceHeader.h>

#include <memory>
#include <thread>
#include <vector>

namespace audiobatch
{
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

    void addRootFolder();
    void chooseRootFolder();
    void clearAllRecords();
    void moveSelectedRecordsToTrash(bool promptForConfirmation);
    void processSelectedRecords();
    void reanalyzeSelectedRecords();
    void removeSelectedRecords();
    void rescanCurrentRoot();
    void revealSelectedRecord() const;
    void showAudioSettingsWindow();
    void showSupportedNormalizationFormats();

    [[nodiscard]] bool hasSelectedRecords() const;
    [[nodiscard]] bool isFileOperationInProgress() const noexcept;

    /// Returns the plugin selection controller, or nullptr before the component finishes constructing.
    [[nodiscard]] PluginChain* getPluginChain() const noexcept
    {
        return pluginChain.get();
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    bool keyPressed(const juce::KeyPress& key) override;

    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;
    void paint(juce::Graphics& graphics) override;
    void resized() override;

protected:
    bool shouldDropFilesWhenDraggedExternally(
        const juce::DragAndDropTarget::SourceDetails& sourceDetails,
        juce::StringArray& files,
        bool& canMoveFiles
    ) override;

private:
    bool keyPressed(const juce::KeyPress& key, juce::Component* originatingComponent) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    juce::Array<juce::File> getSelectedRecordFiles() const;
    juce::StringArray getSelectedRecordPaths() const;
    [[nodiscard]] juce::String getFileOperationBlockedReason() const;
    [[nodiscard]] juce::String getProcessingBlockedReason() const;
    [[nodiscard]] juce::Array<juce::File> releasePreviewBeforeProcessing(
        const std::vector<AudioAnalysisRecord>& records
    );

    [[nodiscard]] bool hasAnyRecords() const;
    [[nodiscard]] bool isAnalysisInProgress() const;
    [[nodiscard]] bool isFileBeingRewritten(const juce::String& fullPath) const;
    [[nodiscard]] std::vector<AudioAnalysisRecord> getSelectedProcessableRecords() const;
    int findRecordIndex(const juce::String& fullPath) const;
    int getSelectionDisplayRow(const juce::SparseSet<int>& selectedRows, int lastRowSelected) const;
    std::vector<AudioAnalysisRecord> getSelectedNormalizableRecords() const;

    void addRoots(const juce::Array<juce::File>& folders);
    void applyCustomGainToSelection(float gainDb, bool hasGain);
    void clearCurrentAudioPreview();
    void configurePreviewControls();
    void configureProcessingControls();
    void configureResultsTable();
    void connectCoordinatorCallbacks();
    void handleAnalysisComplete(int totalFiles, int runId);
    void handleAnalysisPlan(const AnalysisPlan& plan);
    void handleAnalysisResult(const AudioAnalysisRecord& record, int runId);
    void handleDroppedPaths(const juce::StringArray& paths);
    void handleFileContextMenuRequested(int row, int columnId, const juce::MouseEvent& event);
    void handleNormalizeComplete(int totalFiles);
    void handleNormalizeResult(const AudioNormalizationResult& result);
    void handleProcessingComplete(int totalFiles);
    void handleProcessingResult(const PluginProcessingResult& result);
    void handleSecondarySortRequested(int columnId);
    void handleSelectionChanged(int lastRowSelected);
    void handleSortRequested(int columnId, bool isForwards);
    void handleThumbnailFullyLoaded();
    void launchFolderChooser(bool appendToRoots);
    void normalizeSelectedRecords();
    void reconcilePendingAnalysisResults();
    void refreshAnalysis(bool forceRefresh);
    void refreshSortIndicators() const;
    void refreshTablePreservingSelection(const juce::StringArray& selectedPaths);
    void releasePreviewPlayback();
    void removeRecordsByPath(const juce::StringArray& removedPaths, int fallbackRow);
    void replaceRecordAfterOperation(const juce::File& originalFile, const AudioAnalysisRecord& newRecord);
    void requestSeratoPreview();
    void restoreSelectionByPaths(const juce::StringArray& selectedPaths);
    void setRoots(const juce::Array<juce::File>& folders);
    void showAudioResource(juce::URL resource);
    void showFileContextMenu(int row, juce::Point<int> screenPosition);
    void showPluginProcessingWarning(const juce::String& message);
    void sortResults();
    void startAnalysis(const juce::Array<juce::File>& inputPaths, bool recursive, bool forceRefresh, bool clearResults);
    void updateAudioInfo(const AudioAnalysisRecord& record);
    void updateCurrentRootLabel();
    void updateGainControlsForSelection();
    void updatePlaybackButtonForTransportState();
    void updateProcessButtonState();
    void updateResultsTableColumnWidths() const;
    void updateStatusLabel();
    void updateThumbnailDisplayGain() const;
    void zoomLevelChanged(double zoomLevel) const;

    void runMoveToTrash(
        const juce::Array<juce::File>& filesToTrash,
        const juce::StringArray& removedPaths,
        int fallbackRow
    );

    void openDialogWindow(
        SafePointer<juce::DialogWindow>& window,
        const SafePointer<juce::Component>& component,
        const juce::String& title
    );

    void adoptSeratoPreview(
        const FileIdentity& identity,
        std::uint64_t generation,
        std::shared_ptr<const serato::SeratoData> data
    );

    AnalysisCache analysisCache;

    /// Owns the tag worker, stopped explicitly before GUI cleanup and before the cache is destroyed.
    std::unique_ptr<serato::SeratoPreviewLoader> seratoPreviewLoader;

    /// Holds the selected preview's metadata so analysis refreshes retain BPM and key until the selection changes.
    std::shared_ptr<const serato::SeratoData> seratoPreviewData;

    /// Records the requested or adopted file version, including stable negative outcomes.
    std::optional<FileIdentity> seratoPreviewIdentity;

    /// Removes cached rows of deleted files in the background after startup.
    /// Declared after the cache, so it is stopped and joined before the cache is destroyed.
    std::jthread cachePruneThread;

    AnalysisCoordinator analysisCoordinator;
    AudioFileTableModel fileTableModel;
    NormalizeCoordinator normalizeCoordinator;
    PluginProcessingCoordinator pluginCoordinator;
    juce::ApplicationProperties pluginAppProperties;
    juce::StretchableLayoutManager mainVerticalLayout;
    juce::StretchableLayoutResizerBar waveformResizeBar {&mainVerticalLayout, 1, false};
    std::unique_ptr<PluginChain> pluginChain;
    std::vector<AudioAnalysisRecord> analysisResults;

    /// Plays the previewed file.
    /// Declared before the thumbnail and the audio settings component, which hold references into it.
    AudioPreviewPlayer previewPlayer;

    /// Normalization mode and target, persisted in pluginAppProperties, which is declared before it.
    NormalizationTargetControl normalizationTargetControl {pluginAppProperties};

    AnalysisRunState analysisRun;
    SafePointer<juce::DialogWindow> settingsWindow;
    juce::Array<SafePointer<juce::DialogWindow>> childWindows;
    juce::Array<juce::File> currentRoots;
    juce::AudioDeviceSelectorComponent audioSetupComponent;
    juce::File currentAudioFile;
    juce::TableListBox resultsTable;
    juce::URL currentAudioUrl;
    juce::Viewport audioInfoViewport;
    std::unique_ptr<AudioInfoPanel> audioInfo;
    std::unique_ptr<ThumbnailComponent> thumbnail;
    std::unique_ptr<juce::FileChooser> directoryChooser;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    /// Progress and failures of the current or last normalization pass.
    OperationProgress normalizeProgress;

    /// Progress and failures of the current or last plugin processing pass.
    OperationProgress processingProgress;

    /// Primary and secondary sort columns of the results table.
    results_sorting::SortState sortState {.primaryColumnId = columnOverallPeak, .statusText = {}};

    /// Activity labels and last operation messages per file, shown in the status column.
    FileActivityTracker fileActivity {[this] { resultsTable.repaint(); }};

    IntervalStepSlider gainSlider {"GainSlider"};
    juce::Label currentRootLabel {"CurrentRootLabel"};
    juce::Label gainLabel {"GainLabel"};
    juce::Label statusLabel {"StatusLabel"};
    juce::Label zoomLabel {"Zoom"};
    juce::Slider zoomSlider {"ZoomSlider"};
    juce::TextButton gainClearButton {"x"};
    juce::TextButton pluginButton {"Plugins"};
    juce::TextButton processButton {"Process"};
    juce::TextButton settingsButton {"Audio Settings"};
    juce::TextButton startStopButton {"Play/Stop"};
    juce::ToggleButton forceOverwriteToggle {"Force overwrite"};
    juce::ToggleButton normalizeBeforePluginToggle {"Normalize"};

    bool currentWaveformLoadedFromCache {false};

    JUCE_LEAK_DETECTOR(AudioBatchComponent)
};
}  // namespace audiobatch
