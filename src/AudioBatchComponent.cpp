#include "AudioBatchComponent.h"

#include "AudioAnalysisService.h"
#include "CustomLookAndFeel.h"
#include "utils.h"

#include <algorithm>
#include <map>

using AudioInfoRow = std::pair<juce::String, juce::String>;

/// Compact two-column metadata panel displayed beside the waveform preview.
class AudioInfoPanel : public juce::Component
{
public:
    void clear()
    {
        setRows({});
    }

    void setRows(std::vector<AudioInfoRow> newRows)
    {
        rows = std::move(newRows);
        ensureLabelCount(rows.size());

        for (std::size_t index = 0; index < rows.size(); ++index) {
            fieldLabels[index]->setText(rows[index].first, juce::dontSendNotification);
            valueLabels[index]->setText(rows[index].second, juce::dontSendNotification);
            fieldLabels[index]->setVisible(true);
            valueLabels[index]->setVisible(true);
        }

        for (std::size_t index = rows.size(); index < fieldLabels.size(); ++index) {
            fieldLabels[index]->setVisible(false);
            valueLabels[index]->setVisible(false);
        }

        resized();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour(juce::CustomLookAndFeel::greySemiDark.withAlpha(0.85f));
        g.fillRoundedRectangle(bounds, 6.0f);

        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(10, 8);

        if (rows.empty()) {
            return;
        }

        const auto rowCount = static_cast<int>(rows.size());
        const auto rowHeight = juce::jmax(18, bounds.getHeight() / juce::jmax(1, rowCount));
        const auto fieldWidth = juce::roundToInt(bounds.getWidth() * 0.42f);
        const auto fieldArea = bounds.removeFromLeft(fieldWidth);

        for (int row = 0; row < rowCount; ++row) {
            const auto fieldRow = juce::Rectangle(
                fieldArea.getX(), fieldArea.getY() + row * rowHeight, fieldArea.getWidth(), rowHeight
            );
            const auto valueRow
                = juce::Rectangle(bounds.getX(), bounds.getY() + row * rowHeight, bounds.getWidth(), rowHeight);

            fieldLabels[static_cast<std::size_t>(row)]->setBounds(fieldRow);
            valueLabels[static_cast<std::size_t>(row)]->setBounds(valueRow);
        }
    }

private:
    void ensureLabelCount(const std::size_t requiredCount)
    {
        while (fieldLabels.size() < requiredCount) {
            auto fieldLabel = std::make_unique<juce::Label>();
            fieldLabel->setJustificationType(juce::Justification::centredLeft);
            fieldLabel->setColour(juce::Label::textColourId, juce::CustomLookAndFeel::greyMiddleLight);
            addAndMakeVisible(*fieldLabel);
            fieldLabels.push_back(std::move(fieldLabel));

            auto valueLabel = std::make_unique<juce::Label>();
            valueLabel->setJustificationType(juce::Justification::centredRight);
            valueLabel->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.95f));
            addAndMakeVisible(*valueLabel);
            valueLabels.push_back(std::move(valueLabel));
        }
    }

    std::vector<AudioInfoRow> rows;
    std::vector<std::unique_ptr<juce::Label>> fieldLabels;
    std::vector<std::unique_ptr<juce::Label>> valueLabels;
};

/// Main-view constants and helper functions local to this translation unit.
namespace audiobatch::gui
{
constexpr auto supportedAudioFilePatterns = "*.wav;*.aif;*.aiff;*.flac;*.mp3";
constexpr int activityIndicatorTimerHz = 24;
constexpr int nameColumnMinimumWidth = AudioFileTableModel::minimumColumnWidth(AudioFileTableModel::columnName);
constexpr int pathColumnMinimumWidth = AudioFileTableModel::minimumColumnWidth(AudioFileTableModel::columnPath);
constexpr int typeColumnDefaultWidth = AudioFileTableModel::initialColumnWidth(AudioFileTableModel::columnType);
constexpr int nameColumnDefaultWidth = AudioFileTableModel::initialColumnWidth(AudioFileTableModel::columnName);
constexpr int pathColumnDefaultWidth = AudioFileTableModel::initialColumnWidth(AudioFileTableModel::columnPath);

enum FileMenuItemId {
    revealFileMenuItemId = 1,
    openParentDirectoryMenuItemId,
    normalizeMenuItemId,
    normalizeSupportMenuItemId,
    moveToTrashMenuItemId,
};

juce::String buildNormalizationFailureMessage(const juce::StringArray& failures)
{
    if (failures.isEmpty()) {
        return {};
    }

    juce::String message;
    const auto linesToShow = juce::jmin(6, failures.size());

    for (int index = 0; index < linesToShow; ++index) {
        message << "- " << failures[index] << juce::newLine;
    }

    if (failures.size() > linesToShow) {
        message << "- ...and " << juce::String(failures.size() - linesToShow) << " more";
    }

    return message.trimEnd();
}

juce::String getRevealFileMenuLabel()
{
#if JUCE_MAC
    return "Reveal in Finder";
#elif JUCE_WINDOWS
    return "Reveal in Explorer";
#else
    return "Reveal in File Manager";
#endif
}

juce::String getRecordTypeLabel(const AudioAnalysisRecord& record)
{
    if (const auto extension = record.file.getFileExtension().trimCharactersAtStart("."); extension.isNotEmpty()) {
        return extension.toUpperCase();
    }

    auto formatName = record.formatName.trim();

    if (formatName.endsWithIgnoreCase(" file")) {
        formatName = formatName.dropLastCharacters(5).trimEnd();
    }

    if (formatName.isNotEmpty()) {
        return formatName;
    }

    return "Unknown";
}

juce::String formatLabelForFile(const juce::File& file)
{
    auto extension = file.getFileExtension().trimCharactersAtStart(".").toUpperCase();

    if (extension.isEmpty()) {
        extension = "Unknown";
    }

    return extension;
}

class SecondarySortTableHeader final : public juce::TableHeaderComponent
{
public:
    using SecondarySortCallback = std::function<void(int columnId)>;

    explicit SecondarySortTableHeader(SecondarySortCallback secondarySortRequestedCallback) :
        secondarySortRequested(std::move(secondarySortRequestedCallback))
    { }

