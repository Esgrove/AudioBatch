/// Implementation of the main GUI component:
/// construction, layout, the results table, selection, sorting, the status label, and dialog windows.
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

namespace audiobatch
{
/// Creates the results table header, model, and selection behavior.
void AudioBatchComponent::configureResultsTable()
{
    resultsTable.setHeader(std::make_unique<SecondarySortTableHeader>([this](const AudioFileTableColumn column) {
        handleSecondarySortRequested(column);
    }));
    AudioFileTableModel::configureHeader(resultsTable.getHeader());
    resultsTable.setModel(&fileTableModel);
    resultsTable.setColour(juce::ListBox::backgroundColourId, juce::CustomLookAndFeel::greyMediumDark());
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

/// Creates the waveform preview, transport and zoom controls, and the metadata info panel.
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
    juce::Path chevronPath;
    chevronPath.startNewSubPath(0.0f, 0.0f);
    chevronPath.lineTo(layout::previewToggle::glyphWidth * 0.5f, layout::previewToggle::glyphHeight);
    chevronPath.lineTo(layout::previewToggle::glyphWidth, 0.0f);
    juce::DrawablePath collapseChevron;
    collapseChevron.setPath(chevronPath);
    collapseChevron.setFill(juce::Colours::transparentBlack);
    collapseChevron.setStrokeFill(juce::CustomLookAndFeel::greyMiddleLight());
    collapseChevron.setStrokeThickness(layout::previewToggle::strokeWidth);
    chevronPath.applyTransform(
        juce::AffineTransform::rotation(
            juce::MathConstants<float>::pi,
            layout::previewToggle::glyphWidth * 0.5f,
            layout::previewToggle::glyphHeight * 0.5f
        )
    );
    juce::DrawablePath expandChevron;
    expandChevron.setPath(chevronPath);
    expandChevron.setFill(juce::Colours::transparentBlack);
    expandChevron.setStrokeFill(juce::CustomLookAndFeel::greyMiddleLight());
    expandChevron.setStrokeThickness(layout::previewToggle::strokeWidth);
    previewSectionButton.setImages(&collapseChevron, nullptr, nullptr, nullptr, &expandChevron);
    previewSectionButton.setEdgeIndent(layout::previewToggle::imageInset);
    previewSectionButton.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    previewSectionButton.setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
    previewSectionButton.setTooltip("Collapse bottom section (Ctrl+Shift+Space / Cmd+Shift+Space).");
    previewSectionButton.onClick = [this] { togglePreviewSection(); };
    addAndMakeVisible(previewSectionButton);

    controlsViewport.setViewedComponent(&controlsContent, false);
    controlsViewport.setScrollBarsShown(true, false);
    controlsViewport.setScrollBarThickness(layout::scrollBarThickness);
    controlsViewport.setName("Preview and processing controls");
    addAndMakeVisible(controlsViewport);

    thumbnail = std::make_unique<ThumbnailComponent>(previewPlayer.getFormatManager(), previewPlayer);
    addAndMakeVisible(thumbnail.get());
    thumbnail->addChangeListener(this);
    previewPlayer.addChangeListener(this);
    thumbnail->setMouseWheelZoomCallback([this](const double zoomFactor) {
        zoomSlider.setValue(zoomSlider.getValue() * zoomFactor);
    });
    thumbnail->setMouseWheelGainCallback([this](const float deltaDb) {
        if (!gainSlider.isEnabled() || juce::approximatelyEqual(deltaDb, 0.0f)) {
            return;
        }
        const auto interval = gainSlider.getInterval() > 0.0 ? gainSlider.getInterval() : gain_range::stepDecibels;
        const auto step = (deltaDb > 0.0f ? 1.0 : -1.0) * interval;
        const auto newValue
            = juce::jlimit(gainSlider.getMinimum(), gainSlider.getMaximum(), gainSlider.getValue() + step);
        gainSlider.setValue(newValue, juce::sendNotificationSync);
    });
    thumbnail->setThumbnailFullyLoadedCallback([this] { handleThumbnailFullyLoaded(); });

    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium());
    startStopButton.setColour(juce::TextButton::buttonOnColourId, juce::CustomLookAndFeel::green());
    startStopButton.setTooltip("Start or stop preview playback for the selected file.");
    startStopButton.onClick = [this] { previewPlayer.togglePlayback(); };
    addAndMakeVisible(startStopButton);

