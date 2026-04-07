#include "AudioBatchComponent.h"

#include "AudioAnalysisService.h"
#include "CustomLookAndFeel.h"
#include "utils.h"

#include <algorithm>

using AudioInfoRow = std::pair<juce::String, juce::String>;

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
        auto bounds = getLocalBounds().toFloat();
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
            auto fieldRow = juce::Rectangle<int>(
                fieldArea.getX(), fieldArea.getY() + row * rowHeight, fieldArea.getWidth(), rowHeight
            );
            auto valueRow
                = juce::Rectangle<int>(bounds.getX(), bounds.getY() + row * rowHeight, bounds.getWidth(), rowHeight);

            fieldLabels[static_cast<std::size_t>(row)]->setBounds(fieldRow);
            valueLabels[static_cast<std::size_t>(row)]->setBounds(valueRow);
        }
    }

private:
    void ensureLabelCount(std::size_t requiredCount)
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

namespace
{
constexpr auto supportedAudioFilePatterns = "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3";
constexpr int nameColumnMinimumWidth = 120;
constexpr int pathColumnMinimumWidth = 160;
constexpr int nameColumnDefaultWidth = 220;
constexpr int pathColumnDefaultWidth = 360;

template<typename Value>
bool compareWithDirection(Value lhs, Value rhs, bool forwards)
{
    return forwards ? lhs < rhs : lhs > rhs;
}
}  // namespace

AudioBatchComponent::AudioBatchComponent() :
    analysisCoordinator(analysisCache),
    fileTableModel(
        analysisResults,
        [this](int row) { handleSelectionChanged(row); },
        [this](int columnId, bool isForwards) { handleSortRequested(columnId, isForwards); }
    ),
    audioSetupComp(audioDeviceManager, 0, 0, 0, 2, false, false, true, false)
{
    currentRoot = getInitialRootDirectory();

    formatManager.registerBasicFormats();

    chooseFolderButton.onClick = [this] { browseForRootFolder(); };
    addAndMakeVisible(chooseFolderButton);

    rescanButton.onClick = [this] { refreshAnalysis(true); };
    addAndMakeVisible(rescanButton);

    currentRootLabel.setText(currentRoot.getFullPathName(), juce::dontSendNotification);
    currentRootLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(currentRootLabel);

    statusLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(statusLabel);

    AudioFileTableModel::configureHeader(resultsTable.getHeader());
    resultsTable.setModel(&fileTableModel);
    resultsTable.setColour(juce::ListBox::backgroundColourId, juce::CustomLookAndFeel::greyMediumDark);
    resultsTable.setMultipleSelectionEnabled(true);
    resultsTable.setOutlineThickness(0);
    resultsTable.setRowHeight(24);
    resultsTable.getHeader().setSortColumnId(currentSortColumnId, currentSortForwards);
    addAndMakeVisible(resultsTable);

    // Default to a larger list view while allowing the lower preview area to be resized.
    mainVerticalLayout.setItemLayout(0, 240.0, -0.85, -0.62);
    mainVerticalLayout.setItemLayout(1, 6.0, 6.0, 6.0);
    mainVerticalLayout.setItemLayout(2, 180.0, -0.70, -0.38);
    addAndMakeVisible(waveformResizeBar);

    thumbnail = std::make_unique<ThumbnailComponent>(formatManager, transportSource);
    addAndMakeVisible(thumbnail.get());
    thumbnail->addChangeListener(this);
    thumbnail->setThumbnailFullyLoadedCallback([this] { handleThumbnailFullyLoaded(); });

    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium);
    startStopButton.setColour(juce::TextButton::buttonOnColourId, juce::CustomLookAndFeel::green);
    startStopButton.onClick = [this] { startOrStop(); };
    addAndMakeVisible(startStopButton);

    zoomSlider.setRange(100, 1500, 1);
    zoomSlider.setNumDecimalPlacesToDisplay(0);
    zoomSlider.setSkewFactor(0.5);
    zoomSlider.setValue(100);
    zoomSlider.setTextValueSuffix("%");
    zoomSlider.onValueChange = [this] { zoomLevelChanged(zoomSlider.getValue() * 0.01); };
    addAndMakeVisible(zoomSlider);

    zoomLabel.setText("Zoom", juce::dontSendNotification);
    zoomLabel.attachToComponent(&zoomSlider, true);
    zoomSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, zoomSlider.getTextBoxHeight());
    addAndMakeVisible(zoomLabel);

    audioInfo = std::make_unique<AudioInfoPanel>();
    addAndMakeVisible(audioInfo.get());

    settingsButton.onClick = [this] { openDialogWindow(settingsWindow, &audioSetupComp, "Audio Settings"); };
    addAndMakeVisible(settingsButton);

    thread.startThread(juce::Thread::Priority::high);

    analysisCache.open();

    const SafePointer<AudioBatchComponent> safeThis(this);
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

    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [this](bool granted) {
        int numInputChannels = granted ? 2 : 0;
        audioDeviceManager.initialise(numInputChannels, 2, nullptr, true, {}, nullptr);
    });

    audioDeviceManager.addAudioCallback(&audioSourcePlayer);
    audioSourcePlayer.setSource(&transportSource);

    setOpaque(true);
    setSize(1024, 800);
    setWantsKeyboardFocus(true);

    refreshAnalysis(false);
}