    void columnClicked(const int columnId, const juce::ModifierKeys& mods) override
    {
        if ((mods.isCtrlDown() || mods.isCommandDown()) && secondarySortRequested != nullptr) {
            secondarySortRequested(columnId);
            return;
        }

        juce::TableHeaderComponent::columnClicked(columnId, mods);
    }

    void setSecondarySortIndicator(const int columnId, const bool isForwards)
    {
        if (baseColumnNames.empty()) {
            cacheBaseColumnNames();
        }

        secondarySortColumnId = columnId;
        secondarySortForwards = isForwards;

        for (const auto& [trackedColumnId, baseName] : baseColumnNames) {
            auto displayName = baseName;

            if (trackedColumnId == secondarySortColumnId) {
                displayName << (secondarySortForwards ? " [2^]" : " [2v]");
            }

            setColumnName(trackedColumnId, displayName);
        }

        repaint();
    }

private:
    void cacheBaseColumnNames()
    {
        const auto columnCount = getNumColumns(false);

        for (int index = 0; index < columnCount; ++index) {
            const auto columnId = getColumnIdOfIndex(index, false);
            baseColumnNames.emplace(columnId, getColumnName(columnId));
        }
    }

    SecondarySortCallback secondarySortRequested;
    std::map<int, juce::String> baseColumnNames;
    int secondarySortColumnId = 0;
    bool secondarySortForwards = true;
};

SecondarySortTableHeader& getSecondarySortHeader(juce::TableListBox& table)
{
    return static_cast<SecondarySortTableHeader&>(table.getHeader());
}

int compareNaturalStrings(const juce::String& left, const juce::String& right)
{
    return left.compareNatural(right);
}

template<typename Value>
int comparePeaks(Value left, Value right)
{
    const auto leftMagnitude = std::abs(left);
    const auto rightMagnitude = std::abs(right);

    if (juce::approximatelyEqual(leftMagnitude, rightMagnitude)) {
        return 0;
    }

    return leftMagnitude < rightMagnitude ? -1 : 1;
}

int compareBitrates(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    const auto leftBitrate = AudioAnalysisService::getAverageBitrateKbps(lhs);
    const auto rightBitrate = AudioAnalysisService::getAverageBitrateKbps(rhs);

    if (juce::approximatelyEqual(leftBitrate, rightBitrate)) {
        return 0;
    }

    return leftBitrate < rightBitrate ? -1 : 1;
}

int compareSampleRates(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    if (lhs.sampleRate == rhs.sampleRate) {
        return 0;
    }

    return lhs.sampleRate < rhs.sampleRate ? -1 : 1;
}

int compareLoudness(const double left, const double right)
{
    const auto leftIsNegativeInfinity = left <= AudioAnalysisRecord::negativeInfinityLoudness;
    const auto rightIsNegativeInfinity = right <= AudioAnalysisRecord::negativeInfinityLoudness;

    if (leftIsNegativeInfinity && rightIsNegativeInfinity) {
        return 0;
    }

    if (leftIsNegativeInfinity != rightIsNegativeInfinity) {
        return leftIsNegativeInfinity ? -1 : 1;
    }

    if (juce::approximatelyEqual(left, right)) {
        return 0;
    }

    return left < right ? -1 : 1;
}

int compareRecordsByColumn(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs, const int columnId)
{
    switch (columnId) {
        case AudioFileTableModel::columnName:
            return compareNaturalStrings(lhs.fileName, rhs.fileName);

        case AudioFileTableModel::columnPath:
            return compareNaturalStrings(lhs.fullPath, rhs.fullPath);

        case AudioFileTableModel::columnType:
            return compareNaturalStrings(getRecordTypeLabel(lhs), getRecordTypeLabel(rhs));

        case AudioFileTableModel::columnBitrate:
            return compareBitrates(lhs, rhs);

        case AudioFileTableModel::columnSampleRate:
            return compareSampleRates(lhs, rhs);

        case AudioFileTableModel::columnPeakLeft:
            return comparePeaks(lhs.peakLeft, rhs.peakLeft);

        case AudioFileTableModel::columnPeakRight:
            return comparePeaks(lhs.peakRight, rhs.peakRight);

        case AudioFileTableModel::columnOverallTruePeak:
            return comparePeaks(lhs.overallTruePeak, rhs.overallTruePeak);

        case AudioFileTableModel::columnMaxShortTermLufs:
            return compareLoudness(lhs.maxShortTermLufs, rhs.maxShortTermLufs);

        case AudioFileTableModel::columnIntegratedLufs:
            return compareLoudness(lhs.integratedLufs, rhs.integratedLufs);

        case AudioFileTableModel::columnStatus:
            return compareNaturalStrings(
                AudioAnalysisService::formatStatus(lhs), AudioAnalysisService::formatStatus(rhs)
            );

        case AudioFileTableModel::columnOverallPeak:
        default:
            return comparePeaks(lhs.overallPeak, rhs.overallPeak);
    }
}
}  // namespace audiobatch::gui

using namespace audiobatch::gui;