    zoomSlider.setRange(zoom_range::minimumPercent, zoom_range::maximumPercent, 1);
    zoomSlider.setNumDecimalPlacesToDisplay(0);
    zoomSlider.setSkewFactor(0.5);
    zoomSlider.setValue(zoom_range::minimumPercent);
    zoomSlider.setTextValueSuffix("%");
    zoomSlider.setTooltip("Adjust waveform zoom. You can also use the mouse wheel over the waveform.");
    zoomSlider.onValueChange = [this] { zoomLevelChanged(zoomSlider.getValue() * zoom_range::percentToScaleFactor); };
    controlsContent.addAndMakeVisible(zoomSlider);

    zoomLabel.setText("Zoom", juce::dontSendNotification);
    zoomLabel.setJustificationType(juce::Justification::centredLeft);
    zoomSlider.setTextBoxStyle(
        juce::Slider::TextBoxRight, false, layout::zoomTextBoxWidth, zoomSlider.getTextBoxHeight()
    );
    controlsContent.addAndMakeVisible(zoomLabel);

    audioInfo = std::make_unique<AudioInfoPanel>();
    addAndMakeVisible(*audioInfo);
}

/// Creates the per-file gain, normalization toggle, process, and settings controls,
/// and connects the plugin chain to the preview.
void AudioBatchComponent::configureProcessingControls()
{
    const auto refreshLiveChain = [this] {
        const auto refreshed = previewPlayer.refreshPluginChain();
        juce::ignoreUnused(refreshed);
        previewPreferences.pluginChainEnabled = previewPlayer.isPluginPreviewEnabled();
        synchronizePreviewPreferences();
        updateProcessButtonState();
    };
    pluginChain->setChainChangedCallback(refreshLiveChain);
    pluginChain->setLatencyChangedCallback(refreshLiveChain);
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

    pluginButton.setTooltip("Edit the plugin chain, add plugins, or scan for plugins.");
    pluginButton.onClick = [this] { pluginChain->showMenu(pluginButton); };
    controlsContent.addAndMakeVisible(pluginButton);

    gainLabel.setText("Input gain", juce::dontSendNotification);
    gainLabel.setJustificationType(juce::Justification::centredLeft);
    controlsContent.addAndMakeVisible(gainLabel);

    gainSlider.setRange(-gain_range::maximumDecibels, gain_range::maximumDecibels, gain_range::stepDecibels);
    gainSlider.setValue(0.0, juce::dontSendNotification);
    gainSlider.setNumDecimalPlacesToDisplay(1);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setTooltip("Per-file gain in dB. Non-zero values override normalization for the file.");
    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(
        juce::Slider::TextBoxRight, false, layout::gainTextBoxWidth, gainSlider.getTextBoxHeight()
    );
    gainSlider.setDoubleClickReturnValue(true, 0.0);
    gainSlider.setEnabled(false);
    gainSlider.onValueChange = [this] {
        const auto value = static_cast<float>(gainSlider.getValue());
        applyCustomGainToSelection(value, !juce::approximatelyEqual(value, 0.0f));
    };
    controlsContent.addAndMakeVisible(gainSlider);

    normalizeBeforePluginToggle.setTooltip(
        "When enabled, files without a custom gain are normalized to the selected peak or loudness target "
        "before the plugin chain."
    );
    normalizeBeforePluginToggle.onClick
        = [this] { normalizationTargetControl.setEnabled(normalizeBeforePluginToggle.getToggleState()); };
    controlsContent.addAndMakeVisible(normalizeBeforePluginToggle);

    forceOverwriteToggle.setTooltip(
        "When enabled, normalizing or processing replaces an existing output file with the same name. "
        "The replaced file is moved to the system trash."
    );
    controlsContent.addAndMakeVisible(forceOverwriteToggle);

    normalizationTargetControl.loadSettings();
    normalizationTargetControl.setEnabled(normalizeBeforePluginToggle.getToggleState());
    controlsContent.addAndMakeVisible(normalizationTargetControl);

    processButton.setTooltip("Process selected files through the plugin chain.");
    updateProcessButtonState();
    processButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::blue());
    processButton.setColour(juce::TextButton::textColourOffId, juce::CustomLookAndFeel::greyDark());
    processButton.onClick = [this] { processSelectedRecords(); };
    addAndMakeVisible(processButton);

    settingsButton.setTooltip("Open audio device and preview settings.");
    settingsButton.onClick = [this] { showAudioSettingsWindow(); };
    controlsContent.addAndMakeVisible(settingsButton);
}

