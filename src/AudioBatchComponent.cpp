/// Implementation of the main GUI component: construction, layout, the results table, selection,
/// sorting, the status label, and dialog windows.
/// The other AudioBatchComponent*.cpp files implement the remaining member functions by topic.

#include "AudioBatchComponent.h"

#include "AppInfo.h"
#include "AudioAnalysisService.h"
#include "AudioBatchComponentConstants.h"
#include "AudioInfoPanel.h"
#include "CustomLookAndFeel.h"
#include "Log.h"
#include "PluginProcessingService.h"
#include "ResultsSorting.h"
#include "StringFormat.h"

using namespace audiobatch::gui;

void AudioBatchComponent::configureResultsTable()
{
    resultsTable.setHeader(std::make_unique<SecondarySortTableHeader>([this](const int columnId) {
        handleSecondarySortRequested(columnId);
    }));
    AudioFileTableModel::configureHeader(resultsTable.getHeader());
    resultsTable.setModel(&fileTableModel);
    resultsTable.setColour(juce::ListBox::backgroundColourId, juce::CustomLookAndFeel::greyMediumDark());
    resultsTable.setMultipleSelectionEnabled(true);
    resultsTable.setOutlineThickness(0);
    resultsTable.setRowHeight(layout::resultsTableRowHeight);
    resultsTable.getHeader().setSortColumnId(currentSortColumnId, currentSortForwards);
    refreshSortIndicators();
    addAndMakeVisible(resultsTable);
    resultsTable.addKeyListener(this);
}

void AudioBatchComponent::configurePreviewControls()
{
    // Default to a larger list view while allowing the lower preview area to be resized.
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

    thumbnail = std::make_unique<ThumbnailComponent>(formatManager, transportSource);
    addAndMakeVisible(thumbnail.get());
    thumbnail->addChangeListener(this);
    transportSource.addChangeListener(this);
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
    startStopButton.onClick = [this] { startOrStop(); };
    addAndMakeVisible(startStopButton);

    zoomSlider.setRange(zoom_range::minimumPercent, zoom_range::maximumPercent, 1);
    zoomSlider.setNumDecimalPlacesToDisplay(0);
    zoomSlider.setSkewFactor(0.5);
    zoomSlider.setValue(zoom_range::minimumPercent);
    zoomSlider.setTextValueSuffix("%");
    zoomSlider.setTooltip("Adjust waveform zoom. You can also use the mouse wheel over the waveform.");
    zoomSlider.onValueChange = [this] { zoomLevelChanged(zoomSlider.getValue() * zoom_range::percentToScaleFactor); };
    addAndMakeVisible(zoomSlider);

    zoomLabel.setText("Zoom", juce::dontSendNotification);
    zoomLabel.attachToComponent(&zoomSlider, true);
    zoomSlider.setTextBoxStyle(
        juce::Slider::TextBoxLeft, false, layout::zoomTextBoxWidth, zoomSlider.getTextBoxHeight()
    );
    addAndMakeVisible(zoomLabel);

    audioInfo = std::make_unique<AudioInfoPanel>();
    audioInfoViewport.setViewedComponent(audioInfo.get(), false);
    audioInfoViewport.setScrollBarsShown(true, false);
    audioInfoViewport.setScrollBarThickness(8);
    addAndMakeVisible(audioInfoViewport);

    audioInfo->setPreferredHeightChangedCallback([this] { resized(); });
}

