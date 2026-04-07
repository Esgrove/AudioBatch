#pragma once

#include "AnalysisCache.h"
#include "AnalysisCoordinator.h"
#include "AudioFileTableModel.h"
#include "ThumbnailComponent.h"

#include <JuceHeader.h>

#include <vector>

class AudioInfoPanel;

class AudioBatchComponent :
    public juce::Component,
    public juce::FileDragAndDropTarget,
    public juce::DragAndDropContainer,
    private juce::ChangeListener
{
public:
    AudioBatchComponent();
    ~AudioBatchComponent() override;

    void filesDropped(const juce::StringArray& files, int x, int y) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void handleDroppedPaths(const juce::StringArray& paths);
    bool keyPressed(const juce::KeyPress& key) override;
    void browseForRootFolder();
    void handleAnalysisComplete(int totalFiles);
    void handleAnalysisResult(const AudioAnalysisRecord& record);
    void handleThumbnailFullyLoaded();
    void handleSelectionChanged(int lastRowSelected);
    void handleSortRequested(int columnId, bool isForwards);
    bool loadURLIntoTransport(const juce::URL& audioUrl);
    int findRecordIndex(const juce::String& fullPath) const;
    juce::StringArray getSelectedRecordPaths() const;
    int getSelectionDisplayRow(const juce::SparseSet<int>& selectedRows, int lastRowSelected) const;
    static juce::File getInitialRootDirectory();
    void refreshAnalysis(bool forceRefresh);
    void restoreSelectionByPaths(const juce::StringArray& selectedPaths);
    void sortResults();
    void startAnalysis(const juce::Array<juce::File>& inputPaths, bool recursive, bool forceRefresh, bool clearResults);
    void updateResultsTableColumnWidths();
    void updateAudioInfo(const AudioAnalysisRecord& record);
    void updateStatusLabel();
    bool shouldDropFilesWhenDraggedExternally(
        const juce::DragAndDropTarget::SourceDetails& sourceDetails,
        juce::StringArray& files,
        bool& canMoveFiles
    ) override;

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void openDialogWindow(
        SafePointer<juce::DialogWindow>& window,
        const SafePointer<juce::Component>& component,
        const juce::String& title
    );
    void showAudioResource(juce::URL resource);
    void startOrStop();
    void zoomLevelChanged(double zoomLevel);
    void mouseMagnify(const juce::MouseEvent&, float scaleFactor) override;

    AnalysisCache analysisCache;
    AnalysisCoordinator analysisCoordinator;
    std::vector<AudioAnalysisRecord> analysisResults;
    juce::StretchableLayoutManager mainVerticalLayout;
    juce::StretchableLayoutResizerBar waveformResizeBar {&mainVerticalLayout, 1, false};
    AudioFileTableModel fileTableModel;
    std::unique_ptr<juce::AudioFormatReaderSource> currentAudioFileSource;
    std::unique_ptr<juce::FileChooser> directoryChooser;
    std::unique_ptr<ThumbnailComponent> thumbnail;

    SafePointer<juce::DialogWindow> settingsWindow;

    juce::Array<juce::Component::SafePointer<juce::DialogWindow>> childWindows;
    juce::AudioDeviceManager audioDeviceManager;
    juce::AudioDeviceSelectorComponent audioSetupComp;
    juce::AudioFormatManager formatManager;
    juce::AudioSourcePlayer audioSourcePlayer;
    juce::AudioTransportSource transportSource;
    juce::File currentAudioFile;
    juce::File currentRoot;
    juce::TableListBox resultsTable;
    juce::TimeSliceThread thread {"audio file preview"};
    juce::URL currentAudioUrl;
    int completedResults = 0;
    int expectedResults = 0;
    int currentSortColumnId = AudioFileTableModel::columnOverallPeak;
    bool currentSortForwards = true;
    bool currentWaveformLoadedFromCache = false;

    juce::Label currentRootLabel {"CurrentRootLabel"};
    juce::Label statusLabel {"StatusLabel"};
    juce::Label zoomLabel {"Zoom"};
    juce::Slider zoomSlider {"ZoomSlider"};
    juce::TextButton chooseFolderButton {"Choose Folder"};
    juce::TextButton rescanButton {"Rescan"};
    juce::TextButton settingsButton {"Settings"};
    juce::TextButton startStopButton {"Play/Stop"};
    std::unique_ptr<AudioInfoPanel> audioInfo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioBatchComponent)
};
