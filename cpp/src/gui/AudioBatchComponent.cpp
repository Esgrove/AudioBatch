/// Implementation of the main GUI component:
/// construction, the results table, selection, sorting, the status bar text and progress, and dialog windows.
/// The other AudioBatchComponent*.cpp files implement the remaining member functions by topic.

#include "gui/AudioBatchComponent.h"

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisFormatting.h"
#include "gui/AudioBatchComponentConstants.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "plugins/PluginProcessing.h"
#include "utils/AppInfo.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <array>
#include <optional>
#include <utility>
#include <variant>

namespace audiobatch
{
/// Creates the results table header, model, and selection behavior.
void AudioBatchComponent::configureResultsTable()
{
    resultsTable.setHeader(
        std::make_unique<SecondarySortTableHeader>(
            [this](const AudioFileTableColumn column) { handleSecondarySortRequested(column); },
            &AudioFileTableModel::getColumnDescription
        )
    );
    AudioFileTableModel::configureHeader(resultsTable.getHeader());
    resultsTable.setModel(&fileTableModel);
    resultsTable.setColour(juce::ListBox::backgroundColourId, CustomLookAndFeel::greyMediumDark());
    resultsTable.setMultipleSelectionEnabled(true);
    resultsTable.setOutlineThickness(0);
    resultsTable.setRowHeight(layout::resultsTableRowHeight);
    // The activity labels change while files are busy, so only the settled text decides the status column order.
    sortState.statusText = [this](const AudioAnalysisRecord& record) {
        const auto [text, isError] = fileActivity.getMessage(record.fullPath);
        return text.isNotEmpty() ? text : formatStatus(record);
    };
    resultsTable.getHeader().setSortColumnId(toColumnId(sortState.primaryColumn), sortState.primaryForwards);
    refreshSortIndicators();
    addAndMakeVisible(resultsTable);
    resultsTable.addKeyListener(this);
}

/// Sets up the split between the results table and the preview workspace,
/// and connects the waveform, the section toggle, the playback, preview chain, and zoom controls.
void AudioBatchComponent::configurePreviewControls()
{
    mainVerticalLayout.setItemLayout(
        0,
        layout::resultsTableMinimumHeight,
        layout::resultsTableMaximumProportion,
        layout::resultsTablePreferredProportion
    );
    mainVerticalLayout.setItemLayout(1, layout::resizeBarHeight, layout::resizeBarHeight, layout::resizeBarHeight);
    mainVerticalLayout.setItemLayout(
        2, layout::previewMinimumHeight, layout::previewMaximumProportion, layout::previewPreferredProportion
    );
    addAndMakeVisible(waveformResizeBar);
    addAndMakeVisible(previewWorkspace);

    previewWorkspace.previewSectionButton.onClick = [this] { togglePreviewSection(); };

    auto& thumbnail = previewWorkspace.thumbnail;
    thumbnail.addChangeListener(this);
    previewPlayer.addChangeListener(this);
    thumbnail.setMouseWheelZoomCallback([this](const double zoomFactor) {
        auto& zoomSlider = previewWorkspace.zoomSlider;
        zoomSlider.setValue(zoomSlider.getValue() * zoomFactor);
    });
    thumbnail.setMouseWheelGainCallback([this](const float deltaDb) {
        auto& gainSlider = previewWorkspace.gainSlider;
        if (!gainSlider.isEnabled() || juce::approximatelyEqual(deltaDb, 0.0f)) {
            return;
        }
        const auto interval = gainSlider.getInterval() > 0.0 ? gainSlider.getInterval() : gain_range::stepDecibels;
        const auto step = (deltaDb > 0.0f ? 1.0 : -1.0) * interval;
        const auto newValue
            = juce::jlimit(gainSlider.getMinimum(), gainSlider.getMaximum(), gainSlider.getValue() + step);
        gainSlider.setValue(newValue, juce::sendNotificationSync);
    });
    thumbnail.setThumbnailFullyLoadedCallback([this] { handleThumbnailFullyLoaded(); });

    previewWorkspace.startStopButton.onClick = [this] { previewPlayer.togglePlayback(); };
    previewWorkspace.previewChainToggle.onClick = [this] {
        if (previewWorkspace.previewChainToggle.getToggleState() != isPluginPreviewEnabled()) {
            togglePluginPreview();
        }
    };
    previewWorkspace.zoomSlider.onValueChange
        = [this] { zoomLevelChanged(previewWorkspace.zoomSlider.getValue() * zoom_range::percentToScaleFactor); };
}

/// Connects the plugin chain and the audio settings to the preview,
/// and connects the gain, normalization, process, plugin chain, and settings controls.
void AudioBatchComponent::configureProcessingControls()
{
    const auto refreshLiveChain = [this] {
        const auto refreshed = previewPlayer.refreshPluginChain();
        juce::ignoreUnused(refreshed);
        previewPreferences.pluginChainEnabled = previewPlayer.isPluginPreviewEnabled();
        synchronizePreviewPreferences();
        updateProcessButtonState();
    };
    liveChain->setChainChangedCallback(refreshLiveChain);
    liveChain->setLatencyChangedCallback(refreshLiveChain);
    previewPlayer.setErrorCallback([this](const juce::String& message) {
        previewPreferences.pluginChainEnabled = previewPlayer.isPluginPreviewEnabled();
        synchronizePreviewPreferences();
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon, "Audio Preview", message, "OK", this
            ),
            nullptr
        );
    });
    audioSettingsComponent.setPreferencesChangedCallback([this](const AudioPreviewPreferences& preferences) {
        setPreviewPreferences(preferences);
    });
    setPreviewPreferences(loadAudioPreviewPreferences(*pluginAppProperties.getUserSettings()));

    previewWorkspace.pluginButton.onClick = [this] { liveChain->showMenu(previewWorkspace.pluginButton); };

    previewWorkspace.gainSlider.onValueChange = [this] {
        const auto value = static_cast<float>(previewWorkspace.gainSlider.getValue());
        applyCustomGainToSelection(
            juce::approximatelyEqual(value, 0.0f) ? std::nullopt : std::optional(Decibels {value})
        );
    };

    auto& normalizationTargetControl = previewWorkspace.normalizationTargetControl;
    auto& normalizeBeforePluginToggle = previewWorkspace.normalizeBeforePluginToggle;
    normalizationTargetControl.loadSettings();
    normalizationTargetControl.setNormalizeCallback([this] { normalizeSelectedRecords(); });
    normalizationTargetControl.setTargetEnabled(normalizeBeforePluginToggle.getToggleState());
    normalizeBeforePluginToggle.onClick = [this] {
        previewWorkspace.normalizationTargetControl.setTargetEnabled(
            previewWorkspace.normalizeBeforePluginToggle.getToggleState()
        );
    };

    updateProcessButtonState();
    previewWorkspace.processButton.onClick = [this] {
        if (isBackgroundWorkRunning()) {
            cancelCurrentWork();
        } else {
            processSelectedRecords();
        }
    };
    previewWorkspace.settingsButton.onClick = [this] { showAudioSettingsWindow(); };
}