/// Connects the per-file progress reports of the coordinators to the activity tracker on the message thread.
/// Normalization and processing files switch from waiting to busy when a worker starts on them.
/// A start arriving after its pass ended belongs to no pass, so it does not mark the file busy again.
void AudioBatchComponent::connectProgressCallbacks()
{
    const SafePointer safeThis(this);

    analysisCoordinator.setProgressCallback([safeThis](const juce::File& file, const float progress, const int runId) {
        juce::MessageManager::callAsync([safeThis, file, progress, runId] {
            if (safeThis != nullptr && runId == safeThis->analysisRun.runId) {
                safeThis->fileActivity.setProgress(file.getFullPathName(), progress);
            }
        });
    });

    const auto postStarting = [safeThis](const FileActivity activity) {
        return [safeThis, activity](const juce::File& file) {
            juce::MessageManager::callAsync([safeThis, file, activity] {
                if (safeThis != nullptr && safeThis->isFileOperationInProgress()) {
                    safeThis->fileActivity.setActive(file.getFullPathName(), activity);
                }
            });
        };
    };

    const auto postProgress = [safeThis](const juce::File& file, const float progress) {
        juce::MessageManager::callAsync([safeThis, file, progress] {
            if (safeThis != nullptr) {
                safeThis->fileActivity.setProgress(file.getFullPathName(), progress);
            }
        });
    };

    normalizeCoordinator.setStartingCallback(postStarting(FileActivity::normalizing));
    normalizeCoordinator.setProgressCallback(postProgress);
    pluginCoordinator.setStartingCallback(postStarting(FileActivity::processing));
    pluginCoordinator.setProgressCallback(postProgress);
}

/// Connects the analysis, normalization, and plugin coordinators to the message thread handlers.
void AudioBatchComponent::connectCoordinatorCallbacks()
{
    const SafePointer safeThis(this);

    seratoPreviewLoader = std::make_unique<serato::SeratoPreviewLoader>(
        analysisCache,
        [safeThis](
            FileIdentity identity, const std::uint64_t generation, std::shared_ptr<const serato::SeratoData> data
        ) {
            juce::MessageManager::callAsync(
                [safeThis, previewIdentity = std::move(identity), generation, previewData = std::move(data)] {
                    if (safeThis != nullptr) {
                        safeThis->adoptSeratoPreview(previewIdentity, generation, previewData);
                    }
                }
            );
        }
    );

    // Coordinator callbacks arrive on worker threads.
    // The handler runs on the message thread, and only while this component still exists.
    const auto postToMessageThread = [safeThis](auto handler) {
        return [safeThis, handler](const auto&... arguments) {
            juce::MessageManager::callAsync([safeThis, handler, arguments...] {
                if (safeThis != nullptr) {
                    (safeThis.getComponent()->*handler)(arguments...);
                }
            });
        };
    };

    analysisCoordinator.setPlanCallback(postToMessageThread(&AudioBatchComponent::handleAnalysisPlan));
    analysisCoordinator.setResultCallback([this](const AudioAnalysisRecord& record, const int runId) {
        analysisResultBatcher.post({.record = record, .runId = runId});
    });
    analysisCoordinator.setCompletionCallback(postToMessageThread(&AudioBatchComponent::handleAnalysisComplete));
    analysisCoordinator.setStartingCallback([safeThis](const juce::File& file, const int runId) {
        juce::MessageManager::callAsync([safeThis, file, runId] {
            if (safeThis != nullptr && runId == safeThis->analysisRun.runId) {
                safeThis->fileActivity.setActive(file.getFullPathName(), FileActivity::analyzing);
            }
        });
    });
    connectProgressCallbacks();

    normalizeCoordinator.setResultCallback(postToMessageThread(&AudioBatchComponent::handleNormalizeResult));
    normalizeCoordinator.setCompletionCallback(postToMessageThread(&AudioBatchComponent::handleNormalizeComplete));

    pluginCoordinator.setResultCallback(postToMessageThread(&AudioBatchComponent::handleProcessingResult));
    pluginCoordinator.setCompletionCallback(postToMessageThread(&AudioBatchComponent::handleProcessingComplete));

    pluginCoordinator.setStartErrorCallback([safeThis](const juce::String& errorMessage) {
        juce::MessageManager::callAsync([safeThis, errorMessage] {
            if (safeThis != nullptr) {
                safeThis->processingProgress.inProgress = false;
                safeThis->statusLabel.setText(errorMessage, juce::dontSendNotification);
                safeThis->showPluginProcessingWarning(errorMessage);
            }
        });
    });
}