AudioBatchComponent::AudioBatchComponent() :
    analysisCoordinator(analysisCache),
    normalizeCoordinator(),
    fileTableModel(
        analysisResults,
        [this](const int row) { handleSelectionChanged(row); },
        [this](const int columnId, const bool isForwards) { handleSortRequested(columnId, isForwards); },
        [this](const int row, const int columnId, const juce::MouseEvent& event) {
            handleFileContextMenuRequested(row, columnId, event);
        },
        [this](const AudioAnalysisRecord& record) { return getActiveStatusLabel(record); },
        [this] { return getActivityPhase(); }
    ),
    audioSetupComp(audioDeviceManager, 0, 0, 0, 2, false, false, true, false)
{
    currentRoot = getInitialRootDirectory();

    formatManager.registerBasicFormats();

    currentRootLabel.setText(currentRoot.getFullPathName(), juce::dontSendNotification);
    currentRootLabel.setTooltip(currentRoot.getFullPathName());
    currentRootLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(currentRootLabel);

    statusLabel.setTooltip("Background analysis and normalization status.");
    statusLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(statusLabel);

    resultsTable.setHeader(std::make_unique<SecondarySortTableHeader>([this](const int columnId) {
        handleSecondarySortRequested(columnId);
    }));
    AudioFileTableModel::configureHeader(resultsTable.getHeader());
    resultsTable.setModel(&fileTableModel);
    resultsTable.setColour(juce::ListBox::backgroundColourId, juce::CustomLookAndFeel::greyMediumDark);
    resultsTable.setMultipleSelectionEnabled(true);
    resultsTable.setOutlineThickness(0);
    resultsTable.setRowHeight(24);
    resultsTable.getHeader().setSortColumnId(currentSortColumnId, currentSortForwards);
    refreshSortIndicators();
    addAndMakeVisible(resultsTable);

    // Default to a larger list view while allowing the lower preview area to be resized.
    mainVerticalLayout.setItemLayout(0, 240.0, -0.85, -0.69);
    mainVerticalLayout.setItemLayout(1, 6.0, 6.0, 6.0);
    mainVerticalLayout.setItemLayout(2, 180.0, -0.70, -0.31);
    addAndMakeVisible(waveformResizeBar);

    thumbnail = std::make_unique<ThumbnailComponent>(formatManager, transportSource);
    addAndMakeVisible(thumbnail.get());
    thumbnail->addChangeListener(this);
    thumbnail->setMouseWheelZoomCallback([this](const double zoomFactor) {
        zoomSlider.setValue(zoomSlider.getValue() * zoomFactor);
    });
    thumbnail->setThumbnailFullyLoadedCallback([this] { handleThumbnailFullyLoaded(); });

    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium);
    startStopButton.setColour(juce::TextButton::buttonOnColourId, juce::CustomLookAndFeel::green);
    startStopButton.setTooltip("Start or stop preview playback for the selected file.");
    startStopButton.onClick = [this] { startOrStop(); };
    addAndMakeVisible(startStopButton);

    zoomSlider.setRange(100, 1500, 1);
    zoomSlider.setNumDecimalPlacesToDisplay(0);
    zoomSlider.setSkewFactor(0.5);
    zoomSlider.setValue(100);
    zoomSlider.setTextValueSuffix("%");
    zoomSlider.setTooltip("Adjust waveform zoom. You can also use the mouse wheel over the waveform.");
    zoomSlider.onValueChange = [this] { zoomLevelChanged(zoomSlider.getValue() * 0.01); };
    addAndMakeVisible(zoomSlider);

    zoomLabel.setText("Zoom", juce::dontSendNotification);
    zoomLabel.attachToComponent(&zoomSlider, true);
    zoomSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, zoomSlider.getTextBoxHeight());
    addAndMakeVisible(zoomLabel);

    audioInfo = std::make_unique<AudioInfoPanel>();
    addAndMakeVisible(audioInfo.get());

    settingsButton.setTooltip("Open audio device settings.");
    settingsButton.onClick = [this] { showAudioSettingsWindow(); };
    addAndMakeVisible(settingsButton);

    tooltipWindow = std::make_unique<juce::TooltipWindow>(this, 700);

    thread.startThread(juce::Thread::Priority::high);

    analysisCache.open();

    const SafePointer safeThis(this);
    analysisCoordinator.setResultCallback([safeThis](const AudioAnalysisRecord& record) {
        juce::MessageManager::callAsync([safeThis, record] {
            if (safeThis != nullptr) {
                safeThis->handleAnalysisResult(record);
            }
        });
    });
    analysisCoordinator.setCompletionCallback([safeThis](int totalFiles) {
        juce::MessageManager::callAsync([safeThis, totalFiles] {
            if (safeThis != nullptr) {
                safeThis->handleAnalysisComplete(totalFiles);
            }
        });
    });

    normalizeCoordinator.setResultCallback([safeThis](const AudioNormalizationResult& result) {
        juce::MessageManager::callAsync([safeThis, result] {
            if (safeThis != nullptr) {
                safeThis->handleNormalizeResult(result);
            }
        });
    });

    normalizeCoordinator.setCompletionCallback([safeThis](int totalFiles) {
        juce::MessageManager::callAsync([safeThis, totalFiles] {
            if (safeThis != nullptr) {
                safeThis->handleNormalizeComplete(totalFiles);
            }
        });
    });

    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [this](const bool granted) {
        const int numInputChannels = granted ? 2 : 0;
        audioDeviceManager.initialise(numInputChannels, 2, nullptr, true, {}, nullptr);
    });

    audioDeviceManager.addAudioCallback(&audioSourcePlayer);
    audioSourcePlayer.setSource(&transportSource);

    setOpaque(true);
    setSize(1024, 800);
    setWantsKeyboardFocus(true);

    juce::MessageManager::callAsync([safeThis = SafePointer(this)] {
        if (safeThis != nullptr) {
            safeThis->refreshAnalysis(false);
        }
    });
}

AudioBatchComponent::~AudioBatchComponent()
{
    analysisCoordinator.cancelAndWait();
    normalizeCoordinator.cancelAndWait();

    stopTimer();
    transportSource.stop();
    transportSource.setSource(nullptr);
    audioSourcePlayer.setSource(nullptr);

    audioDeviceManager.removeAudioCallback(&audioSourcePlayer);

    thumbnail->removeChangeListener(this);
    thumbnail->setMouseWheelZoomCallback({});
    thumbnail->setThumbnailFullyLoadedCallback({});
    resultsTable.setModel(nullptr);

    if (childWindows.size() != 0) {
        for (auto& window : childWindows) {
            window.deleteAndZero();
        }
        childWindows.clear();
    }

    thread.stopThread(2000);
}

void AudioBatchComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

bool AudioBatchComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    return !files.isEmpty();
}

void AudioBatchComponent::filesDropped(const juce::StringArray& files, int, int)
{
    handleDroppedPaths(files);
}

