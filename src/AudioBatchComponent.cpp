#include "AudioBatchComponent.h"

#include "CustomLookAndFeel.h"
#include "utils.h"

AudioBatchComponent::AudioBatchComponent() : audioSetupComp(audioDeviceManager, 0, 0, 0, 2, false, false, true, false)
{
    addAndMakeVisible(fileTreeComp);

#if JUCE_WINDOWS
    directoryList.setDirectory(juce::File("D:\\Dropbox\\DJ MUSIC\\FUNKY JAM 1"), false, true);
#else
    directoryList.setDirectory(juce::File("~/Dropbox/DJ MUSIC/FUNKY JAM 1"), false, true);
#endif

    fileTreeComp.setTitle("Files");
    fileTreeComp.addListener(this);
    fileTreeComp.setDragAndDropDescription("AudioBatchFileTree");

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

    // audio setup
    formatManager.registerBasicFormats();

    thread.startThread(juce::Thread::Priority::high);

    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [this](bool granted) {
        int numInputChannels = granted ? 2 : 0;
        audioDeviceManager.initialise(numInputChannels, 2, nullptr, true, {}, nullptr);
    });

    audioDeviceManager.addAudioCallback(&audioSourcePlayer);
    audioSourcePlayer.setSource(&transportSource);

    setOpaque(true);
    setSize(1024, 800);
    setWantsKeyboardFocus(true);
}

AudioBatchComponent::~AudioBatchComponent()
{
    transportSource.setSource(nullptr);
    audioSourcePlayer.setSource(nullptr);

    audioDeviceManager.removeAudioCallback(&audioSourcePlayer);

    fileTreeComp.removeListener(this);

    thumbnail->removeChangeListener(this);

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

    auto info = r.removeFromBottom(juce::jmax(200, juce::roundToIntAccurate(r.getHeight() * 0.275)));

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

    r.removeFromBottom(6);

    fileTreeComp.setBounds(r);
}

bool AudioBatchComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey) {
        startOrStop();
    }

    return false;
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
            audioURL.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)));
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

void AudioBatchComponent::selectionChanged()
{
    auto selectedFile = fileTreeComp.getSelectedFile();
    if (selectedFile.existsAsFile() && selectedFile != currentAudioFile) {
        currentAudioFile = selectedFile;
        audioInfo.clear();
        showAudioResource(juce::URL(currentAudioFile));
        utils::log_info("Loaded file: " + currentAudioFile.getFileName());

        auto reader = currentAudioFileSource.get()->getAudioFormatReader();
        auto metadata = reader->metadataValues;
        auto sampleRate = reader->sampleRate;
        auto channels = reader->numChannels;
        auto samples = reader->lengthInSamples;
        auto seconds = static_cast<double>(samples) / sampleRate;
        juce::RelativeTime length {seconds};

        logAudioInfoMessage(reader->getFormatName());
        logAudioInfoMessage(juce::String(sampleRate) + " samplerate");
        logAudioInfoMessage(juce::String(channels) + " channels");
        logAudioInfoMessage(juce::String(reader->bitsPerSample) + " bits per sample");
        logAudioInfoMessage(length.getDescription());
        logAudioInfoMessage(juce::String(samples) + " samples");
    }
}

void AudioBatchComponent::fileClicked(const juce::File&, const juce::MouseEvent&) {}
void AudioBatchComponent::fileDoubleClicked(const juce::File&)
{
    transportSource.stop();
    transportSource.setPosition(0.0);
}

void AudioBatchComponent::browserRootChanged(const juce::File&) {}

void AudioBatchComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == thumbnail.get()) {
        showAudioResource(juce::URL(thumbnail->getLastDroppedFile()));
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
    const juce::String& title)
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

void AudioBatchComponent::calculateAudioStats()
{
    auto reader = currentAudioFileSource.get()->getAudioFormatReader();
    float minRight {0.0};
    float minLeft {0.0};
    float maxRight {0.0};
    float maxLeft {0.0};
    reader->readMaxLevels(0, reader->lengthInSamples, minLeft, maxLeft, minRight, maxRight);
    juce::String minInfo {"Min: " + juce::String(minLeft, 2) + "   " + juce::String(minRight, 2)};
    juce::String maxInfo {"Max: +" + juce::String(maxLeft, 2) + "   +" + juce::String(maxRight, 2)};
    logAudioInfoMessage(minInfo);
    logAudioInfoMessage(maxInfo);
}

void AudioBatchComponent::logAudioInfoMessage(const juce::String& m)
{
    audioInfo.moveCaretToEnd();
    audioInfo.insertTextAtCaret(m + juce::newLine);
}
