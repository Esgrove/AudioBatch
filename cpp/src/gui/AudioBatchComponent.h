/// Main GUI component of the application.
/// Declares AudioBatchComponent, which owns the analysis results table, the preview and its workspace controls,
/// folder scanning, per-file gains, and the plugin chain and processing flow.
/// It starts background analysis, normalization, and plugin processing through BackgroundWorkController,
/// and applies their results to the table and the preview as the controller's listener.

#pragma once

#include "analysis/AnalysisCache.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/AudioFileTableModel.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/BackgroundWorkController.h"
#include "gui/components/AudioSettingsComponent.h"
#include "gui/components/PreviewWorkspace.h"
#include "gui/components/StatusBar.h"
#include "gui/FileActivityTracker.h"
#include "gui/GuiLayout.h"
#include "gui/LiveChain.h"
#include "gui/PreviewController.h"
#include "gui/ResultsModel.h"
#include "gui/ResultsSorting.h"
#include "plugins/PluginLibrary.h"
#include "plugins/PluginProcessing.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace audiobatch
{
/// Main application view that coordinates analysis, playback preview, and file-list interactions.
class AudioBatchComponent :
    public juce::Component,
    public juce::FileDragAndDropTarget,
    public juce::DragAndDropContainer,
    juce::ChangeListener,
    juce::KeyListener,
    private juce::ComponentListener,
    private BackgroundWorkListener
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
    void cancelCurrentWork();
    [[nodiscard]] bool isBackgroundWorkRunning() const noexcept;
    [[nodiscard]] juce::String describeNormalizationTarget() const;
    void moveSelectedRecordsToTrash(bool promptForConfirmation);
    void openPaths(const juce::StringArray& paths, bool appendToRoots);
    void processSelectedRecords();
    void processAllRecords();
    void normalizeAllRecords();
    void reanalyzeSelectedRecords();
    void removeSelectedRecords();
    void rescanCurrentRoot();
    void revealSelectedRecord() const;
    void showAudioSettingsWindow();
    void showSupportedNormalizationFormats();
    void setViewOptionsChangedCallback(std::function<void()> callback);
    void togglePreviewSection();
    void toggleSeratoWaveformColouring();
    void toggleLoudnessGraph();
    void togglePluginPreview();
    void togglePreviewLatencyCompensation();
    void toggleContinuePreviewPlayback();
    void togglePlayFromFirstCuePoint();

    [[nodiscard]] bool hasSelectedRecords() const;
    [[nodiscard]] bool isFileOperationInProgress() const noexcept;
    [[nodiscard]] bool isPreviewSectionExpanded() const noexcept;
    [[nodiscard]] bool isSeratoWaveformColouringEnabled() const noexcept;
    [[nodiscard]] bool isLoudnessGraphEnabled() const noexcept;
    [[nodiscard]] bool isPluginPreviewEnabled() const noexcept;
    [[nodiscard]] bool isPreviewLatencyCompensated() const noexcept;
    [[nodiscard]] bool isContinuePreviewPlaybackEnabled() const noexcept;
    [[nodiscard]] bool isPlayFromFirstCuePointEnabled() const noexcept;

    /// Returns the plugin chain, which the constructor creates and the destructor destroys.
    [[nodiscard]] LiveChain& getLiveChain() const noexcept
    {
        return *liveChain;
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
    void componentVisibilityChanged(juce::Component& component) override;
    bool keyPressed(const juce::KeyPress& key, juce::Component* originatingComponent) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void analysisPlanReceived(const AnalysisPlan& plan, const AnalyzingWork& run) override;
    void analysisResultsReceived(std::vector<AudioAnalysisRecord> records) override;
    void analysisCompleted(const AnalyzingWork& run, int totalFiles) override;
    void normalizeResultReceived(const AudioNormalizationResult& result) override;
    void normalizeCompleted(const NormalizingWork& pass, int totalFiles) override;
    void processingResultReceived(const PluginProcessingResult& result) override;
    void processingCompleted(const ProcessingWork& pass, int totalFiles) override;
    void processingStartFailed(const juce::String& errorMessage) override;
    void updatePreviewAnimation();

    juce::Array<juce::File> getSelectedRecordFiles() const;
    juce::StringArray getSelectedRecordPaths() const;
    [[nodiscard]] juce::Array<juce::File> releasePreviewBeforeProcessing(
        const std::vector<AudioAnalysisRecord>& records
    );

    [[nodiscard]] bool hasAnyRecords() const;
    [[nodiscard]] bool isFileBeingRewritten(const juce::String& fullPath) const;
    [[nodiscard]] std::vector<AudioAnalysisRecord> getSelectedProcessableRecords() const;
    [[nodiscard]] std::vector<AudioAnalysisRecord> getAllReadyRecords() const;
    int getSelectionDisplayRow(const juce::SparseSet<int>& selectedRows, int lastRowSelected) const;
    std::vector<AudioAnalysisRecord> getSelectedNormalizableRecords() const;

    void addRoots(const juce::Array<juce::File>& folders);
    void applyCustomGainToSelection(std::optional<Decibels> gain);
    void clearCurrentAudioPreview();
    void clearPreviewDisplay();
    void configurePreviewControls();
    void configureProcessingControls();
    void configureResultsTable();
    void handleDroppedPaths(const juce::StringArray& paths);
    void handleFileContextMenuRequested(int row, const juce::MouseEvent& event);
    void handleSecondarySortRequested(AudioFileTableColumn column);
    void handleSelectionChanged(int lastRowSelected);
    void handleSortRequested(std::optional<AudioFileTableColumn> column, bool isForwards);
    void handleThumbnailFullyLoaded();
    void launchFolderChooser(bool appendToRoots);
    void normalizeSelectedRecords();
    void normalizeRecords(const std::vector<AudioAnalysisRecord>& recordsToNormalize);
    void processRecords(const std::vector<AudioAnalysisRecord>& records);
    void reconcilePendingAnalysisResults();
    void refreshAnalysis(AnalysisRows rows);
    void refreshSortIndicators() const;
    void refreshTablePreservingSelection(const juce::StringArray& selectedPaths);
    void releasePreviewPlayback();
    void removeRecordsByPath(const juce::StringArray& removedPaths, int fallbackRow);
    void replaceRecordAfterOperation(const juce::File& originalFile, const AudioAnalysisRecord& newRecord);
    void requestSeratoPreview();
    void restoreSelectionByPaths(const juce::StringArray& selectedPaths);
    void setRoots(const juce::Array<juce::File>& folders);
    void showAudioFile(const juce::File& file);
    void showFileContextMenu(int row, juce::Point<int> screenPosition);
    void showPluginProcessingWarning(const juce::String& message);
    void startAnalysis(const juce::Array<juce::File>& inputPaths, bool recursive, bool forceRefresh, AnalysisRows rows);
    void updateAudioInfo(const AudioAnalysisRecord& record);
    void updateCurrentRootLabel();
    void updateGainControlsForSelection();
    void updateOutputStatus();
    void updatePlaybackButtonForTransportState();
    void updateProcessButtonState();
    void updateProgressIndicators();
    void updateResultsTableColumnWidths() const;
    void updateStatusLabel();
    void updateThumbnailDisplayGain();
    void updatePreviewLoudnessHistory();
    void updatePreviewInputGain(bool immediately = false);
    void setPreviewPreferences(const AudioPreviewPreferences& preferences);
    void synchronizePreviewPreferences();
    void zoomLevelChanged(double zoomLevel);

    void runMoveToTrash(
        const juce::Array<juce::File>& filesToTrash,
        const juce::StringArray& removedPaths,
        int fallbackRow
    );

    void openDialogWindow(
        SafePointer<juce::DialogWindow>& window,
        const SafePointer<juce::Component>& component,
        const juce::String& title,
        int initialWidth = layout::dialogContentWidth,
        int initialHeight = layout::dialogContentHeight
    );

    void adoptSeratoPreview(
        const utils::FileIdentity& identity,
        std::uint64_t generation,
        std::shared_ptr<const serato::SeratoData> data
    );

    serato::SeratoPreviewLoader::Completion createSeratoPreviewCompletion();

    /// Notifies the window when preview command labels or check marks need refreshing.
    std::function<void()> onViewOptionsChanged;

    AnalysisCache analysisCache;

    /// Removes cached rows of deleted files in the background after startup.
    /// Declared after the cache, so it is stopped and joined before the cache is destroyed.
    std::jthread cachePruneThread;

    /// The rows of the results table. Declared before the table model, which refers to it.
    ResultsModel analysisResults;
    AudioFileTableModel fileTableModel;
    juce::ApplicationProperties pluginAppProperties;
    juce::StretchableLayoutManager mainVerticalLayout;
    juce::StretchableLayoutResizerBar waveformResizeBar {&mainVerticalLayout, 1, false};

    /// Plays the previewed file.
    /// Declared before the thumbnail and the audio settings component, which hold references into it.
    AudioPreviewPlayer previewPlayer;

    /// The previewed file and its Serato tags.
    /// Its tag worker is stopped explicitly before GUI cleanup,
    /// and it is declared after the cache and the player it uses, so it is destroyed before them.
    PreviewController preview {analysisCache, previewPlayer, createSeratoPreviewCompletion()};

    /// The known plugins, created in the constructor once the settings storage is set up.
    /// Declared before the chain, which creates its processors through it.
    std::unique_ptr<PluginLibrary> pluginLibrary;
    std::unique_ptr<LiveChain> liveChain;

    /// The waveform, metrics, and controls below the results table.
    /// Declared after the player and the settings it refers to.
    PreviewWorkspace previewWorkspace {previewPlayer, pluginAppProperties};

    SafePointer<juce::DialogWindow> settingsWindow;
    juce::Array<SafePointer<juce::DialogWindow>> childWindows;
    juce::Array<juce::File> currentRoots;
    AudioSettingsComponent audioSettingsComponent {previewPlayer.getDeviceManager()};
    AudioPreviewPreferences previewPreferences;
    juce::TableListBox resultsTable;
    std::unique_ptr<juce::FileChooser> directoryChooser;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    /// Primary and secondary sort columns of the results table.
    results_sorting::SortState sortState {
        .primaryColumn = AudioFileTableColumn::overallPeak,
        .secondaryColumn = std::nullopt,
        .statusText = {},
    };

    /// Activity, progress, and last operation messages per file, shown in the status column.
    /// Its callback also runs on every animation tick while files are busy, which keeps the overall progress current.
    FileActivityTracker fileActivity {resultsTable, [this] {
                                          resultsTable.repaint();
                                          updateProgressIndicators();
                                      }};

    /// Runs the analysis, normalization, and plugin processing passes, one at a time.
    /// Declared after the cache and the activity tracker it refers to,
    /// so it is destroyed, and its workers stopped, before them.
    BackgroundWorkController backgroundWork {analysisCache, fileActivity, *this};

    /// Root folders, background activity, audio output device, and overall progress along the bottom of the window.
    StatusBar statusBar;

    bool automaticSelectionChange {false};

    /// Expanded divider allocation is independent of the animated or compact workspace.
    bool previewSectionExpanded {true};
    bool restoreExpandedLayout {false};
    double expandedPreviewProportion {-layout::previewPreferredProportion};
    double animationStartProportion {-layout::previewPreferredProportion};
    double animationStartedAtMilliseconds {0.0};
    int workspaceAvailableHeight {0};
    int currentPreviewHeight {0};
    juce::VBlankAttachment previewAnimationAttachment;

    JUCE_LEAK_DETECTOR(AudioBatchComponent)
};
}  // namespace audiobatch