void AudioBatchComponent::resized()
{
    auto r = getLocalBounds().reduced(4);
    auto topBar = r.removeFromTop(34);

    statusLabel.setBounds(topBar.removeFromRight(240));
    topBar.removeFromRight(6);
    currentRootLabel.setBounds(topBar);

    r.removeFromTop(6);

    juce::Component* verticalSections[] = {&resultsTable, &waveformResizeBar, thumbnail.get()};
    mainVerticalLayout.layOutComponents(
        verticalSections, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true
    );
    updateResultsTableColumnWidths();

    auto info = thumbnail->getBounds();

    const auto space = juce::jmin(250, juce::jmax(150, juce::roundToIntAccurate(r.getWidth() * 0.2)));
    auto control = info.removeFromLeft(space);
    auto buttons = control.removeFromBottom(50);

    settingsButton.setBounds(buttons.removeFromLeft(juce::roundToIntAccurate((space - 6) * 0.5)));
    buttons.removeFromLeft(6);
    startStopButton.setBounds(buttons);

    control.removeFromBottom(12);

    zoomSlider.setBounds(control.removeFromBottom(zoomSlider.getTextBoxHeight()));

    control.removeFromBottom(12);

    audioInfo->setBounds(control);

    info.removeFromLeft(6);

    thumbnail->setBounds(info);
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

bool AudioBatchComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::backspaceKey && key.getModifiers().isCtrlDown()) {
        moveSelectedRecordsToTrash(!key.getModifiers().isShiftDown());
        return true;
    }

    if (key == juce::KeyPress::spaceKey) {
        startOrStop();
        return true;
    }

    if (key == juce::KeyPress::F5Key) {
        refreshAnalysis(true);
        return true;
    }

    return false;
}

void AudioBatchComponent::chooseRootFolder()
{
    directoryChooser
        = std::make_unique<juce::FileChooser>("Choose Audio Folder", currentRoot, supportedAudioFilePatterns, true);

    directoryChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = SafePointer(this)](const juce::FileChooser& chooser) {
            if (safeThis == nullptr) {
                return;
            }

            if (const auto selectedFolder = chooser.getResult(); selectedFolder.isDirectory()) {
                safeThis->currentRoot = selectedFolder;
                safeThis->currentRootLabel.setText(selectedFolder.getFullPathName(), juce::dontSendNotification);
                safeThis->currentRootLabel.setTooltip(selectedFolder.getFullPathName());
                safeThis->refreshAnalysis(false);
            }

            safeThis->directoryChooser.reset();
        }
    );
}

void AudioBatchComponent::rescanCurrentRoot()
{
    refreshAnalysis(true);
}

void AudioBatchComponent::showAudioSettingsWindow()
{
    openDialogWindow(settingsWindow, &audioSetupComp, "Audio Settings");
}

void AudioBatchComponent::showSupportedNormalizationFormats()
{
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOk(
            juce::MessageBoxIconType::InfoIcon,
            "Normalization Format Support",
            AudioNormalizationService::getFormatSupportSummary(),
            "OK",
            this
        ),
        nullptr
    );
}

juce::File AudioBatchComponent::getInitialRootDirectory()
{
    const auto homeDirectory = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    const auto dropboxDirectory
        = homeDirectory.getChildFile("Dropbox").getChildFile("DJ MUSIC").getChildFile("FUNKY JAM 1");

    if (dropboxDirectory.isDirectory()) {
        return dropboxDirectory;
    }

    const auto musicDirectory = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    if (musicDirectory.isDirectory()) {
        return musicDirectory;
    }

    return homeDirectory;
}

void AudioBatchComponent::handleDroppedPaths(const juce::StringArray& paths)
{
    juce::Array<juce::File> droppedFiles;
    juce::File droppedDirectory;

    for (const auto& path : paths) {
        const juce::File droppedPath(path);

        if (droppedPath.isDirectory()) {
            if (droppedDirectory == juce::File()) {
                droppedDirectory = droppedPath;
            }

            continue;
        }

        if (droppedPath.existsAsFile()) {
            droppedFiles.addIfNotAlreadyThere(droppedPath);
        }
    }

    if (droppedFiles.isEmpty() && droppedDirectory.isDirectory()) {
        currentRoot = droppedDirectory;
        currentRootLabel.setText(currentRoot.getFullPathName(), juce::dontSendNotification);
        refreshAnalysis(false);
        return;
    }

    if (droppedFiles.isEmpty()) {
        statusLabel.setText("No supported dropped files", juce::dontSendNotification);
        return;
    }

    startAnalysis(droppedFiles, false, false, false);
}

void AudioBatchComponent::refreshAnalysis(const bool forceRefresh)
{
    juce::Array<juce::File> inputPaths;
    inputPaths.add(currentRoot);
    startAnalysis(inputPaths, true, forceRefresh, true);
}

void AudioBatchComponent::handleFileContextMenuRequested(const int row, int columnId, const juce::MouseEvent& event)
{
    juce::ignoreUnused(columnId);

    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    if (!resultsTable.isRowSelected(row)) {
        resultsTable.selectRow(row, true, true);
    }

    showFileContextMenu(row, event.getScreenPosition());
}

void AudioBatchComponent::showFileContextMenu(int row, const juce::Point<int> screenPosition)
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(row)];
    const auto parentDirectory = record.file.getParentDirectory();
    const auto selectedRecords = getSelectedNormalizableRecords();
    const auto canNormalize = !normalizeInProgress && !isAnalysisInProgress() && canNormalizeRecords(selectedRecords);

    juce::PopupMenu menu;
    menu.addItem(revealFileMenuItemId, getRevealFileMenuLabel(), record.file.exists());
    menu.addItem(openParentDirectoryMenuItemId, "Open Parent Folder", parentDirectory.isDirectory());
    menu.addSeparator();
    menu.addItem(normalizeMenuItemId, "Normalize to 0 dBFS", canNormalize);
    menu.addItem(normalizeSupportMenuItemId, "Normalization Format Support...");
    menu.addSeparator();
    menu.addItem(moveToTrashMenuItemId, "Move to Trash", record.file.exists());

    const auto safeThis = SafePointer(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetScreenArea(juce::Rectangle(screenPosition.x, screenPosition.y, 1, 1))
            .withParentComponent(this),
        [safeThis, row](const int result) {
            if (safeThis == nullptr) {
                return;
            }

            switch (result) {
                case revealFileMenuItemId:
                    safeThis->revealRecordInFileManager(row);
                    break;
                case openParentDirectoryMenuItemId:
                    safeThis->revealRecordParentDirectory(row);
                    break;
                case normalizeMenuItemId:
                    safeThis->normalizeSelectedRecords();
                    break;
                case normalizeSupportMenuItemId:
                    safeThis->showSupportedNormalizationFormats();
                    break;
                case moveToTrashMenuItemId:
                    safeThis->moveSelectedRecordsToTrash(true);
                    break;
                default:
                    break;
            }
        }
    );
}