void AudioBatchComponent::configureProcessingControls()
{
    pluginAppProperties.setStorageParameters(utils::createApplicationSettingsOptions());

    pluginChain = std::make_unique<PluginChain>(pluginAppProperties);
    pluginChain->setChainChangedCallback([this] { updateProcessButtonState(); });

    pluginButton.setTooltip("Edit the plugin chain, add plugins, or scan for plugins.");
    pluginButton.onClick = [this] {
        if (pluginChain != nullptr) {
            pluginChain->showMenu(pluginButton);
        }
    };
    addAndMakeVisible(pluginButton);

    gainLabel.setText("Gain", juce::dontSendNotification);
    gainLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(gainLabel);

    gainSlider.setRange(-gain_range::maximumDecibels, gain_range::maximumDecibels, gain_range::stepDecibels);
    gainSlider.setValue(0.0, juce::dontSendNotification);
    gainSlider.setNumDecimalPlacesToDisplay(1);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setTooltip("Per-file gain in dB. Non-zero values override normalization for the file.");
    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, gainSlider.getTextBoxHeight());
    gainSlider.setDoubleClickReturnValue(true, 0.0);
    gainSlider.setEnabled(false);
    gainSlider.onValueChange = [this] {
        const auto value = static_cast<float>(gainSlider.getValue());
        applyCustomGainToSelection(value, !juce::approximatelyEqual(value, 0.0f));
    };
    addAndMakeVisible(gainSlider);

    gainClearButton.setTooltip("Clear per-file gain for the current selection.");
    gainClearButton.setEnabled(false);
    gainClearButton.onClick = [this] {
        gainSlider.setValue(0.0, juce::dontSendNotification);
        applyCustomGainToSelection(0.0f, false);
    };
    addAndMakeVisible(gainClearButton);

    normalizeBeforePluginToggle.setTooltip(
        "When enabled, files without a custom gain are peak-normalized before the plugin chain."
    );
    addAndMakeVisible(normalizeBeforePluginToggle);

    forceOverwriteToggle.setTooltip(
        "When enabled, normalizing or processing replaces an existing output file with the same name. "
        "The replaced file is moved to the system trash."
    );
    addAndMakeVisible(forceOverwriteToggle);

    normalizationTargetControl.loadSettings();
    addAndMakeVisible(normalizationTargetControl);

    processButton.setTooltip("Process selected files through the plugin chain.");
    updateProcessButtonState();
    processButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium());
    processButton.onClick = [this] { processSelectedRecords(); };
    addAndMakeVisible(processButton);

    settingsButton.setTooltip("Open audio device settings.");
    settingsButton.onClick = [this] { showAudioSettingsWindow(); };
    addAndMakeVisible(settingsButton);
}

void AudioBatchComponent::connectCoordinatorCallbacks()
{
    const SafePointer safeThis(this);

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
    analysisCoordinator.setResultCallback(postToMessageThread(&AudioBatchComponent::handleAnalysisResult));
    analysisCoordinator.setCompletionCallback(postToMessageThread(&AudioBatchComponent::handleAnalysisComplete));
    analysisCoordinator.setStartingCallback([safeThis](const juce::File& file, const int runId) {
        juce::MessageManager::callAsync([safeThis, file, runId] {
            if (safeThis != nullptr && runId == safeThis->analysisRunId) {
                safeThis->fileActivity.setActive(file.getFullPathName(), "Analyzing");
            }
        });
    });

    normalizeCoordinator.setResultCallback(postToMessageThread(&AudioBatchComponent::handleNormalizeResult));
    normalizeCoordinator.setCompletionCallback(postToMessageThread(&AudioBatchComponent::handleNormalizeComplete));

    pluginCoordinator.setResultCallback(postToMessageThread(&AudioBatchComponent::handleProcessingResult));
    pluginCoordinator.setCompletionCallback(postToMessageThread(&AudioBatchComponent::handleProcessingComplete));

    pluginCoordinator.setStartErrorCallback([safeThis](const juce::String& errorMessage) {
        juce::MessageManager::callAsync([safeThis, errorMessage] {
            if (safeThis != nullptr) {
                safeThis->pluginProcessingInProgress = false;
                safeThis->statusLabel.setText(errorMessage, juce::dontSendNotification);
                safeThis->showPluginProcessingWarning(errorMessage);
            }
        });
    });
}

AudioBatchComponent::AudioBatchComponent() :
    analysisCoordinator(analysisCache),
    fileTableModel(
        analysisResults,
        [this](const int row) { handleSelectionChanged(row); },
        [this](const int columnId, const bool isForwards) { handleSortRequested(columnId, isForwards); },
        [this](const int row, const int columnId, const juce::MouseEvent& event) {
            handleFileContextMenuRequested(row, columnId, event);
        },
        [this](const AudioAnalysisRecord& record) { return fileActivity.getActiveLabel(record.fullPath); },
        [this](const AudioAnalysisRecord& record) { return fileActivity.getMessage(record.fullPath); },
        [] { return FileActivityTracker::getAnimationPhase(); }
    ),
    audioSetupComponent(audioDeviceManager, 0, 0, 0, 2, false, false, true, false)
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
    formatManager.registerBasicFormats();
    logStartupCheckpoint("audio formats registered");

    currentRootLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(currentRootLabel);

    statusLabel.setTooltip("Background analysis and normalization status.");
    statusLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(statusLabel);

    configureResultsTable();
    configurePreviewControls();
    configureProcessingControls();

    tooltipWindow = std::make_unique<juce::TooltipWindow>(this, tooltipDelayMilliseconds);
    logStartupCheckpoint("ui controls created");

    thread.startThread(juce::Thread::Priority::high);
    logStartupCheckpoint("audio preview thread started");

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

    requestAudioDeviceAccess();

    audioDeviceManager.addAudioCallback(&audioSourcePlayer);
    audioSourcePlayer.setSource(&transportSource);
    logStartupCheckpoint("audio callback connected");

    setOpaque(true);
    setSize(layout::initialComponentWidth, layout::initialComponentHeight);
    setWantsKeyboardFocus(true);
    logStartupCheckpoint("constructor complete");
}