AudioBatchComponent::~AudioBatchComponent()
{
    analysisCoordinator.cancelAndWait();

    transportSource.setSource(nullptr);
    audioSourcePlayer.setSource(nullptr);

    audioDeviceManager.removeAudioCallback(&audioSourcePlayer);

    thumbnail->removeChangeListener(this);
    thumbnail->setThumbnailFullyLoadedCallback({});
    resultsTable.setModel(nullptr);

    if (childWindows.size() != 0) {
        for (auto& window : childWindows) {
            window.deleteAndZero();
        }
        childWindows.clear();
    }
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

    chooseFolderButton.setBounds(topBar.removeFromLeft(140));
    topBar.removeFromLeft(6);
    rescanButton.setBounds(topBar.removeFromLeft(90));
    topBar.removeFromLeft(10);
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

    auto space = juce::jmin(250, juce::jmax(150, juce::roundToIntAccurate(r.getWidth() * 0.2)));
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

void AudioBatchComponent::updateResultsTableColumnWidths()
{
    auto& header = resultsTable.getHeader();

    const auto fixedColumnWidth = header.getColumnWidth(AudioFileTableModel::columnOverallPeak)
        + header.getColumnWidth(AudioFileTableModel::columnPeakLeft)
        + header.getColumnWidth(AudioFileTableModel::columnPeakRight)
        + header.getColumnWidth(AudioFileTableModel::columnStatus);

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
    if (key == juce::KeyPress::spaceKey) {
        startOrStop();
    } else if (key == juce::KeyPress::F5Key) {
        refreshAnalysis(true);
    }

    return false;
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

void AudioBatchComponent::browseForRootFolder()
{
    directoryChooser
        = std::make_unique<juce::FileChooser>("Choose Audio Folder", currentRoot, supportedAudioFilePatterns, true);

    directoryChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = SafePointer<AudioBatchComponent>(this)](const juce::FileChooser& chooser) {
            if (safeThis == nullptr) {
                return;
            }

            const auto selectedFolder = chooser.getResult();

            if (selectedFolder.isDirectory()) {
                safeThis->currentRoot = selectedFolder;
                safeThis->currentRootLabel.setText(selectedFolder.getFullPathName(), juce::dontSendNotification);
                safeThis->refreshAnalysis(false);
            }

            safeThis->directoryChooser.reset();
        }
    );
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

void AudioBatchComponent::refreshAnalysis(bool forceRefresh)
{
    juce::Array<juce::File> inputPaths;
    inputPaths.add(currentRoot);
    startAnalysis(inputPaths, true, forceRefresh, true);
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

void AudioBatchComponent::handleAnalysisResult(const AudioAnalysisRecord& record)
{
    ++completedResults;

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

void AudioBatchComponent::handleAnalysisComplete(int totalFiles)
{
    expectedResults = totalFiles;
    updateStatusLabel();

    if (resultsTable.getSelectedRow() < 0 && !analysisResults.empty()) {
        resultsTable.selectRow(0);
    }
}

void AudioBatchComponent::updateStatusLabel()
{
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

void AudioBatchComponent::sortResults()
{
    std::sort(analysisResults.begin(), analysisResults.end(), [this](const auto& lhs, const auto& rhs) {
        if (lhs.hasError() != rhs.hasError()) {
            return !lhs.hasError();
        }

        auto compareStrings = [this](const juce::String& left, const juce::String& right) {
            const auto comparison = left.compareNatural(right);
            if (comparison == 0) {
                return false;
            }
            return currentSortForwards ? comparison < 0 : comparison > 0;
        };

        auto compareFloats = [this](float left, float right) {
            if (juce::approximatelyEqual(left, right)) {
                return false;
            }
            return compareWithDirection(left, right, currentSortForwards);
        };

        switch (currentSortColumnId) {
            case AudioFileTableModel::columnName:
                if (lhs.fileName != rhs.fileName) {
                    return compareStrings(lhs.fileName, rhs.fileName);
                }
                return compareStrings(lhs.fullPath, rhs.fullPath);

            case AudioFileTableModel::columnPath:
                if (lhs.fullPath != rhs.fullPath) {
                    return compareStrings(lhs.fullPath, rhs.fullPath);
                }
                return compareStrings(lhs.fileName, rhs.fileName);

            case AudioFileTableModel::columnPeakLeft:
                if (!juce::approximatelyEqual(lhs.peakLeft, rhs.peakLeft)) {
                    return compareFloats(lhs.peakLeft, rhs.peakLeft);
                }
                return compareStrings(lhs.fileName, rhs.fileName);

            case AudioFileTableModel::columnPeakRight:
                if (!juce::approximatelyEqual(lhs.peakRight, rhs.peakRight)) {
                    return compareFloats(lhs.peakRight, rhs.peakRight);
                }
                return compareStrings(lhs.fileName, rhs.fileName);

            case AudioFileTableModel::columnStatus:
                if (AudioAnalysisService::formatStatus(lhs) != AudioAnalysisService::formatStatus(rhs)) {
                    return compareStrings(
                        AudioAnalysisService::formatStatus(lhs), AudioAnalysisService::formatStatus(rhs)
                    );
                }
                return compareStrings(lhs.fileName, rhs.fileName);

            case AudioFileTableModel::columnOverallPeak:
            default:
                if (!juce::approximatelyEqual(lhs.overallPeak, rhs.overallPeak)) {
                    return compareFloats(lhs.overallPeak, rhs.overallPeak);
                }
                return compareStrings(lhs.fileName, rhs.fileName);
        }
    });
}

void AudioBatchComponent::startAnalysis(
    const juce::Array<juce::File>& inputPaths,
    bool recursive,
    bool forceRefresh,
    bool clearResults
)
{
    if (clearResults) {
        analysisResults.clear();
        currentAudioFile = {};
        currentAudioUrl = {};
        currentWaveformLoadedFromCache = false;
        currentAudioFileSource.reset();
        transportSource.stop();
        transportSource.setSource(nullptr);
        thumbnail->setURL(juce::URL());
        startStopButton.setEnabled(false);
        startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium);
        audioInfo->clear();
        resultsTable.updateContent();
        updateResultsTableColumnWidths();
        resultsTable.repaint();
    }

    AudioAnalysisOptions options;
    options.inputPaths = inputPaths;
    options.recursive = recursive;
    options.refresh = forceRefresh;

    completedResults = 0;
    expectedResults = AudioAnalysisService::collectInputFiles(options.inputPaths, options.recursive).size();

    if (expectedResults == 0) {
        statusLabel.setText(
            clearResults ? "No supported audio files found" : "No supported dropped files", juce::dontSendNotification
        );
        return;
    }

    statusLabel.setText("Analyzing 0/" + juce::String(expectedResults), juce::dontSendNotification);

    if (clearResults) {
        currentRootLabel.setText(currentRoot.getFullPathName(), juce::dontSendNotification);
    }

    analysisCoordinator.start(options);
}

void AudioBatchComponent::handleSortRequested(int columnId, bool isForwards)
{
    currentSortColumnId = columnId;
    currentSortForwards = isForwards;

    const auto selectedPaths = getSelectedRecordPaths();

    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(selectedPaths);
}

int AudioBatchComponent::getSelectionDisplayRow(const juce::SparseSet<int>& selectedRows, int lastRowSelected) const
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

void AudioBatchComponent::handleSelectionChanged(int lastRowSelected)
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
        utils::log_info("Loaded file: " + record.fileName);
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

    if (record.hasError()) {
        rows.emplace_back("File", record.fileName);
        rows.emplace_back("Error", record.errorMessage);
        audioInfo->setRows(std::move(rows));
        return;
    }

    if (record.formatName.isNotEmpty()) {
        rows.emplace_back("Format", record.formatName);
    }

    rows.emplace_back("Sample rate", juce::String(record.sampleRate));
    rows.emplace_back("Channels", juce::String(record.channels));
    rows.emplace_back("Bits per sample", juce::String(record.bitsPerSample));
    rows.emplace_back("Duration", juce::RelativeTime(record.durationSeconds).getDescription());
    rows.emplace_back("Samples", juce::String(record.lengthInSamples));
    rows.emplace_back("Peak Max", AudioAnalysisService::formatPeakDisplay(record.overallPeak));
    rows.emplace_back("Peak L", AudioAnalysisService::formatPeakDisplay(record.peakLeft));
    rows.emplace_back("Peak R", AudioAnalysisService::formatPeakDisplay(record.peakRight));
    rows.emplace_back("Status", AudioAnalysisService::formatStatus(record));

    if (record.fromCache) {
        rows.emplace_back("Source", "Analysis cache");
    }

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

void AudioBatchComponent::mouseMagnify(const juce::MouseEvent&, float scaleFactor)
{
    auto newZoom = zoomSlider.getValue() * (scaleFactor * scaleFactor);
    zoomSlider.setValue(newZoom);
}

void AudioBatchComponent::zoomLevelChanged(double zoomLevel)
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