void AudioBatchComponent::revealRecordInFileManager(const int row) const
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    if (const auto& record = analysisResults[static_cast<std::size_t>(row)]; record.file.exists()) {
        record.file.revealToUser();
    } else {
        record.file.getParentDirectory().revealToUser();
    }
}

void AudioBatchComponent::revealRecordParentDirectory(const int row) const
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto parentDirectory = analysisResults[static_cast<std::size_t>(row)].file.getParentDirectory();

    if (parentDirectory.isDirectory()) {
        parentDirectory.revealToUser();
    }
}

void AudioBatchComponent::moveSelectedRecordsToTrash(const bool promptForConfirmation)
{
    const auto selectedRows = resultsTable.getSelectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    juce::Array<juce::File> filesToTrash;
    juce::StringArray removedPaths;

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        const auto& record = analysisResults[static_cast<std::size_t>(rowNumber)];

        if (!record.file.exists()) {
            continue;
        }

        filesToTrash.add(record.file);
        removedPaths.addIfNotAlreadyThere(record.fullPath);
    }

    if (filesToTrash.isEmpty()) {
        return;
    }

    const auto fileCount = filesToTrash.size();
    const auto message = fileCount == 1
        ? "Move\n\n" + filesToTrash.getFirst().getFullPathName() + "\n\nto the system trash?"
        : "Move " + juce::String(fileCount) + " selected files to the system trash?";

    if (!promptForConfirmation) {
        runMoveToTrash(filesToTrash, removedPaths, selectedRows[0]);
        return;
    }

    const auto safeThis = SafePointer(this);
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::WarningIcon, "Move to Trash", message, "Move to Trash", "Cancel", this
        ),
        [safeThis, filesToTrash, removedPaths, fallbackRow = selectedRows[0]](const int result) {
            if (safeThis == nullptr || result == 0) {
                return;
            }
            safeThis->runMoveToTrash(filesToTrash, removedPaths, fallbackRow);
        }
    );
}

void AudioBatchComponent::runMoveToTrash(
    const juce::Array<juce::File>& filesToTrash,
    const juce::StringArray& removedPaths,
    const int fallbackRow
)
{
    juce::StringArray failedPaths;
    juce::StringArray successfullyRemovedPaths;

    if (currentAudioFile.exists()) {
        for (const auto& file : filesToTrash) {
            if (file == currentAudioFile) {
                clearCurrentAudioPreview();
                break;
            }
        }
    }

    for (int index = 0; index < filesToTrash.size(); ++index) {
        const auto& file = filesToTrash.getReference(index);

        if (!utils::move_to_trash(file)) {
            failedPaths.add(file.getFullPathName());
            continue;
        }

        if (juce::isPositiveAndBelow(index, removedPaths.size())) {
            successfullyRemovedPaths.addIfNotAlreadyThere(removedPaths[index]);
        }
    }

    removeRecordsByPath(successfullyRemovedPaths, fallbackRow);

    if (!failedPaths.isEmpty()) {
        for (const auto& failedPath : failedPaths) {
            utils::log_error("Move to trash failed for " + failedPath);
        }

        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Move to Trash Failed",
                failedPaths.size() == 1
                    ? "Could not move this file to the system trash:\n\n" + failedPaths[0]
                    : "Could not move " + juce::String(failedPaths.size()) + " files to the system trash.",
                "OK",
                this
            ),
            nullptr
        );
    }
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

bool AudioBatchComponent::canNormalizeRecords(const std::vector<AudioAnalysisRecord>& records)
{
    if (records.empty()) {
        return false;
    }

    return std::all_of(records.begin(), records.end(), [](const auto& record) {
        return AudioNormalizationService::canNormalizeFile(record.file);
    });
}

juce::String AudioBatchComponent::buildNormalizationUnavailableMessage(
    const std::vector<AudioAnalysisRecord>& records
) const
{
    juce::StringArray unsupportedLines;

    for (const auto& record : records) {
        const auto reason = AudioNormalizationService::getNormalizationSupportMessage(record.file);

        if (reason.isEmpty()) {
            continue;
        }

        unsupportedLines.add(record.fileName + " (" + formatLabelForFile(record.file) + "): " + reason);
    }

    if (unsupportedLines.isEmpty()) {
        return {};
    }

    return "The selected files cannot be normalized with the current build:\n\n"
        + unsupportedLines.joinIntoString(juce::newLine) + "\n\n"
        + AudioNormalizationService::getFormatSupportSummary();
}

void AudioBatchComponent::markFilesProcessing(const juce::Array<juce::File>& files, const juce::String& activityLabel)
{
    for (const auto& file : files) {
        activeFileStatusLabels[file.getFullPathName()] = activityLabel;
    }

    syncActivityTimer();
    resultsTable.repaint();
}

void AudioBatchComponent::unmarkFileProcessing(const juce::String& fullPath)
{
    activeFileStatusLabels.erase(fullPath);
    syncActivityTimer();
}

