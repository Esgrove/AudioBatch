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
#include "gui/GuiLayout.h"

#include <JuceHeader.h>

#include <memory>

namespace audiobatch
{
class AudioPreviewPlayer;
class PreviewWidthDivider;

/// Holds and lays out the controls of the preview workspace.
/// The constructor sets up how the controls look,
/// and the main component connects their callbacks and keeps them in sync with the selection.
/// The controls are public, because the main component drives each of them directly.
class PreviewWorkspace final : public juce::Component
{
public:
    PreviewWorkspace(AudioPreviewPlayer& previewPlayer, juce::ApplicationProperties& settings);

    ~PreviewWorkspace() override;

    PreviewWorkspace(const PreviewWorkspace&) = delete;
    PreviewWorkspace(PreviewWorkspace&&) = delete;
    PreviewWorkspace& operator=(const PreviewWorkspace&) = delete;
    PreviewWorkspace& operator=(PreviewWorkspace&&) = delete;

    [[nodiscard]] bool isCompact() const noexcept;

    void loadWidthSettings();
    void setCompact(bool shouldBeCompact);
    void setPlaybackState(bool enabled, bool playing);

    // juce::Component
    void resized() override;

    /// Selected-file summary, metrics and details.
    AudioInfoPanel audioInfo;
    /// Custom gain in decibels.
    IntervalStepSlider gainSlider {"GainSlider"};
    /// Normalization mode and target persisted in the supplied settings.
    NormalizationTargetControl normalizationTargetControl;
    /// Waveform of the previewed file.
    ThumbnailComponent thumbnail;

    /// Secondary-control parent outlives the viewport borrowing it.
    juce::Component controlsContent;
    /// Collapses or expands the bottom section.
    juce::DrawableButton previewSectionButton {"Collapse bottom section", juce::DrawableButton::ImageFitted};
    /// Slider captions.
    juce::Label gainLabel {"GainLabel"};
    juce::Label zoomLabel {"Zoom"};
    /// Waveform zoom in percent.
    juce::Slider zoomSlider {"ZoomSlider"};

    /// Opens the chain menu.
    juce::TextButton pluginButton {"Plugin chain"};
    /// Processes selected files, or cancels the running pass.
    juce::TextButton processButton {"Process"};
    /// Opens audio settings.
    juce::TextButton settingsButton {"Audio Settings"};
    /// Starts playback, showing Stop while playing.
    juce::TextButton startStopButton {"Play"};

    /// Allows overwriting existing output files.
    juce::ToggleButton forceOverwriteToggle {"Overwrite"};
    /// Normalizes before plugin processing.
    juce::ToggleButton normalizeBeforePluginToggle {"Normalize"};
    /// Enables live-chain playback in the wide layout.
    juce::ToggleButton previewChainToggle {"Preview chain"};

    /// Scrolls borrowed secondary controls independently of the playback controls.
    juce::Viewport controlsViewport;

    /// Width interaction state stays public with this component's existing all-public UI facade.
    /// Shared application settings outlive the workspace. Reads are deferred until storage is configured.
    juce::ApplicationProperties& properties;
    /// Dividers are destroyed before the controls they observe.
    std::unique_ptr<PreviewWidthDivider> controlsWidthDivider;
    std::unique_ptr<PreviewWidthDivider> informationWidthDivider;
    double controlsWidthScale {layout::previewWidths::DEFAULT_CONTROLS_SCALE};
    double informationWidthScale {layout::previewWidths::DEFAULT_INFORMATION_SCALE};

private:
    juce::Rectangle<int> layoutSliders(juce::Rectangle<int> content);

    void configurePreviewSectionButton();
    void configureSecondaryControls();
    void layoutCompact(juce::Rectangle<int> bounds, int playbackWidth);
    void layoutNarrowControls(juce::Rectangle<int> content);
    void layoutPlaybackButtons(juce::Rectangle<int> bounds);
    void layoutWideControls(juce::Rectangle<int> content);
    void saveWidthSettings();
    void setPanelWidth(bool information, int width);
};
}  // namespace audiobatch