/// Creates the completion for the Serato tag worker of the preview,
/// which adopts the tags on the message thread while this component still exists.
/// Called from the member initializer of the preview controller, so it only captures a safe pointer to this component.
serato::SeratoPreviewLoader::Completion AudioBatchComponent::createSeratoPreviewCompletion()
{
    return
        [safeThis = SafePointer(this)](
            utils::FileIdentity identity, const std::uint64_t generation, std::shared_ptr<const serato::SeratoData> data
        ) {
            juce::MessageManager::callAsync(
                [safeThis, previewIdentity = std::move(identity), generation, previewData = std::move(data)] {
                    if (safeThis != nullptr) {
                        safeThis->adoptSeratoPreview(previewIdentity, generation, previewData);
                    }
                }
            );
        };
}

/// Creates the UI, opens the analysis cache, and starts the initial scan.
AudioBatchComponent::AudioBatchComponent() :
    fileTableModel(
        analysisResults,
        [this](const int row) { handleSelectionChanged(row); },
        [this](const std::optional<AudioFileTableColumn> column, const bool isForwards) {
            handleSortRequested(column, isForwards);
        },
        [this](const int row, const juce::MouseEvent& event) { handleFileContextMenuRequested(row, event); },
        [this](const AudioAnalysisRecord& record) { return fileActivity.getActivityState(record.fullPath); },
        [this](const AudioAnalysisRecord& record) { return fileActivity.getMessage(record.fullPath); },
        [] { return FileActivityTracker::getAnimationPhase(); }
    )
{
    const auto constructorStartedAtMs = juce::Time::getMillisecondCounterHiRes();
    const auto logStartupCheckpoint = [constructorStartedAtMs](const juce::String& step) {
        utils::logDebug(
            "AudioBatchComponent startup: {} ({:.1f} ms)",
            step,
            juce::Time::getMillisecondCounterHiRes() - constructorStartedAtMs
        );
    };

    logStartupCheckpoint("constructor begin");

    // The plugin chain is created first, so every control and callback configured below can rely on it.
    pluginAppProperties.setStorageParameters(utils::createApplicationSettingsOptions());
    pluginLibrary = std::make_unique<PluginLibrary>(pluginAppProperties);
    liveChain = std::make_unique<LiveChain>(
        *pluginLibrary, pluginAppProperties, previewPlayer.getRenderingLock(), previewPlayer.getDeviceManager()
    );
    previewPlayer.setLiveChain(liveChain.get());
    previewPlayer.setSavedDeviceState(pluginAppProperties.getUserSettings()->getXmlValue(audioDeviceStatePropertyKey));

    addAndMakeVisible(statusBar);
    previewPlayer.getDeviceManager().addChangeListener(this);
    updateOutputStatus();

    configureResultsTable();
    configurePreviewControls();
    configureProcessingControls();

    tooltipWindow = std::make_unique<juce::TooltipWindow>(this, layout::tooltipDelayMilliseconds);
    logStartupCheckpoint("ui controls created");

    utils::logDebug("AudioBatchComponent startup: opening analysis cache");
    const auto analysisCacheOpened = analysisCache.open();
    if (analysisCacheOpened) {
        logStartupCheckpoint("analysis cache opened");

        // Checking every cached path touches the file system, so it runs in the background.
        cachePruneThread = std::jthread([this](const std::stop_token& stopToken) {
            analysisCache.pruneMissingFiles([&stopToken] { return stopToken.stop_requested(); });
        });
    } else {
        utils::logError("AudioBatchComponent startup: analysis cache open failed");
    }

    setOpaque(true);
    setSize(layout::initialComponentWidth, layout::initialComponentHeight);
    setWantsKeyboardFocus(true);
    logStartupCheckpoint("constructor complete");
}

