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
    bool keyPressed(const juce::KeyPress& key) override;
    bool loadURLIntoTransport(const juce::URL& audioURL);

    void browserRootChanged(const juce::File&) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void fileClicked(const juce::File&, const juce::MouseEvent&) override;
    void fileDoubleClicked(const juce::File&) override;
    void selectionChanged() override;
    void showAudioResource(juce::URL resource);
    void startOrStop();
    void zoomLevelChanged(double zoomLevel);
    void showAudioStats();
    void logAudioInfoMessage(const juce::String& m);

    void openDialogWindow(
        SafePointer<juce::DialogWindow>& window,
        const SafePointer<juce::Component>& component,
        const juce::String& title);

    juce::AudioDeviceManager audioDeviceManager;

    juce::AudioFormatManager formatManager;
    juce::TimeSliceThread thread {"audio file preview"};

    std::unique_ptr<juce::WildcardFileFilter> audioFileFilter
        = std::make_unique<juce::WildcardFileFilter>("*.wav;*.aiff;*.aif;*.m4a;*.mp3;*.flac;*.ogg", "", "audio files");

    juce::DirectoryContentsList directoryList {audioFileFilter.get(), thread};
    juce::FileTreeComponent fileTreeComp {directoryList};

    juce::URL currentAudioUrl;
    juce::File currentAudioFile;
    juce::AudioSourcePlayer audioSourcePlayer;
    juce::AudioTransportSource transportSource;
    std::unique_ptr<juce::AudioFormatReaderSource> currentAudioFileSource;

    SafePointer<juce::DialogWindow> settingsWindow;

    juce::Array<juce::Component::SafePointer<juce::DialogWindow>> childWindows;

    std::unique_ptr<ThumbnailComponent> thumbnail;
    juce::TextButton startStopButton {"Play/Stop"};

    juce::Slider zoomSlider {"ZoomSlider"};
    juce::Label zoomLabel {"Zoom"};
    juce::TextEditor audioInfo {"AudioInfo"};
    juce::TextButton settingsButton {"Settings"};
    juce::AudioDeviceSelectorComponent audioSetupComp;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioBatchComponent)
};
