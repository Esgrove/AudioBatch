#include "AudioBatchComponent.h"

#include "AudioAnalysisService.h"
#include "CustomLookAndFeel.h"
#include "utils.h"

#include <algorithm>

namespace
{
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
        [this](int row) { handleRowSelected(row); },
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
    resultsTable.setMultipleSelectionEnabled(false);
    resultsTable.setOutlineThickness(0);
    resultsTable.setRowHeight(24);
    resultsTable.getHeader().setSortColumnId(currentSortColumnId, currentSortForwards);
    addAndMakeVisible(resultsTable);

    thumbnail = std::make_unique<ThumbnailComponent>(formatManager, transportSource);
    addAndMakeVisible(thumbnail.get());
    thumbnail->addChangeListener(this);

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

    audioInfo.setMultiLine(true);
    audioInfo.setReadOnly(true);
    audioInfo.setScrollbarsShown(true);
    audioInfo.setCaretVisible(false);
    addAndMakeVisible(audioInfo);

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

    resultsTable.setBounds(r.removeFromTop(juce::jmax(240, juce::roundToIntAccurate(r.getHeight() * 0.42f))));

    r.removeFromTop(6);

    auto info = r;

    auto space = juce::jmin(250, juce::jmax(150, juce::roundToIntAccurate(r.getWidth() * 0.2)));
    auto control = info.removeFromLeft(space);
    auto buttons = control.removeFromBottom(50);

    settingsButton.setBounds(buttons.removeFromLeft(juce::roundToIntAccurate((space - 6) * 0.5)));
    buttons.removeFromLeft(6);
    startStopButton.setBounds(buttons);

    control.removeFromBottom(12);

    zoomSlider.setBounds(control.removeFromBottom(zoomSlider.getTextBoxHeight()));

    control.removeFromBottom(12);

    audioInfo.setBounds(control);

    info.removeFromLeft(6);

    thumbnail->setBounds(info);
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
    directoryChooser = std::make_unique<juce::FileChooser>("Choose Audio Folder", currentRoot, juce::String(), true);

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

void AudioBatchComponent::refreshAnalysis(bool forceRefresh)
{
    analysisResults.clear();
    completedResults = 0;
    currentAudioFile = {};
    currentAudioUrl = {};
    currentAudioFileSource.reset();
    transportSource.stop();
    transportSource.setSource(nullptr);
    thumbnail->setURL(juce::URL());
    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium);
    audioInfo.clear();
    resultsTable.updateContent();
    resultsTable.repaint();

    AudioAnalysisOptions options;
    options.inputPaths.add(currentRoot);
    options.recursive = true;
    options.refresh = forceRefresh;

    expectedResults = AudioAnalysisService::collectInputFiles(options.inputPaths, options.recursive).size();

    if (expectedResults == 0) {
        statusLabel.setText("No supported audio files found", juce::dontSendNotification);
        return;
    }

    statusLabel.setText("Analyzing 0/" + juce::String(expectedResults), juce::dontSendNotification);
    currentRootLabel.setText(currentRoot.getFullPathName(), juce::dontSendNotification);
    analysisCoordinator.start(options);
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

void AudioBatchComponent::handleAnalysisResult(const AudioAnalysisRecord& record)
{
    ++completedResults;

    const auto selectedRow = resultsTable.getSelectedRow();
    const auto selectedPath = juce::isPositiveAndBelow(selectedRow, static_cast<int>(analysisResults.size()))
        ? analysisResults[static_cast<std::size_t>(selectedRow)].fullPath
        : juce::String();

    if (const auto existingIndex = findRecordIndex(record.fullPath); existingIndex >= 0) {
        analysisResults[static_cast<std::size_t>(existingIndex)] = record;
    } else {
        analysisResults.push_back(record);
    }

    sortResults();
    resultsTable.updateContent();

    if (selectedPath.isNotEmpty()) {
        if (const auto newSelectedIndex = findRecordIndex(selectedPath); newSelectedIndex >= 0) {
            resultsTable.selectRow(newSelectedIndex);
        }
    }

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

void AudioBatchComponent::handleSortRequested(int columnId, bool isForwards)
{
    currentSortColumnId = columnId;
    currentSortForwards = isForwards;

    const auto selectedRow = resultsTable.getSelectedRow();
    const auto selectedPath = juce::isPositiveAndBelow(selectedRow, static_cast<int>(analysisResults.size()))
        ? analysisResults[static_cast<std::size_t>(selectedRow)].fullPath
        : juce::String();

    sortResults();
    resultsTable.updateContent();

    if (selectedPath.isNotEmpty()) {
        if (const auto newIndex = findRecordIndex(selectedPath); newIndex >= 0) {
            resultsTable.selectRow(newIndex);
        }
    }
}

void AudioBatchComponent::handleRowSelected(int row)
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(row)];

    if (record.file.existsAsFile()) {
        currentAudioFile = record.file;
        showAudioResource(juce::URL(record.file));
        utils::log_info("Loaded file: " + record.fileName);
    }

    updateAudioInfo(record);
}

void AudioBatchComponent::updateAudioInfo(const AudioAnalysisRecord& record)
{
    audioInfo.clear();

    if (record.hasError()) {
        logAudioInfoMessage(record.fileName);
        logAudioInfoMessage("Error: " + record.errorMessage);
        return;
    }

    if (record.formatName.isNotEmpty()) {
        logAudioInfoMessage(record.formatName);
    }

    logAudioInfoMessage(juce::String(record.sampleRate) + " samplerate");
    logAudioInfoMessage(juce::String(record.channels) + " channels");
    logAudioInfoMessage(juce::String(record.bitsPerSample) + " bits per sample");
    logAudioInfoMessage(juce::RelativeTime(record.durationSeconds).getDescription());
    logAudioInfoMessage(juce::String(record.lengthInSamples) + " samples");
    logAudioInfoMessage("Peak L: " + AudioAnalysisService::formatPeakDisplay(record.peakLeft));
    logAudioInfoMessage("Peak R: " + AudioAnalysisService::formatPeakDisplay(record.peakRight));
    logAudioInfoMessage("Peak Max: " + AudioAnalysisService::formatPeakDisplay(record.overallPeak));
    logAudioInfoMessage("Status: " + AudioAnalysisService::formatStatus(record));

    if (record.fromCache) {
        logAudioInfoMessage("Source: Analysis cache");
    }
}

void AudioBatchComponent::showAudioResource(juce::URL resource)
{
    if (loadURLIntoTransport(resource)) {
        currentAudioUrl = std::move(resource);
        startStopButton.setEnabled(true);
        startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::green);
        zoomSlider.setValue(100.0, juce::dontSendNotification);
    } else {
        startStopButton.setEnabled(false);
        startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium);
    }

    thumbnail->setURL(currentAudioUrl);
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
        const auto droppedFile = thumbnail->getLastDroppedFile().getLocalFile();

        if (!droppedFile.existsAsFile()) {
            return;
        }

        currentAudioFile = droppedFile;
        showAudioResource(juce::URL(droppedFile));

        if (const auto existingIndex = findRecordIndex(droppedFile.getFullPathName()); existingIndex >= 0) {
            updateAudioInfo(analysisResults[static_cast<std::size_t>(existingIndex)]);
        } else {
            updateAudioInfo(AudioAnalysisService::analyzeFile(droppedFile));
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

void AudioBatchComponent::logAudioInfoMessage(const juce::String& m)
{
    audioInfo.moveCaretToEnd();
    audioInfo.insertTextAtCaret(m + juce::newLine);
}