/// Stops background work and releases audio and dialog resources.
AudioBatchComponent::~AudioBatchComponent()
{
    previewAnimationAttachment = {};
    if (settingsWindow != nullptr) {
        settingsWindow->removeComponentListener(this);
    }
    previewPlayer.setDeviceSettingsActive(false);
    previewPlayer.release();
    fileActivity.clearAllActive();
    utils::logDebug("Application shutdown: stopping preview metadata worker");
    preview.stop();
    // A plugin job that hangs makes the shutdown end the process, so the chain is saved first.
    liveChain->saveChain();
    utils::logDebug("Application shutdown: stopping analysis, normalization, and plugin processing workers");
    backgroundWork.cancelAndWait();

    utils::logDebug("Application shutdown: releasing preview and dialogs");
    previewPlayer.getDeviceManager().removeChangeListener(this);
    previewPlayer.removeChangeListener(this);
    previewPlayer.setErrorCallback({});
    liveChain->setChainChangedCallback({});
    liveChain->setLatencyChangedCallback({});
    previewPlayer.setLiveChain(nullptr);
    previewWorkspace.thumbnail.removeChangeListener(this);
    previewWorkspace.thumbnail.setMouseWheelZoomCallback({});
    previewWorkspace.thumbnail.setMouseWheelGainCallback({});
    previewWorkspace.thumbnail.setThumbnailFullyLoadedCallback({});
    audioSettingsComponent.setPreferencesChangedCallback({});
    resultsTable.setModel(nullptr);

    if (childWindows.size() != 0) {
        for (auto& window : childWindows) {
            window.deleteAndZero();
        }
        childWindows.clear();
    }
    utils::logDebug("Application shutdown: releasing component members");
}