AudioBatchComponent::~AudioBatchComponent()
{
    analysisCoordinator.cancelAndWait();
    normalizeCoordinator.cancelAndWait();
    pluginCoordinator.cancelAndWait();

    transportSource.stop();
    transportSource.setSource(nullptr);
    audioSourcePlayer.setSource(nullptr);

    audioDeviceManager.removeAudioCallback(&audioSourcePlayer);

    transportSource.removeChangeListener(this);
    thumbnail->removeChangeListener(this);
    thumbnail->setMouseWheelZoomCallback({});
    thumbnail->setMouseWheelGainCallback({});
    thumbnail->setThumbnailFullyLoadedCallback({});
    resultsTable.setModel(nullptr);

    if (childWindows.size() != 0) {
        for (auto& window : childWindows) {
            window.deleteAndZero();
        }
        childWindows.clear();
    }

    thread.stopThread(previewThreadStopTimeoutMilliseconds);
}

void AudioBatchComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioBatchComponent::resized()
{
    auto bounds = getLocalBounds().reduced(4);
    auto topBar = bounds.removeFromTop(layout::topBarHeight);

    statusLabel.setBounds(topBar.removeFromRight(layout::statusLabelWidth));
    topBar.removeFromRight(layout::sectionSpacing);
    currentRootLabel.setBounds(topBar);

    bounds.removeFromTop(layout::sectionSpacing);

    std::array<juce::Component*, 3> verticalSections {&resultsTable, &waveformResizeBar, thumbnail.get()};
    mainVerticalLayout.layOutComponents(
        verticalSections.data(), 3, bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), true, true
    );
    updateResultsTableColumnWidths();

    auto previewArea = thumbnail->getBounds();

    const auto space = juce::jmin(280, juce::jmax(180, juce::roundToIntAccurate(bounds.getWidth() * 0.22)));
    auto control = previewArea.removeFromLeft(space);

    const auto buttonGap = layout::sectionSpacing;

    // Bottom row: Process and Play/Stop.
    auto buttonsBottom = control.removeFromBottom(layout::controlButtonHeight);
    const auto bottomButtonWidth = (buttonsBottom.getWidth() - buttonGap) / 2;
    processButton.setBounds(buttonsBottom.removeFromLeft(bottomButtonWidth));
    buttonsBottom.removeFromLeft(buttonGap);
    startStopButton.setBounds(buttonsBottom);

    control.removeFromBottom(4);

    // Row above: Audio Settings and Plugins.
    auto buttonsTop = control.removeFromBottom(layout::controlButtonHeight);
    const auto topButtonWidth = (buttonsTop.getWidth() - buttonGap) / 2;
    settingsButton.setBounds(buttonsTop.removeFromLeft(topButtonWidth));
    buttonsTop.removeFromLeft(buttonGap);
    pluginButton.setBounds(buttonsTop);

    control.removeFromBottom(8);

    // The normalize and force overwrite toggles share a row above the buttons.
    auto toggleRow = control.removeFromBottom(layout::normalizeToggleHeight);
    normalizeBeforePluginToggle.setBounds(toggleRow.removeFromLeft((toggleRow.getWidth() - buttonGap) / 2));
    toggleRow.removeFromLeft(buttonGap);
    forceOverwriteToggle.setBounds(toggleRow);

    control.removeFromBottom(layout::sectionSpacing);

    // The normalization mode and target share the row above the toggles.
    normalizationTargetControl.setBounds(control.removeFromBottom(layout::normalizeToggleHeight));

    control.removeFromBottom(layout::sectionSpacing);

    zoomSlider.setBounds(control.removeFromBottom(zoomSlider.getTextBoxHeight()));

    control.removeFromBottom(layout::sectionSpacing);

    // Gain row: inline label, slider, then the clear button on the right.
    auto gainRow = control.removeFromBottom(gainSlider.getTextBoxHeight());
    gainClearButton.setBounds(gainRow.removeFromRight(layout::gainClearButtonWidth));
    gainRow.removeFromRight(4);
    const auto gainLabelWidth = 36;
    gainLabel.setBounds(gainRow.removeFromLeft(gainLabelWidth));
    gainSlider.setBounds(gainRow);

    control.removeFromBottom(8);

    audioInfoViewport.setBounds(control);
    const auto viewportWidth = audioInfoViewport.getMaximumVisibleWidth();
    const auto preferredHeight = audioInfo->getPreferredHeight();
    const auto viewportHeight = audioInfoViewport.getMaximumVisibleHeight();
    audioInfo->setSize(viewportWidth, juce::jmax(viewportHeight, preferredHeight));

    previewArea.removeFromLeft(layout::sectionSpacing);

    thumbnail->setBounds(previewArea);
}