void AudioBatchComponent::reconcilePendingAnalysisResults()
{
    const auto selectedPaths = getSelectedRecordPaths();
    bool resultsChanged = false;

    for (auto& record : analysisResults) {
        const auto activeStatus = getActiveStatusLabel(record);
        const auto needsRefresh = activeStatus == "Analyzing" || record.status == AudioAnalysisStatus::pending;

        if (!needsRefresh) {
            continue;
        }

        AudioAnalysisRecord refreshedRecord;

        if (analysisCache.getAnalysis(record.file, refreshedRecord)) {
            record = std::move(refreshedRecord);
            resultsChanged = true;
            continue;
        }

        if (!record.file.existsAsFile()) {
            record.status = AudioAnalysisStatus::failed;
            record.errorMessage = "File does not exist";
            utils::log_error("Analysis failed for " + record.fullPath + ": " + record.errorMessage);
            resultsChanged = true;
            continue;
        }

        auto analyzedRecord = AudioAnalysisService::analyzeFile(record.file);
        analysisCache.storeAnalysis(analyzedRecord);
        record = std::move(analyzedRecord);
        resultsChanged = true;
    }

    for (auto iterator = activeFileStatusLabels.begin(); iterator != activeFileStatusLabels.end();) {
        if (iterator->second == "Analyzing") {
            iterator = activeFileStatusLabels.erase(iterator);
        } else {
            ++iterator;
        }
    }

    completedResults = expectedResults;
    syncActivityTimer();

    if (resultsChanged) {
        sortResults();
        resultsTable.updateContent();
        updateResultsTableColumnWidths();
        restoreSelectionByPaths(selectedPaths);

        if (currentAudioFile.existsAsFile()) {
            if (const auto currentIndex = findRecordIndex(currentAudioFile.getFullPathName()); currentIndex >= 0) {
                updateAudioInfo(analysisResults[static_cast<std::size_t>(currentIndex)]);
            }
        }
    }

    resultsTable.repaint();
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

void AudioBatchComponent::removeRecordsByPath(const juce::StringArray& removedPaths, const int fallbackRow)
{
    if (removedPaths.isEmpty()) {
        return;
    }

    auto remainingSelectedPaths = getSelectedRecordPaths();
    for (int index = remainingSelectedPaths.size(); --index >= 0;) {
        if (removedPaths.contains(remainingSelectedPaths[index])) {
            remainingSelectedPaths.remove(index);
        }
    }

    const auto currentFileRemoved
        = currentAudioFile.exists() && removedPaths.contains(currentAudioFile.getFullPathName());

    analysisResults.erase(
        std::remove_if(
            analysisResults.begin(),
            analysisResults.end(),
            [&removedPaths](const AudioAnalysisRecord& record) { return removedPaths.contains(record.fullPath); }
        ),
        analysisResults.end()
    );

    expectedResults = juce::jmax(0, expectedResults - removedPaths.size());
    completedResults = juce::jmin(completedResults, expectedResults);

    if (currentFileRemoved) {
        clearCurrentAudioPreview();
    }

    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(remainingSelectedPaths);

    if (resultsTable.getNumSelectedRows() == 0 && !analysisResults.empty()) {
        resultsTable.selectRow(juce::jlimit(0, static_cast<int>(analysisResults.size()) - 1, fallbackRow));
    }

    if (analysisResults.empty()) {
        audioInfo->clear();
    }

    updateStatusLabel();
    resultsTable.repaint();
}

void AudioBatchComponent::handleAnalysisResult(const AudioAnalysisRecord& record)
{
    ++completedResults;
    unmarkFileProcessing(record.fullPath);

    if (!record.file.exists()) {
        updateStatusLabel();
        return;
    }

    const auto selectedPaths = getSelectedRecordPaths();

    if (const auto existingIndex = findRecordIndex(record.fullPath); existingIndex >= 0) {
        analysisResults[static_cast<std::size_t>(existingIndex)] = record;
    } else {
        analysisResults.push_back(record);
    }

    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(selectedPaths);

    if (currentAudioFile == record.file) {
        updateAudioInfo(record);
    }

    updateStatusLabel();
}

void AudioBatchComponent::handleNormalizeResult(const AudioNormalizationResult& result)
{
    ++normalizedResultsCompleted;
    unmarkFileProcessing(result.fullPath);

    if (result.succeeded) {
        auto selectedPaths = getSelectedRecordPaths();

        for (int index = 0; index < selectedPaths.size(); ++index) {
            if (selectedPaths[index] == result.fullPath) {
                selectedPaths.set(index, result.analysisRecord.fullPath);
            }
        }

        analysisResults.erase(
            std::remove_if(
                analysisResults.begin(),
                analysisResults.end(),
                [&result](const AudioAnalysisRecord& record) {
                    return record.fullPath == result.fullPath && record.fullPath != result.analysisRecord.fullPath;
                }
            ),
            analysisResults.end()
        );

        if (const auto existingIndex = findRecordIndex(result.analysisRecord.fullPath); existingIndex >= 0) {
            analysisResults[static_cast<std::size_t>(existingIndex)] = result.analysisRecord;
        } else {
            analysisResults.push_back(result.analysisRecord);
        }

        analysisCache.storeAnalysis(result.analysisRecord);
        sortResults();
        resultsTable.updateContent();
        updateResultsTableColumnWidths();
        restoreSelectionByPaths(selectedPaths);

        if (currentAudioFile == result.file) {
            currentAudioFile = result.analysisRecord.file;
            showAudioResource(juce::URL(currentAudioFile));
            updateAudioInfo(result.analysisRecord);
        } else if (currentAudioFile == result.analysisRecord.file) {
            updateAudioInfo(result.analysisRecord);
        }
    } else {
        normalizationFailures.add(result.fileName + ": " + result.errorMessage);
    }

    updateStatusLabel();
}

void AudioBatchComponent::handleNormalizeComplete(const int totalFiles)
{
    normalizedResultsExpected = totalFiles;
    normalizeInProgress = false;
    syncActivityTimer();

    if (!normalizationFailures.isEmpty()) {
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Normalize",
                normalizationFailures.size() == 1 ? normalizationFailures[0]
                                                  : "Some files could not be normalized:\n\n"
                        + buildNormalizationFailureMessage(normalizationFailures),
                "OK",
                this
            ),
            nullptr
        );
    }

    if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateStatusLabel();
}

void AudioBatchComponent::handleAnalysisComplete(const int totalFiles)
{
    expectedResults = totalFiles;
    reconcilePendingAnalysisResults();
    updateStatusLabel();

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - analysisStartedAtMs;
    const auto cachedFileCount = juce::jmax(0, totalFiles - analyzedFilesThisRun);
    const auto failedFileCount = std::count_if(analysisResults.begin(), analysisResults.end(), [](const auto& record) {
        return record.hasError();
    });
    utils::log_info(
        "Analysis complete: loaded " + juce::String(totalFiles) + " files (" + juce::String(analyzedFilesThisRun)
        + " analyzed, " + juce::String(cachedFileCount) + " from cache, " + juce::String(failedFileCount)
        + " failed) in " + juce::String(elapsedMs / 1000.0, 2) + " s"
    );

    if (resultsTable.getSelectedRow() < 0 && !analysisResults.empty()) {
        resultsTable.selectRow(0);
    } else if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }
}

void AudioBatchComponent::updateStatusLabel()
{
    if (normalizeInProgress && normalizedResultsExpected > 0) {
        statusLabel.setText(
            "Normalizing " + juce::String(normalizedResultsCompleted) + "/" + juce::String(normalizedResultsExpected),
            juce::dontSendNotification
        );
        return;
    }

    if (expectedResults <= 0) {
        statusLabel.setText("No supported audio files found", juce::dontSendNotification);
        return;
    }

    if (completedResults < expectedResults) {
        statusLabel.setText(
            "Analyzing " + juce::String(completedResults) + "/" + juce::String(expectedResults),
            juce::dontSendNotification
        );
        return;
    }

    statusLabel.setText("Loaded " + juce::String(analysisResults.size()) + " files", juce::dontSendNotification);
}

