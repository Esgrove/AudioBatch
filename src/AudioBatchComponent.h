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
    bool loadURLIntoTransport(const juce::URL& audioUrl);

    void browserRootChanged(const juce::File&) override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void fileClicked(const juce::File&, const juce::MouseEvent&) override;
    void fileDoubleClicked(const juce::File&) override;
    void logAudioInfoMessage(const juce::String& m);
    void openDialogWindow(
        SafePointer<juce::DialogWindow>& window,
        const SafePointer<juce::Component>& component,
        const juce::String& title);
    void selectionChanged() override;
    void showAudioResource(juce::URL resource);
    void calculateAudioStats();
    void startOrStop();
    void zoomLevelChanged(double zoomLevel);
    void mouseMagnify(const juce::MouseEvent&, float scaleFactor) override;

    std::unique_ptr<juce::AudioFormatReaderSource> currentAudioFileSource;
    std::unique_ptr<ThumbnailComponent> thumbnail;
    std::unique_ptr<juce::WildcardFileFilter> audioFileFilter
        = std::make_unique<juce::WildcardFileFilter>("*.wav;*.aiff;*.aif;*.m4a;*.mp3;*.flac;*.ogg", "", "audio files");

    SafePointer<juce::DialogWindow> settingsWindow;

    juce::Array<juce::Component::SafePointer<juce::DialogWindow>> childWindows;
    juce::AudioDeviceManager audioDeviceManager;
    juce::AudioDeviceSelectorComponent audioSetupComp;
    juce::AudioFormatManager formatManager;
    juce::AudioSourcePlayer audioSourcePlayer;
    juce::AudioTransportSource transportSource;
    juce::DirectoryContentsList directoryList {audioFileFilter.get(), thread};
    juce::File currentAudioFile;
    juce::FileTreeComponent fileTreeComp {directoryList};
    juce::TimeSliceThread thread {"audio file preview"};
    juce::URL currentAudioUrl;

    juce::Label zoomLabel {"Zoom"};
    juce::Slider zoomSlider {"ZoomSlider"};
    juce::TextButton settingsButton {"Settings"};
    juce::TextButton startStopButton {"Play/Stop"};
    juce::TextEditor audioInfo {"AudioInfo"};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioBatchComponent)
};