/// Creates the UI, opens the analysis cache, and starts the initial scan.
AudioBatchComponent::AudioBatchComponent() :
    analysisCoordinator(analysisCache),
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
    pluginChain = std::make_unique<PluginChain>(
        pluginAppProperties, previewPlayer.getRenderingLock(), previewPlayer.getDeviceManager()
    );
    previewPlayer.setPluginChain(pluginChain.get());

    currentRootLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(currentRootLabel);

    statusLabel.setTooltip("Background analysis and normalization status.");
    statusLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(statusLabel);

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

    connectCoordinatorCallbacks();
    logStartupCheckpoint("analysis and normalization callbacks configured");

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
    if (seratoPreviewLoader) {
        seratoPreviewLoader->cancelAndWait();
    }
    utils::logDebug("Application shutdown: stopping analysis workers");
    analysisCoordinator.cancelAndWait();
    utils::logDebug("Application shutdown: stopping normalization workers");
    normalizeCoordinator.cancelAndWait();
    utils::logDebug("Application shutdown: stopping plugin processing workers");
    pluginCoordinator.cancelAndWait();

    utils::logDebug("Application shutdown: releasing preview and dialogs");
    previewPlayer.removeChangeListener(this);
    previewPlayer.setErrorCallback({});
    pluginChain->setChainChangedCallback({});
    pluginChain->setLatencyChangedCallback({});
    previewPlayer.setPluginChain(nullptr);
    thumbnail->removeChangeListener(this);
    thumbnail->setMouseWheelZoomCallback({});
    thumbnail->setMouseWheelGainCallback({});
    thumbnail->setThumbnailFullyLoadedCallback({});
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

/// Fills the background with the look-and-feel window colour,
/// and draws the overall progress bar above the results table while a pass of known size runs.
void AudioBatchComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    if (overallProgressPermille < 0) {
        return;
    }

    /// Converts the stored tenths of a percent to a fraction.
    constexpr float permillePerUnit = 1000.0f;
    /// Opacity of the unfilled track behind the overall progress bar.
    constexpr float progressTrackAlpha = 0.25f;

    const auto bar = getOverallProgressBounds().toFloat();
    graphics.setColour(juce::CustomLookAndFeel::greyMiddle().withAlpha(progressTrackAlpha));
    graphics.fillRect(bar);
    graphics.setColour(juce::CustomLookAndFeel::blue());
    graphics.fillRect(bar.withWidth(bar.getWidth() * static_cast<float>(overallProgressPermille) / permillePerUnit));
}