void AudioBatchComponent::normalizeSelectedRecords()
{
    if (normalizeInProgress) {
        utils::log_error("Normalization requested while another normalization pass is already in progress");
        statusLabel.setText("Normalization already in progress", juce::dontSendNotification);
        return;
    }

    if (isAnalysisInProgress()) {
        utils::log_error("Normalization requested before analysis finished");
        statusLabel.setText("Wait for analysis to finish before normalizing", juce::dontSendNotification);
        return;
    }

    const auto recordsToNormalize = getSelectedNormalizableRecords();

    if (recordsToNormalize.empty()) {
        return;
    }

    if (!canNormalizeRecords(recordsToNormalize)) {
        utils::log_error("Normalization unavailable for the selected files");
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Normalization Unavailable",
                buildNormalizationUnavailableMessage(recordsToNormalize),
                "OK",
                this
            ),
            nullptr
        );
        statusLabel.setText("Selected file type cannot be normalized in this build", juce::dontSendNotification);
        return;
    }

    juce::Array<juce::File> filesToNormalize;
    for (const auto& record : recordsToNormalize) {
        filesToNormalize.add(record.file);

        if (record.file == currentAudioFile) {
            clearCurrentAudioPreview();
        }
    }

    normalizationFailures.clear();
    normalizedResultsCompleted = 0;
    normalizeInProgress = true;
    normalizedResultsExpected = normalizeCoordinator.start(recordsToNormalize);

    if (normalizedResultsExpected <= 0) {
        normalizeInProgress = false;
        updateStatusLabel();
        return;
    }

    markFilesProcessing(filesToNormalize, "Normalizing");

    updateStatusLabel();
}

void AudioBatchComponent::sortResults()
{
    std::sort(analysisResults.begin(), analysisResults.end(), [this](const auto& lhs, const auto& rhs) {
        if (lhs.hasError() != rhs.hasError()) {
            return !lhs.hasError();
        }

        const auto compareUsingSort = [&](int columnId, bool isForwards) {
            if (columnId <= 0) {
                return 0;
            }

            const auto comparison = compareRecordsByColumn(lhs, rhs, columnId);
            if (comparison == 0) {
                return 0;
            }

            return isForwards ? comparison : -comparison;
        };

        if (const auto primaryComparison = compareUsingSort(currentSortColumnId, currentSortForwards);
            primaryComparison != 0)
        {
            return primaryComparison < 0;
        }

        if (const auto secondaryComparison = compareUsingSort(secondarySortColumnId, secondarySortForwards);
            secondaryComparison != 0)
        {
            return secondaryComparison < 0;
        }

        if (const auto nameComparison = compareNaturalStrings(lhs.fileName, rhs.fileName); nameComparison != 0) {
            return nameComparison < 0;
        }

        return compareNaturalStrings(lhs.fullPath, rhs.fullPath) < 0;
    });
}

void AudioBatchComponent::startAnalysis(
    const juce::Array<juce::File>& inputPaths,
    const bool recursive,
    const bool forceRefresh,
    const bool clearResults
)
{
    if (normalizeInProgress) {
        statusLabel.setText("Normalization in progress", juce::dontSendNotification);
        return;
    }

    if (clearResults) {
        analysisResults.clear();
        activeFileStatusLabels.clear();
        clearCurrentAudioPreview();
        audioInfo->clear();
        resultsTable.updateContent();
        updateResultsTableColumnWidths();
        resultsTable.repaint();
    }

    AudioAnalysisOptions options;
    options.inputPaths = inputPaths;
    options.recursive = recursive;
    options.refresh = forceRefresh;

    analysisStartedAtMs = juce::Time::getMillisecondCounterHiRes();
    completedResults = 0;
    const auto files = AudioAnalysisService::collectInputFiles(options.inputPaths, options.recursive);
    expectedResults = files.size();

    if (expectedResults == 0) {
        statusLabel.setText(
            clearResults ? "No supported audio files found" : "No supported dropped files", juce::dontSendNotification
        );
        return;
    }

    juce::Array<juce::File> staleFiles;

    for (const auto& file : files) {
        AudioAnalysisRecord cachedRecord;

        if (!forceRefresh && analysisCache.getAnalysis(file, cachedRecord)) {
            continue;
        }

        staleFiles.add(file);

        if (findRecordIndex(file.getFullPathName()) >= 0) {
            continue;
        }

        auto placeholder = AudioAnalysisRecord::fromFile(file);
        placeholder.status = AudioAnalysisStatus::pending;
        analysisResults.push_back(std::move(placeholder));
    }

    analyzedFilesThisRun = staleFiles.size();

    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    markFilesProcessing(staleFiles, "Analyzing");

    statusLabel.setText("Analyzing 0/" + juce::String(expectedResults), juce::dontSendNotification);

    if (clearResults) {
        currentRootLabel.setText(currentRoot.getFullPathName(), juce::dontSendNotification);
        currentRootLabel.setTooltip(currentRoot.getFullPathName());
    }

    analysisCoordinator.start(options, files);
}

juce::String AudioBatchComponent::getActiveStatusLabel(const AudioAnalysisRecord& record) const
{
    if (const auto iterator = activeFileStatusLabels.find(record.fullPath); iterator != activeFileStatusLabels.end()) {
        return iterator->second;
    }

    return {};
}

float AudioBatchComponent::getActivityPhase() const
{
    constexpr auto rotationsPerSecond = 0.85;
    const auto phase = std::fmod(juce::Time::getMillisecondCounterHiRes() * 0.001 * rotationsPerSecond, 1.0);
    return static_cast<float>(phase);
}

bool AudioBatchComponent::isAnalysisInProgress() const
{
    return completedResults < expectedResults;
}

bool AudioBatchComponent::isAnyFileProcessing() const
{
    return !activeFileStatusLabels.empty();
}

void AudioBatchComponent::syncActivityTimer()
{
    if (isAnyFileProcessing()) {
        startTimerHz(activityIndicatorTimerHz);
    } else {
        stopTimer();
    }
}