void AudioBatchComponent::updateResultsTableColumnWidths() const
{
    auto& header = resultsTable.getHeader();

    const auto fixedColumnWidth = header.getColumnWidth(AudioFileTableModel::columnOverallPeak)
        + (header.isColumnVisible(AudioFileTableModel::columnType)
               ? header.getColumnWidth(AudioFileTableModel::columnType)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnBitrate)
               ? header.getColumnWidth(AudioFileTableModel::columnBitrate)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnSampleRate)
               ? header.getColumnWidth(AudioFileTableModel::columnSampleRate)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnPeakLeft)
               ? header.getColumnWidth(AudioFileTableModel::columnPeakLeft)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnPeakRight)
               ? header.getColumnWidth(AudioFileTableModel::columnPeakRight)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnOverallTruePeak)
               ? header.getColumnWidth(AudioFileTableModel::columnOverallTruePeak)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnMaxShortTermLufs)
               ? header.getColumnWidth(AudioFileTableModel::columnMaxShortTermLufs)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnIntegratedLufs)
               ? header.getColumnWidth(AudioFileTableModel::columnIntegratedLufs)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnCustomGain)
               ? header.getColumnWidth(AudioFileTableModel::columnCustomGain)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnStatus)
               ? header.getColumnWidth(AudioFileTableModel::columnStatus)
               : 0);

    const auto availableFlexibleWidth = resultsTable.getVisibleContentWidth() - fixedColumnWidth;
    const auto currentNameWidth = header.getColumnWidth(AudioFileTableModel::columnName);
    const auto currentPathWidth = header.getColumnWidth(AudioFileTableModel::columnPath);
    const auto currentFlexibleWidth = currentNameWidth + currentPathWidth;
    const auto defaultFlexibleWidth = nameColumnDefaultWidth + pathColumnDefaultWidth;
    const auto widthRatio = static_cast<double>(currentFlexibleWidth > 0 ? currentNameWidth : nameColumnDefaultWidth)
        / static_cast<double>(currentFlexibleWidth > 0 ? currentFlexibleWidth : defaultFlexibleWidth);

    int nameWidth = nameColumnMinimumWidth;
    int pathWidth = pathColumnMinimumWidth;

    if (availableFlexibleWidth >= nameColumnMinimumWidth + pathColumnMinimumWidth) {
        nameWidth = juce::roundToInt(static_cast<double>(availableFlexibleWidth) * widthRatio);
        nameWidth = juce::jlimit(nameColumnMinimumWidth, availableFlexibleWidth - pathColumnMinimumWidth, nameWidth);
        pathWidth = availableFlexibleWidth - nameWidth;
    }

    header.setColumnWidth(AudioFileTableModel::columnName, nameWidth);
    header.setColumnWidth(AudioFileTableModel::columnPath, pathWidth);
}

bool AudioBatchComponent::hasSelectedRecords() const
{
    return resultsTable.getNumSelectedRows() > 0;
}

bool AudioBatchComponent::hasAnyRecords() const
{
    return !analysisResults.empty();
}

void AudioBatchComponent::showAudioSettingsWindow()
{
    openDialogWindow(settingsWindow, &audioSetupComponent, "Audio Settings");
}

