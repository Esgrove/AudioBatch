#pragma once

#include "ThumbnailComponent.h"

#include <JuceHeader.h>

class AudioBatchComponent : public juce::Component, private juce::FileBrowserListener, private juce::ChangeListener
{
public:
    AudioBatchComponent();
    ~AudioBatchComponent() override;

    void paint(juce::Graphics& g) override;

    void resized() override;

private:
    juce::AudioDeviceManager audioDeviceManager;

    juce::AudioFormatManager formatManager;
    juce::TimeSliceThread thread {"audio file preview"};

    juce::DirectoryContentsList directoryList {nullptr, thread};
    juce::FileTreeComponent fileTreeComp {directoryList};

    juce::URL currentAudioFile;
    juce::AudioSourcePlayer audioSourcePlayer;
    juce::AudioTransportSource transportSource;
    std::unique_ptr<juce::AudioFormatReaderSource> currentAudioFileSource;

    std::unique_ptr<ThumbnailComponent> thumbnail;
    juce::TextButton startStopButton {"Play/Stop"};

    void showAudioResource(juce::URL resource);

    bool loadURLIntoTransport(const juce::URL& audioURL);

    void startOrStop();

    void selectionChanged() override;

    void fileClicked(const juce::File&, const juce::MouseEvent&) override;
    void fileDoubleClicked(const juce::File&) override;
    void browserRootChanged(const juce::File&) override;

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioBatchComponent)
};