/// Fills the background with the look-and-feel window colour.
void AudioBatchComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

/// Distributes leftover table width between the name and path columns while preserving their current ratio.
void AudioBatchComponent::updateResultsTableColumnWidths() const
{
    auto& header = resultsTable.getHeader();

    // Every column other than name and path keeps its width, so only visible ones take space from the flexible columns.
    constexpr std::array fixedWidthColumns {
        AudioFileTableColumn::type,
        AudioFileTableColumn::bitDepth,
        AudioFileTableColumn::bitrate,
        AudioFileTableColumn::sampleRate,
        AudioFileTableColumn::length,
        AudioFileTableColumn::overallPeak,
        AudioFileTableColumn::peakLeft,
        AudioFileTableColumn::peakRight,
        AudioFileTableColumn::overallTruePeak,
        AudioFileTableColumn::maxShortTermLufs,
        AudioFileTableColumn::integratedLufs,
        AudioFileTableColumn::customGain,
        AudioFileTableColumn::status,
    };
    int fixedColumnWidth = 0;
    for (const auto column : fixedWidthColumns) {
        if (const auto columnId = std::to_underlying(column); header.isColumnVisible(columnId)) {
            fixedColumnWidth += header.getColumnWidth(columnId);
        }
    }

    const auto availableFlexibleWidth = resultsTable.getVisibleContentWidth() - fixedColumnWidth;
    const auto currentNameWidth = header.getColumnWidth(std::to_underlying(AudioFileTableColumn::name));
    const auto currentPathWidth = header.getColumnWidth(std::to_underlying(AudioFileTableColumn::path));
    const auto currentFlexibleWidth = currentNameWidth + currentPathWidth;
    constexpr auto defaultFlexibleWidth = layout::table::nameColumnDefaultWidth + layout::table::pathColumnDefaultWidth;
    const auto widthRatio
        = static_cast<double>(currentFlexibleWidth > 0 ? currentNameWidth : layout::table::nameColumnDefaultWidth)
        / static_cast<double>(currentFlexibleWidth > 0 ? currentFlexibleWidth : defaultFlexibleWidth);

    int nameWidth = layout::table::nameColumnMinimumWidth;
    int pathWidth = layout::table::pathColumnMinimumWidth;

    if (availableFlexibleWidth >= layout::table::nameColumnMinimumWidth + layout::table::pathColumnMinimumWidth) {
        nameWidth = juce::roundToInt(static_cast<double>(availableFlexibleWidth) * widthRatio);
        nameWidth = juce::jlimit(
            layout::table::nameColumnMinimumWidth,
            availableFlexibleWidth - layout::table::pathColumnMinimumWidth,
            nameWidth
        );
        pathWidth = availableFlexibleWidth - nameWidth;
    }

    header.setColumnWidth(std::to_underlying(AudioFileTableColumn::name), nameWidth);
    header.setColumnWidth(std::to_underlying(AudioFileTableColumn::path), pathWidth);
}

/// True when the table has at least one selected row.
bool AudioBatchComponent::hasSelectedRecords() const
{
    return resultsTable.getNumSelectedRows() > 0;
}

/// True when the table currently holds any analyzed records.
bool AudioBatchComponent::hasAnyRecords() const
{
    return !analysisResults.empty();
}

/// Opens the audio-device settings dialog.
void AudioBatchComponent::showAudioSettingsWindow()
{
    previewPlayer.setDeviceSettingsActive(true);
    openDialogWindow(
        settingsWindow,
        &audioSettingsComponent,
        "Audio Settings",
        layout::audioSettings::preferredWidth,
        layout::audioSettings::preferredHeight
    );
    settingsWindow->addComponentListener(this);
}

/// Releases the temporary settings lease when the dialog is hidden, including its native close button.
void AudioBatchComponent::componentVisibilityChanged(juce::Component& component)
{
    if (&component == settingsWindow.getComponent()) {
        previewPlayer.setDeviceSettingsActive(component.isVisible());
    }
}