int AudioBatchComponent::findRecordIndex(const juce::String& fullPath) const
{
    for (std::size_t index = 0; index < analysisResults.size(); ++index) {
        if (analysisResults[index].fullPath == fullPath) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

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

void AudioBatchComponent::refreshTablePreservingSelection(const juce::StringArray& selectedPaths)
{
    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(selectedPaths);
}

void AudioBatchComponent::updateProcessButtonState()
{
    const bool hasChain = pluginChain != nullptr && pluginChain->getNumEnabledValidSlots() > 0;
    processButton.setEnabled(hasChain && hasAnyRecords() && !pluginProcessingInProgress && !isAnalysisInProgress());
}

void AudioBatchComponent::updateStatusLabel()
{
    if (pluginProcessingInProgress && processedResultsExpected > 0) {
        statusLabel.setText(
            utils::format("Processing {}/{}", processedResultsCompleted, processedResultsExpected),
            juce::dontSendNotification
        );
        return;
    }

    if (normalizeInProgress && normalizedResultsExpected > 0) {
        statusLabel.setText(
            utils::format("Normalizing {}/{}", normalizedResultsCompleted, normalizedResultsExpected),
            juce::dontSendNotification
        );
        return;
    }

    if (analysisRunning) {
        statusLabel.setText(
            analysisPlanReceived ? utils::format("Analyzing {}/{}", completedResults, expectedResults)
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

void AudioBatchComponent::sortResults()
{
    results_sorting::sortRecords(
        analysisResults,
        {
            .primaryColumnId = currentSortColumnId,
            .primaryForwards = currentSortForwards,
            .secondaryColumnId = secondarySortColumnId,
            .secondaryForwards = secondarySortForwards,
            // The activity labels change while files are busy, so only the settled text decides the order.
            .statusText = [this](const AudioAnalysisRecord& record) {
                const auto message = fileActivity.getMessage(record.fullPath);
                return message.text.isNotEmpty() ? message.text : AudioAnalysisService::formatStatus(record);
            },
        }
    );
}

void AudioBatchComponent::handleSortRequested(const int columnId, const bool isForwards)
{
    currentSortColumnId = columnId;
    currentSortForwards = isForwards;

    if (secondarySortColumnId == currentSortColumnId) {
        secondarySortColumnId = 0;
        secondarySortForwards = true;
    }

    refreshSortIndicators();

    const auto selectedPaths = getSelectedRecordPaths();

    refreshTablePreservingSelection(selectedPaths);
}

void AudioBatchComponent::handleSecondarySortRequested(const int columnId)
{
    if (columnId <= 0) {
        return;
    }

    if (columnId == currentSortColumnId) {
        secondarySortColumnId = 0;
        secondarySortForwards = true;
    } else if (secondarySortColumnId == columnId) {
        secondarySortForwards = !secondarySortForwards;
    } else {
        secondarySortColumnId = columnId;
        secondarySortForwards = true;
    }

    refreshSortIndicators();

    const auto selectedPaths = getSelectedRecordPaths();

    refreshTablePreservingSelection(selectedPaths);
}

void AudioBatchComponent::refreshSortIndicators()
{
    results_sorting::getSecondarySortHeader(resultsTable)
        .setSecondarySortIndicator(secondarySortColumnId, secondarySortForwards);
}

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

void AudioBatchComponent::handleSelectionChanged(const int lastRowSelected)
{
    const auto selectedRows = resultsTable.getSelectedRows();
    const auto row = getSelectionDisplayRow(selectedRows, lastRowSelected);

    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(row)];

    if (record.file.existsAsFile() && currentAudioFile != record.file) {
        currentAudioFile = record.file;
        showAudioResource(juce::URL(record.file));
        utils::logDebug("Loaded file: {}", record.fileName);
    }

    updateAudioInfo(record);
    updateGainControlsForSelection();
    updateThumbnailDisplayGain();
}

void AudioBatchComponent::openDialogWindow(
    SafePointer<juce::DialogWindow>& window,
    const SafePointer<juce::Component>& component,
    const juce::String& title
)
{
    // Create the window the first time it is opened.
    if (window == nullptr) {
        juce::DialogWindow::LaunchOptions launchOptions;
        launchOptions.dialogTitle = title;
        launchOptions.content.setNonOwned(component);
        launchOptions.content->setSize(layout::dialogContentWidth, layout::dialogContentHeight);
        window = launchOptions.create();
        childWindows.add(window);
    }
#if JUCE_WINDOWS
    window->setUsingNativeTitleBar(false);
    window->setTitleBarTextCentred(false);
    window->setTitleBarHeight(30);
#else
    window->setUsingNativeTitleBar(true);
#endif
    window->setSize(window->getWidth(), window->getHeight());
    window->setResizable(true, true);
    window->setResizeLimits(window->getWidth(), window->getHeight(), 4096, 4096);
    window->setColour(juce::DialogWindow::backgroundColourId, juce::CustomLookAndFeel::greySemiDark());
    window->setVisible(true);
    window->toFront(true);
}

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

        if (!PluginProcessingService::canProcessFile(record.file)) {
            continue;
        }

        selected.push_back(record);
    }

    return selected;
}
