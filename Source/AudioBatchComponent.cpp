#include "AudioBatchComponent.h"

AudioBatchComponent::AudioBatchComponent()
{
    addAndMakeVisible(fileTreeComp);

    directoryList.setDirectory(juce::File("D:\\Dropbox\\DJ MUSIC"), true, true);

    fileTreeComp.setTitle("Files");
    fileTreeComp.setColour(juce::FileTreeComponent::backgroundColourId, juce::Colours::lightgrey.withAlpha(0.6f));
    fileTreeComp.addListener(this);

    thumbnail.reset(new ThumbnailComponent(formatManager, transportSource));
    addAndMakeVisible(thumbnail.get());
    thumbnail->addChangeListener(this);

    addAndMakeVisible(startStopButton);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff79ed7f));
    startStopButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    startStopButton.onClick = [this] { startOrStop(); };

    // audio setup
    formatManager.registerBasicFormats();

    thread.startThread(3);

    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [this](bool granted) {
        int numInputChannels = granted ? 2 : 0;
        audioDeviceManager.initialise(numInputChannels, 2, nullptr, true, {}, nullptr);
    });

    audioDeviceManager.addAudioCallback(&audioSourcePlayer);
    audioSourcePlayer.setSource(&transportSource);

    setOpaque(true);
    setSize(1024, 800);
}

AudioBatchComponent::~AudioBatchComponent()
{
    transportSource.setSource(nullptr);
    audioSourcePlayer.setSource(nullptr);

    audioDeviceManager.removeAudioCallback(&audioSourcePlayer);

    fileTreeComp.removeListener(this);

    thumbnail->removeChangeListener(this);
}

void AudioBatchComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioBatchComponent::resized()
{
    auto r = getLocalBounds().reduced(4);

    auto controls = r.removeFromBottom(90);

    startStopButton.setBounds(controls);

    r.removeFromBottom(6);

    thumbnail->setBounds(r.removeFromBottom(140));
    r.removeFromBottom(6);

    fileTreeComp.setBounds(r);
}

void AudioBatchComponent::showAudioResource(juce::URL resource)
{
    if (loadURLIntoTransport(resource))
        currentAudioFile = std::move(resource);

    thumbnail->setURL(currentAudioFile);
}

bool AudioBatchComponent::loadURLIntoTransport(const juce::URL& audioURL)
{
    // unload the previous file source and delete it..
    transportSource.stop();
    transportSource.setSource(nullptr);
    currentAudioFileSource.reset();

    juce::AudioFormatReader* reader = nullptr;

    if (reader == nullptr)
        reader = formatManager.createReaderFor(audioURL.createInputStream(false));

    if (reader != nullptr) {
        currentAudioFileSource.reset(new juce::AudioFormatReaderSource(reader, true));

        // ..and plug it into our transport source
        transportSource.setSource(
            currentAudioFileSource.get(),
            32768,                // tells it to buffer this many samples ahead
            &thread,              // this is the background thread to use for reading-ahead
            reader->sampleRate);  // allows for sample rate correction

        return true;
    }

    return false;
}

void AudioBatchComponent::startOrStop()
{
    transportSource.isPlaying() ? transportSource.stop() : transportSource.start();
}

void AudioBatchComponent::selectionChanged()
{
    showAudioResource(juce::URL(fileTreeComp.getSelectedFile()));
}

void AudioBatchComponent::fileClicked(const juce::File&, const juce::MouseEvent&) {}
void AudioBatchComponent::fileDoubleClicked(const juce::File&) {}
void AudioBatchComponent::browserRootChanged(const juce::File&) {}

void AudioBatchComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == thumbnail.get()) {
        showAudioResource(juce::URL(thumbnail->getLastDroppedFile()));
    }
}