/// Returns the selected files that still exist on disk.
juce::Array<juce::File> AudioBatchComponent::getSelectedRecordFiles() const
{
    juce::Array<juce::File> selectedFiles;
    const auto selectedRows = resultsTable.getSelectedRows();

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!analysisResults.containsRow(rowNumber)) {
            continue;
        }

        if (const auto& file = analysisResults[rowNumber].file; file.existsAsFile()) {
            selectedFiles.add(file);
        }
    }

    return selectedFiles;
}

/// Returns every analyzed row whose file still exists, in table order.
std::vector<AudioAnalysisRecord> AudioBatchComponent::getAllReadyRecords() const
{
    std::vector<AudioAnalysisRecord> records;
    for (const auto& record : analysisResults) {
        if (record.isReady() && record.file.existsAsFile()) {
            records.push_back(record);
        }
    }
    return records;
}

/// Returns the selected records when every one of them exists on disk and is fully analyzed.
/// Returns an empty list as soon as any selected record is missing or unfinished,
/// so normalization is all-or-nothing.
std::vector<AudioAnalysisRecord> AudioBatchComponent::getSelectedNormalizableRecords() const
{
    std::vector<AudioAnalysisRecord> selectedRecords;
    const auto selectedRows = resultsTable.getSelectedRows();

    selectedRecords.reserve(static_cast<std::size_t>(selectedRows.size()));

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!analysisResults.containsRow(rowNumber)) {
            continue;
        }

        const auto& record = analysisResults[rowNumber];

        if (!record.file.existsAsFile() || !record.isReady()) {
            selectedRecords.clear();
            return selectedRecords;
        }

        selectedRecords.push_back(record);
    }

    return selectedRecords;
}

/// Returns the full paths of the selected rows, used to restore the selection
/// after re-sorting.
juce::StringArray AudioBatchComponent::getSelectedRecordPaths() const
{
    juce::StringArray selectedPaths;
    const auto selectedRows = resultsTable.getSelectedRows();

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!analysisResults.containsRow(rowNumber)) {
            continue;
        }

        selectedPaths.add(analysisResults[rowNumber].fullPath);
    }

    return selectedPaths;
}

/// Re-selects rows by file path after the table order or contents have changed.
void AudioBatchComponent::restoreSelectionByPaths(const juce::StringArray& selectedPaths)
{
    juce::SparseSet<int> selectedRows;

    for (const auto& path : selectedPaths) {
        if (const auto rowNumber = analysisResults.findRow(path)) {
            selectedRows.addRange(juce::Range<int>::withStartAndLength(*rowNumber, 1));
        }
    }

    resultsTable.setSelectedRows(selectedRows, juce::dontSendNotification);
}

/// Repaints the status column of the results table, the only column that shows the activities and messages,
/// so the activity animation does not repaint every other cell on each display frame.
/// A hidden status column has nothing to repaint.
void AudioBatchComponent::repaintStatusColumn()
{
    const auto column = resultsTable.getCellPosition(std::to_underlying(AudioFileTableColumn::status), 0, true);
    if (column.getWidth() > 0) {
        resultsTable.repaint(column.getX(), 0, column.getWidth(), resultsTable.getHeight());
    }
}

/// Re-sorts the results, refreshes the table, and selects the given paths again.
/// The old selection is cleared without a notification first,
/// because updateContent synchronously reports a selection it trimmed to a shorter table,
/// and those row numbers point at different files after the change.
void AudioBatchComponent::refreshTablePreservingSelection(const juce::StringArray& selectedPaths)
{
    analysisResults.sort(sortState);
    resultsTable.setSelectedRows({}, juce::dontSendNotification);
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(selectedPaths);
}

