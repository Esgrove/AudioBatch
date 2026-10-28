/// Bottom section of the main window below the results table.
/// Declares PreviewWorkspace, which holds the waveform, the selected file's metrics, the playback and process buttons,
/// and the scrolling zoom, gain, normalization, plugin chain, and audio settings controls,
/// and lays them out as an expanded section, a wide section with two control columns below the sliders,
/// or a compact row.

#pragma once

#include "gui/components/AudioInfoPanel.h"
#include "gui/components/IntervalStepSlider.h"
#include "gui/components/NormalizationTargetControl.h"
#include "gui/components/ThumbnailComponent.h"

#include <JuceHeader.h>

namespace audiobatch
{
class AudioPreviewPlayer;

/// Holds and lays out the controls of the preview workspace.
/// The constructor sets up how the controls look,
/// and the main component connects their callbacks and keeps them in sync with the selection.
/// The controls are public, because the main component drives each of them directly.
class PreviewWorkspace final : public juce::Component
{
public:
    PreviewWorkspace(AudioPreviewPlayer& previewPlayer, juce::ApplicationProperties& settings);

    [[nodiscard]] bool isCompact() const noexcept;

    void resized() override;
    void setCompact(bool shouldBeCompact);
    void setPlaybackState(bool enabled, bool playing);

    /// The summary, metrics, and details of the selected file.
    AudioInfoPanel audioInfo;
    /// The waveform of the previewed file.
    ThumbnailComponent thumbnail;
    /// Collapses the workspace to the compact row and expands it again.
    juce::DrawableButton previewSectionButton {"Collapse bottom section", juce::DrawableButton::ImageFitted};
    /// Starts the preview, and reads "Stop" while it plays.
    juce::TextButton startStopButton {"Play"};
    /// Processes the selected files through the plugin chain, or cancels the running pass.
    juce::TextButton processButton {"Process"};

    /// The secondary controls scroll independently while playback and processing stay pinned below them.
    /// Declared before the viewport that shows it without owning it.
    juce::Component controlsContent;
    /// Scrolls the secondary controls when the column is shorter than they are.
    juce::Viewport controlsViewport;

    /// Caption of the zoom slider.
    juce::Label zoomLabel {"Zoom"};
    /// Zoom of the waveform in percent.
    juce::Slider zoomSlider {"ZoomSlider"};
    /// Caption of the gain slider.
    juce::Label gainLabel {"GainLabel"};
    /// Custom gain of the selected files in decibels.
    IntervalStepSlider gainSlider {"GainSlider"};
    /// Normalizes the files to the target before the plugin chain processes them.
    juce::ToggleButton normalizeBeforePluginToggle {"Normalize"};
    /// Lets normalization and processing replace an existing output file.
    juce::ToggleButton forceOverwriteToggle {"Overwrite"};
    /// Plays the preview through the plugin chain, shown only in the wide layout, which has a free row for it.
    juce::ToggleButton previewChainToggle {"Preview chain"};

    /// Normalization mode and target, persisted in the settings given to the constructor.
    NormalizationTargetControl normalizationTargetControl;

    /// Opens the plugin chain menu.
    juce::TextButton pluginButton {"Plugin chain"};
    /// Opens the audio settings dialog.
    juce::TextButton settingsButton {"Audio Settings"};

private:
    void configurePreviewSectionButton();
    void configureSecondaryControls();
    void layoutCompact(juce::Rectangle<int> bounds, int playbackWidth);
    void layoutNarrowControls(juce::Rectangle<int> content);
    void layoutPlaybackButtons(juce::Rectangle<int> bounds);
    juce::Rectangle<int> layoutSliders(juce::Rectangle<int> content);
    void layoutWideControls(juce::Rectangle<int> content);
};
}  // namespace audiobatch
