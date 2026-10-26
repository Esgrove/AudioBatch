/// Bottom section of the main window below the results table.
/// Declares PreviewWorkspace, which holds the waveform, the selected file's metrics, the playback and process buttons,
/// and the scrolling zoom, gain, normalization, plugin chain, and audio settings controls,
/// and lays them out as an expanded section or a compact row.

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

    AudioInfoPanel audioInfo;
    ThumbnailComponent thumbnail;
    juce::DrawableButton previewSectionButton {"Collapse bottom section", juce::DrawableButton::ImageFitted};
    juce::TextButton startStopButton {"Play/Stop"};
    juce::TextButton processButton {"Process"};

    /// The secondary controls scroll independently while playback and processing stay pinned below them.
    /// Declared before the viewport that shows it without owning it.
    juce::Component controlsContent;
    juce::Viewport controlsViewport;

    juce::Label zoomLabel {"Zoom"};
    juce::Slider zoomSlider {"ZoomSlider"};
    juce::Label gainLabel {"GainLabel"};
    IntervalStepSlider gainSlider {"GainSlider"};
    juce::ToggleButton normalizeBeforePluginToggle {"Normalize"};
    juce::ToggleButton forceOverwriteToggle {"Overwrite"};

    /// Normalization mode and target, persisted in the settings given to the constructor.
    NormalizationTargetControl normalizationTargetControl;

    juce::TextButton pluginButton {"Plugin chain"};
    juce::TextButton settingsButton {"Audio Settings"};

private:
    void configurePreviewSectionButton();
    void configureSecondaryControls();
    void layoutCompact(juce::Rectangle<int> bounds, int playbackWidth);
    void layoutPlaybackButtons(juce::Rectangle<int> bounds);
};
}  // namespace audiobatch