/// Enables the process button only when an enabled plugin chain exists,
/// records are loaded, and no processing or analysis is running.
void AudioBatchComponent::updateProcessButtonState()
{
    // Collecting the normalizable selection checks each file on disk, so it is skipped while a pass runs.
    bool normalizeEnabled = false;
    if (backgroundWork.isIdle()) {
        const auto normalizable = getSelectedNormalizableRecords();
        normalizeEnabled = !normalizable.empty() && canNormalizeRecords(normalizable);
    }
    previewWorkspace.normalizationTargetControl.setNormalizeEnabled(normalizeEnabled);

    // While a pass runs, the button cancels it.
    if (!backgroundWork.isIdle()) {
        previewWorkspace.processButton.setButtonText("Cancel");
        previewWorkspace.processButton.setTooltip("Cancel the running analysis, normalization, or processing.");
        previewWorkspace.processButton.setEnabled(!isBackgroundWorkCancelling(backgroundWork.getWork()));
        return;
    }

    const bool hasChain = liveChain->getNumEnabledValidSlots() > 0;
    previewWorkspace.processButton.setButtonText("Process");
    previewWorkspace.processButton.setTooltip("Process selected files through the plugin chain.");
    previewWorkspace.processButton.setEnabled(hasChain && hasAnyRecords());
}

/// Shows the most relevant progress or summary text for the current background activity.
void AudioBatchComponent::updateStatusLabel()
{
    updateProgressIndicators();
    // Every start, result, and completion of a pass updates the status, so the Process or Cancel button follows it.
    updateProcessButtonState();

    // A pass whose file count is not known yet shows the summary of the list instead.
    const auto progressText = getProgressText(backgroundWork.getWork());

    if (progressText.isNotEmpty()) {
        statusBar.setStatusText(progressText);
        return;
    }

    if (analysisResults.empty()) {
        statusBar.setStatusText("No supported audio files found");
        return;
    }

    statusBar.setStatusText(utils::format("Loaded {} files", analysisResults.size()));
}

/// Shows the overall progress of the running analysis, normalization, or processing pass in the window title
/// and as the thin line along the top edge of the status bar, and restores the plain title when idle.
/// The line moves in tenths of a percent, while the title only changes once per whole percent.
/// Runs on every activity animation tick,
/// so it only touches the title and the line when their values change.
void AudioBatchComponent::updateProgressIndicators()
{
    const auto [activityName, fraction]
        = getBackgroundProgress(backgroundWork.getWork(), fileActivity.getProgressTotal());

    /// Converts a fraction to the tenths of a percent the status bar draws.
    constexpr float permillePerUnit = 1000.0f;
    /// Converts a fraction to the whole percent shown in the title.
    constexpr float percentPerUnit = 100.0f;

    juce::String title = ProjectInfo::projectName;

    if (fraction.has_value()) {
        title = utils::format("{} - {} {}%", title, activityName, static_cast<int>(*fraction * percentPerUnit));
    } else if (activityName.isNotEmpty()) {
        title = utils::format("{} - {}", title, activityName);
    }

    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>();
        window != nullptr && window->getName() != title)
    {
        window->setName(title);
    }

    statusBar.setProgress(fraction.has_value() ? juce::roundToInt(*fraction * permillePerUnit) : -1);
}

/// Shows the preview output device and its configuration at the right end of the status bar.
/// Called on device manager changes and preview player changes, which include the device opening and closing.
void AudioBatchComponent::updateOutputStatus()
{
    statusBar.setOutputStatus(previewPlayer.getOutputStatus());
}

/// Applies a new primary sort and refreshes the table, keeping the selected rows selected.
void AudioBatchComponent::handleSortRequested(const std::optional<AudioFileTableColumn> column, const bool isForwards)
{
    results_sorting::setPrimarySort(sortState, column, isForwards);

    refreshSortIndicators();

    const auto selectedPaths = getSelectedRecordPaths();

    refreshTablePreservingSelection(selectedPaths);
}

/// Updates the secondary sort requested with a ctrl or cmd click on a header
/// and refreshes the table.
void AudioBatchComponent::handleSecondarySortRequested(const AudioFileTableColumn column)
{
    results_sorting::toggleSecondarySort(sortState, column);

    refreshSortIndicators();

    const auto selectedPaths = getSelectedRecordPaths();

    refreshTablePreservingSelection(selectedPaths);
}