void AudioBatchComponent::timerCallback()
{
    if (!isAnyFileProcessing()) {
        stopTimer();
        return;
    }

    resultsTable.repaint();
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

    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(selectedPaths);
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

    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(selectedPaths);
}

void AudioBatchComponent::refreshSortIndicators()
{
    getSecondarySortHeader(resultsTable).setSecondarySortIndicator(secondarySortColumnId, secondarySortForwards);
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
        utils::log_debug("Loaded file: " + record.fileName);
    }

    updateAudioInfo(record);
}

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

void AudioBatchComponent::updateAudioInfo(const AudioAnalysisRecord& record)
{
    std::vector<AudioInfoRow> rows;

    if (!record.isReady() && !record.hasError()) {
        rows.emplace_back("File", record.fileName);
        rows.emplace_back(
            "Status", getActiveStatusLabel(record).isNotEmpty() ? getActiveStatusLabel(record) : "Pending"
        );
        audioInfo->setRows(std::move(rows));
        return;
    }

    if (record.hasError()) {
        rows.emplace_back("File", record.fileName);
        rows.emplace_back("Error", record.errorMessage);
        audioInfo->setRows(std::move(rows));
        return;
    }

    rows.emplace_back("Format", getRecordTypeLabel(record));
    rows.emplace_back("Channels", juce::String(record.channels));
    rows.emplace_back("Bits per sample", AudioAnalysisService::formatBitsPerSampleDisplay(record));
    rows.emplace_back("Sample rate", juce::String(record.sampleRate));
    rows.emplace_back("Bitrate", AudioAnalysisService::formatBitrateDisplay(record));
    rows.emplace_back("Duration", juce::RelativeTime(record.durationSeconds).getDescription());
    rows.emplace_back("Samples", juce::String(record.lengthInSamples));
    rows.emplace_back("Peak Max", AudioAnalysisService::formatPeakDisplay(record.overallPeak));
    rows.emplace_back("Peak L", AudioAnalysisService::formatPeakDisplay(record.peakLeft));
    rows.emplace_back("Peak R", AudioAnalysisService::formatPeakDisplay(record.peakRight));
    rows.emplace_back("True Peak Max", AudioAnalysisService::formatTruePeakDisplay(record.overallTruePeak));
    rows.emplace_back("Max Short-term", AudioAnalysisService::formatLoudnessDisplay(record.maxShortTermLufs));
    rows.emplace_back("Integrated Loudness", AudioAnalysisService::formatLoudnessDisplay(record.integratedLufs));
    rows.emplace_back("Status", AudioAnalysisService::formatStatus(record));

    audioInfo->setRows(std::move(rows));
}

void AudioBatchComponent::showAudioResource(juce::URL resource)
{
    if (loadURLIntoTransport(resource)) {
        currentAudioUrl = std::move(resource);
        currentWaveformLoadedFromCache = false;
        startStopButton.setEnabled(true);
        startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::green);
        zoomSlider.setValue(100.0, juce::dontSendNotification);

        juce::MemoryBlock waveformData;
        if (currentAudioFile.existsAsFile() && analysisCache.getWaveformData(currentAudioFile, waveformData)
            && thumbnail->loadFromCacheData(waveformData))
        {
            currentWaveformLoadedFromCache = true;
        } else {
            thumbnail->setURL(currentAudioUrl);
        }
    } else {
        currentAudioUrl = {};
        currentWaveformLoadedFromCache = false;
        startStopButton.setEnabled(false);
        startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium);
        thumbnail->setURL(juce::URL());
    }
}

void AudioBatchComponent::handleThumbnailFullyLoaded()
{
    if (currentWaveformLoadedFromCache || !currentAudioFile.existsAsFile()) {
        return;
    }

    const auto waveformData = thumbnail->saveToCacheData();
    if (waveformData.getSize() == 0) {
        return;
    }

    currentWaveformLoadedFromCache = analysisCache.storeWaveformData(currentAudioFile, waveformData);
}

bool AudioBatchComponent::loadURLIntoTransport(const juce::URL& audioURL)
{
    // unload the previous file source and delete it..
    transportSource.stop();
    transportSource.setSource(nullptr);
    currentAudioFileSource.reset();

    juce::AudioFormatReader* reader = nullptr;

    if (reader == nullptr) {
        reader = formatManager.createReaderFor(
            audioURL.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress))
        );
    }
    if (reader != nullptr) {
        currentAudioFileSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
        transportSource.setSource(currentAudioFileSource.get(), 32768, &thread, reader->sampleRate);

        return true;
    }

    return false;
}

void AudioBatchComponent::startOrStop()
{
    if (transportSource.isPlaying()) {
        transportSource.stop();
        startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::green);
    } else {
        transportSource.start();
        startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::red);
    }
}

void AudioBatchComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == thumbnail.get()) {
        const auto droppedFiles = thumbnail->getLastDroppedFiles();

        if (!droppedFiles.isEmpty()) {
            handleDroppedPaths(droppedFiles);
            return;
        }
    }
}

void AudioBatchComponent::clearCurrentAudioPreview()
{
    currentAudioFile = {};
    currentAudioUrl = {};
    currentWaveformLoadedFromCache = false;
    transportSource.stop();
    transportSource.setSource(nullptr);
    currentAudioFileSource.reset();
    thumbnail->setURL(juce::URL());
    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium);
}

void AudioBatchComponent::mouseMagnify(const juce::MouseEvent&, const float scaleFactor)
{
    const auto newZoom = zoomSlider.getValue() * (scaleFactor * scaleFactor);
    zoomSlider.setValue(newZoom);
}

void AudioBatchComponent::zoomLevelChanged(const double zoomLevel)
{
    thumbnail.get()->setZoom(zoomLevel);
}

void AudioBatchComponent::openDialogWindow(
    SafePointer<juce::DialogWindow>& window,
    const SafePointer<juce::Component>& component,
    const juce::String& title
)
{
    // create window the first time it is opened
    if (!window) {
        juce::DialogWindow::LaunchOptions launchOptions;
        launchOptions.dialogTitle = title;
        launchOptions.content.setNonOwned(component);
        launchOptions.content->setSize(500, 150);
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
    window->setColour(juce::DialogWindow::backgroundColourId, juce::CustomLookAndFeel::greySemiDark);
    window->setVisible(true);
    window->toFront(true);
}