/// Returns the area of the thin overall progress bar, in the gap between the top bar and the results table.
juce::Rectangle<int> AudioBatchComponent::getOverallProgressBounds() const
{
    const auto tableBounds = resultsTable.getBounds();
    return {
        tableBounds.getX(),
        tableBounds.getY() - (layout::sectionSpacing + layout::overallProgressBarHeight) / 2,
        tableBounds.getWidth(),
        layout::overallProgressBarHeight,
    };
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

/// Returns the current row index of the record with the given full path, or -1
/// when it is not in the table.
int AudioBatchComponent::findRecordIndex(const juce::String& fullPath) const
{
    for (std::size_t index = 0; index < analysisResults.size(); ++index) {
        if (analysisResults[index].fullPath == fullPath) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

/// Returns the selected files that still exist on disk.
juce::Array<juce::File> AudioBatchComponent::getSelectedRecordFiles() const
{
    juce::Array<juce::File> selectedFiles;
    const auto selectedRows = resultsTable.getSelectedRows();

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        if (const auto& file = analysisResults[static_cast<std::size_t>(rowNumber)].file; file.existsAsFile()) {
            selectedFiles.addIfNotAlreadyThere(file);
        }
    }

    return selectedFiles;
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

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        const auto& record = analysisResults[static_cast<std::size_t>(rowNumber)];

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

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        selectedPaths.addIfNotAlreadyThere(analysisResults[static_cast<std::size_t>(rowNumber)].fullPath);
    }

    return selectedPaths;
}

/// Re-selects rows by file path after the table order or contents have changed.
void AudioBatchComponent::restoreSelectionByPaths(const juce::StringArray& selectedPaths)
{
    juce::SparseSet<int> selectedRows;

    for (const auto& path : selectedPaths) {
        if (const auto rowNumber = findRecordIndex(path); rowNumber >= 0) {
            selectedRows.addRange(juce::Range<int>::withStartAndLength(rowNumber, 1));
        }
    }

    resultsTable.setSelectedRows(selectedRows, juce::dontSendNotification);
}

/// Re-sorts the results, refreshes the table, and selects the given paths again.
/// The old selection is cleared without a notification first,
/// because updateContent synchronously reports a selection it trimmed to a shorter table,
/// and those row numbers point at different files after the change.
void AudioBatchComponent::refreshTablePreservingSelection(const juce::StringArray& selectedPaths)
{
    sortResults();
    resultsTable.setSelectedRows({}, juce::dontSendNotification);
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(selectedPaths);
}

/// Enables the process button only when an enabled plugin chain exists,
/// records are loaded, and no processing or analysis is running.
void AudioBatchComponent::updateProcessButtonState()
{
    const bool hasChain = pluginChain->getNumEnabledValidSlots() > 0;
    processButton.setEnabled(hasChain && hasAnyRecords() && !processingProgress.inProgress && !isAnalysisInProgress());
}

/// Shows the most relevant progress or summary text for the current background activity.
void AudioBatchComponent::updateStatusLabel()
{
    updateProgressIndicators();

    if (processingProgress.inProgress && processingProgress.expected > 0) {
        statusLabel.setText(
            utils::format("Processing {}/{}", processingProgress.completed, processingProgress.expected),
            juce::dontSendNotification
        );
        return;
    }

    if (normalizeProgress.inProgress && normalizeProgress.expected > 0) {
        statusLabel.setText(
            utils::format("Normalizing {}/{}", normalizeProgress.completed, normalizeProgress.expected),
            juce::dontSendNotification
        );
        return;
    }

    if (analysisRun.running) {
        statusLabel.setText(
            analysisRun.planReceived ? utils::format("Analyzing {}/{}", analysisRun.completed, analysisRun.expected)
                                     : juce::String("Scanning for audio files..."),
            juce::dontSendNotification
        );
        return;
    }

    if (analysisResults.empty()) {
        statusLabel.setText("No supported audio files found", juce::dontSendNotification);
        return;
    }

    statusLabel.setText(utils::format("Loaded {} files", analysisResults.size()), juce::dontSendNotification);
}

/// Shows the overall progress of the running analysis, normalization, or processing pass in the window title
/// and as the thin bar above the results table, and restores the plain title when idle.
/// The progress counts finished files plus the completed fractions of the files being worked on,
/// so it advances smoothly within long files instead of jumping once per file.
/// Runs on every activity animation tick,
/// so it only touches the title and the bar when their values change.
void AudioBatchComponent::updateProgressIndicators()
{
    juce::String activityName;
    std::optional<float> fraction;

    const auto getRunFraction = [this](const int completed, const int expected) -> std::optional<float> {
        if (expected <= 0) {
            return std::nullopt;
        }

        const auto done = static_cast<float>(completed) + fileActivity.getProgressTotal();
        return juce::jlimit(0.0f, 1.0f, done / static_cast<float>(expected));
    };

    if (processingProgress.inProgress) {
        activityName = "Processing";
        fraction = getRunFraction(processingProgress.completed, processingProgress.expected);
    } else if (normalizeProgress.inProgress) {
        activityName = "Normalizing";
        fraction = getRunFraction(normalizeProgress.completed, normalizeProgress.expected);
    } else if (analysisRun.running && analysisRun.planReceived) {
        activityName = "Analyzing";
        fraction = getRunFraction(analysisRun.completed, analysisRun.expected);
    } else if (analysisRun.running) {
        activityName = "Scanning";
    }

    /// Converts a fraction to the stored tenths of a percent.
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

    const auto permille = fraction.has_value() ? juce::roundToInt(*fraction * permillePerUnit) : -1;

    if (permille != overallProgressPermille) {
        overallProgressPermille = permille;
        repaint(getOverallProgressBounds());
    }
}

/// Sorts the records by the primary and secondary sort columns.
/// Failed files always sort last, and file name plus full path act as the final tie breakers,
/// keeping the order stable across incremental result updates.
void AudioBatchComponent::sortResults()
{
    results_sorting::sortRecords(analysisResults, sortState);
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

    if (currentAudioFile.existsAsFile()) {
        if (const auto currentRow = findRecordIndex(currentAudioFile.getFullPathName());
            currentRow >= 0 && selectedRows.contains(currentRow))
        {
            return currentRow;
        }
    }

    if (juce::isPositiveAndBelow(lastRowSelected, static_cast<int>(analysisResults.size()))
        && selectedRows.contains(lastRowSelected))
    {
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

    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(row)];

    if (record.file.existsAsFile() && currentAudioFile != record.file) {
        const bool continuePlaying
            = shouldContinuePreviewPlayback(previewPreferences, previewPlayer.isPlaying(), !automaticSelectionChange);
        currentAudioFile = record.file;
        showAudioResource(juce::URL(record.file));
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
    window->setColour(juce::DialogWindow::backgroundColourId, juce::CustomLookAndFeel::greySemiDark());
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

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        const auto& record = analysisResults[static_cast<std::size_t>(rowNumber)];

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