/// Updates the header text so the secondary sort key remains visible.
void AudioBatchComponent::refreshSortIndicators() const
{
    results_sorting::getSecondarySortHeader(resultsTable)
        .setSecondarySortIndicator(sortState.secondaryColumn, sortState.secondaryForwards);
}

/// Picks the row whose details should be shown for a multi-selection:
/// the current preview file when it is selected, otherwise the last clicked or the first selected row.
int AudioBatchComponent::getSelectionDisplayRow(
    const juce::SparseSet<int>& selectedRows,
    const int lastRowSelected
) const
{
    if (selectedRows.isEmpty()) {
        return -1;
    }

    if (const auto previewFile = preview.getFile(); previewFile.existsAsFile()) {
        if (const auto currentRow = analysisResults.findRow(previewFile.getFullPathName());
            currentRow.has_value() && selectedRows.contains(*currentRow))
        {
            return *currentRow;
        }
    }

    if (analysisResults.containsRow(lastRowSelected) && selectedRows.contains(lastRowSelected)) {
        return lastRowSelected;
    }

    return selectedRows[0];
}

/// Loads the newly selected file into the preview and refreshes the info panel
/// and gain controls.
void AudioBatchComponent::handleSelectionChanged(const int lastRowSelected)
{
    const auto selectedRows = resultsTable.getSelectedRows();
    const auto row = getSelectionDisplayRow(selectedRows, lastRowSelected);
    updateProcessButtonState();

    if (!analysisResults.containsRow(row)) {
        return;
    }

    const auto& record = analysisResults[row];

    if (record.file.existsAsFile() && preview.getFile() != record.file) {
        const bool continuePlaying
            = shouldContinuePreviewPlayback(previewPreferences, previewPlayer.isPlaying(), !automaticSelectionChange);
        showAudioFile(record.file);
        if (continuePlaying && previewPlayer.hasLoadedSource()) {
            previewPlayer.start();
        }
        utils::logDebug("Loaded file: {}", record.fileName);
    }

    updateAudioInfo(record);
    updateGainControlsForSelection();
    updateThumbnailDisplayGain();
}

/// Shows a reusable dialog window, creating it lazily on first use.
/// The window does not take ownership of the content component.
void AudioBatchComponent::openDialogWindow(
    SafePointer<juce::DialogWindow>& window,
    const SafePointer<juce::Component>& component,
    const juce::String& title,
    const int initialWidth,
    const int initialHeight
)
{
    // Create the window the first time it is opened.
    if (window == nullptr) {
        juce::DialogWindow::LaunchOptions launchOptions;
        launchOptions.dialogTitle = title;
        launchOptions.content.setNonOwned(component);
        launchOptions.content->setSize(initialWidth, initialHeight);
        window = launchOptions.create();
        childWindows.add(window);
    }
#if JUCE_WINDOWS
    window->setUsingNativeTitleBar(false);
    window->setTitleBarTextCentred(false);
    window->setTitleBarHeight(layout::titleBarHeight);
#else
    window->setUsingNativeTitleBar(true);
#endif
    window->setSize(window->getWidth(), window->getHeight());
    window->setResizable(true, true);
    window->setResizeLimits(
        window->getWidth(), window->getHeight(), layout::dialogMaximumWindowSize, layout::dialogMaximumWindowSize
    );
    window->setColour(juce::DialogWindow::backgroundColourId, CustomLookAndFeel::greySemiDark());
    window->setVisible(true);
    window->toFront(true);
}

/// Returns the selected records that exist on disk, are fully analyzed, and have a processable format.
std::vector<AudioAnalysisRecord> AudioBatchComponent::getSelectedProcessableRecords() const
{
    std::vector<AudioAnalysisRecord> selected;
    const auto selectedRows = resultsTable.getSelectedRows();

    selected.reserve(static_cast<std::size_t>(selectedRows.size()));

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!analysisResults.containsRow(rowNumber)) {
            continue;
        }

        const auto& record = analysisResults[rowNumber];

        if (!record.file.existsAsFile() || !record.isReady()) {
            continue;
        }

        if (!canProcessFileWithPlugins(record.file)) {
            continue;
        }

        selected.push_back(record);
    }

    return selected;
}
}  // namespace audiobatch
