/// Implementation of PreviewWorkspace:
/// the look of the preview controls, the chevron images of the section toggle,
/// and the expanded and compact layouts that reuse the same controls.

#include "gui/components/PreviewWorkspace.h"

#include "gui/AudioBatchComponentConstants.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"

namespace audiobatch
{
/// Creates the controls and adds them to the workspace, with the playback controls disabled until a file loads.
/// The settings must outlive the workspace, and are only read once the owner loads the normalization target.
PreviewWorkspace::PreviewWorkspace(AudioPreviewPlayer& previewPlayer, juce::ApplicationProperties& settings) :
    thumbnail(previewPlayer.getFormatManager(), previewPlayer),
    normalizationTargetControl(settings)
{
    configurePreviewSectionButton();
    controlsContent.addAndMakeVisible(previewSectionButton);

    controlsViewport.setViewedComponent(&controlsContent, false);
    controlsViewport.setScrollBarsShown(true, false);
    controlsViewport.setScrollBarThickness(layout::scrollBarThickness);
    controlsViewport.setName("Preview and processing controls");
    addAndMakeVisible(controlsViewport);

    addAndMakeVisible(thumbnail);

    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, CustomLookAndFeel::greyMedium());
    startStopButton.setColour(juce::TextButton::buttonOnColourId, CustomLookAndFeel::green());
    startStopButton.setTooltip("Start or stop preview playback for the selected file.");
    addAndMakeVisible(startStopButton);

    addAndMakeVisible(audioInfo);

    processButton.setTooltip("Process selected files through the plugin chain.");
    processButton.setColour(juce::TextButton::buttonColourId, CustomLookAndFeel::blue());
    processButton.setColour(juce::TextButton::textColourOffId, CustomLookAndFeel::greyDark());
    addAndMakeVisible(processButton);

    configureSecondaryControls();
}

/// Reports whether the workspace shows the compact row, in which the secondary controls are hidden.
bool PreviewWorkspace::isCompact() const noexcept
{
    return !controlsViewport.isVisible();
}

/// Switches between the expanded section and the compact row, hiding the secondary controls in the compact row.
/// The section toggle sits in the Zoom row of the controls while expanded, and leads the compact row.
void PreviewWorkspace::setCompact(const bool shouldBeCompact)
{
    if (shouldBeCompact == isCompact()) {
        return;
    }
    controlsViewport.setVisible(!shouldBeCompact);
    if (shouldBeCompact) {
        addAndMakeVisible(previewSectionButton);
    } else {
        controlsContent.addAndMakeVisible(previewSectionButton);
    }
    audioInfo.setCompact(shouldBeCompact);
    resized();
}

/// Lays out the metrics, the waveform, and the controls in columns,
/// or in one row with the toggle, the playback buttons, the metrics, and the waveform when compact.
/// The controls and the pinned playback row share the same margins, so the column is centred,
/// and the scroll bar takes width only while the controls overflow the column.
void PreviewWorkspace::resized()
{
    const auto workspace = getLocalBounds().reduced(layout::compactPreview::padding);
    const auto columns = layout::previewColumns(workspace);
    if (isCompact()) {
        layoutCompact(
            workspace, columns.controls.getWidth() - layout::previewToggle::size - layout::compactPreview::spacing
        );
        return;
    }

    audioInfo.setBounds(columns.information);
    thumbnail.setBounds(columns.waveform);
    auto controls
        = columns.controls.reduced(layout::information::horizontalPadding, layout::information::verticalPadding);
    layoutPlaybackButtons(controls.removeFromBottom(layout::processButtonHeight));
    controls.removeFromBottom(layout::controlsSectionSpacing);
    controlsViewport.setBounds(controls);
    const bool overflows = layout::controlsContentHeight > controls.getHeight();
    controlsContent.setSize(
        juce::jmax(0, controls.getWidth() - (overflows ? layout::scrollBarThickness : 0)),
        juce::jmax(controls.getHeight(), layout::controlsContentHeight)
    );
    auto content = controlsContent.getLocalBounds();
    auto zoomRow = content.removeFromTop(layout::sliderRowHeight);
    previewSectionButton.setBounds(
        zoomRow.removeFromRight(layout::previewToggle::size)
            .withSizeKeepingCentre(layout::previewToggle::size, layout::previewToggle::size)
    );
    zoomLabel.setBounds(zoomRow);
    zoomSlider.setBounds(content.removeFromTop(layout::sliderRowHeight));
    content.removeFromTop(layout::controlsSectionSpacing);
    gainLabel.setBounds(content.removeFromTop(layout::sliderRowHeight));
    gainSlider.setBounds(content.removeFromTop(layout::sliderRowHeight));
    content.removeFromTop(layout::controlsSectionSpacing);
    auto normalizeRow = content.removeFromTop(layout::normalizeToggleHeight);
    normalizeBeforePluginToggle.setBounds(
        normalizeRow.removeFromLeft((normalizeRow.getWidth() - layout::sectionSpacing) / 2)
    );
    normalizeRow.removeFromLeft(layout::sectionSpacing);
    forceOverwriteToggle.setBounds(normalizeRow);
    content.removeFromTop(layout::normalizeTargetSpacing);
    normalizationTargetControl.setBounds(content.removeFromTop(layout::normalizationTargetHeight));
    content.removeFromTop(layout::controlsSectionSpacing);
    pluginButton.setBounds(content.removeFromTop(layout::controlButtonHeight));
    content.removeFromTop(layout::controlsSectionSpacing);
    settingsButton.setBounds(content.removeFromTop(layout::controlButtonHeight));
}

/// Draws the toggle as a chevron that points down while the section is expanded and up while it is collapsed.
void PreviewWorkspace::configurePreviewSectionButton()
{
    juce::Path chevronPath;
    chevronPath.startNewSubPath(0.0f, 0.0f);
    chevronPath.lineTo(layout::previewToggle::glyphWidth * 0.5f, layout::previewToggle::glyphHeight);
    chevronPath.lineTo(layout::previewToggle::glyphWidth, 0.0f);
    juce::DrawablePath collapseChevron;
    collapseChevron.setPath(chevronPath);
    collapseChevron.setFill(juce::Colours::transparentBlack);
    collapseChevron.setStrokeFill(CustomLookAndFeel::greyMiddleLight());
    collapseChevron.setStrokeThickness(layout::previewToggle::strokeWidth);
    chevronPath.applyTransform(
        juce::AffineTransform::rotation(
            juce::MathConstants<float>::pi,
            layout::previewToggle::glyphWidth * 0.5f,
            layout::previewToggle::glyphHeight * 0.5f
        )
    );
    juce::DrawablePath expandChevron;
    expandChevron.setPath(chevronPath);
    expandChevron.setFill(juce::Colours::transparentBlack);
    expandChevron.setStrokeFill(CustomLookAndFeel::greyMiddleLight());
    expandChevron.setStrokeThickness(layout::previewToggle::strokeWidth);
    previewSectionButton.setImages(&collapseChevron, nullptr, nullptr, nullptr, &expandChevron);
    previewSectionButton.setEdgeIndent(layout::previewToggle::imageInset);
    previewSectionButton.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    previewSectionButton.setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
    previewSectionButton.setTooltip("Collapse bottom section (Ctrl+Shift+Space / Cmd+Shift+Space).");
}

/// Sets up the scrolling controls column:
/// the zoom and gain sliders, the processing toggles, and the plugin chain and settings buttons.
void PreviewWorkspace::configureSecondaryControls()
{
    zoomSlider.setRange(zoom_range::minimumPercent, zoom_range::maximumPercent, 1);
    zoomSlider.setNumDecimalPlacesToDisplay(0);
    zoomSlider.setSkewFactor(0.5);
    zoomSlider.setValue(zoom_range::minimumPercent);
    zoomSlider.setTextValueSuffix("%");
    zoomSlider.setTooltip("Adjust waveform zoom. You can also use the mouse wheel over the waveform.");
    zoomSlider.setTextBoxStyle(
        juce::Slider::TextBoxRight, false, layout::zoomTextBoxWidth, zoomSlider.getTextBoxHeight()
    );
    controlsContent.addAndMakeVisible(zoomSlider);

    zoomLabel.setText("Zoom", juce::dontSendNotification);
    zoomLabel.setJustificationType(juce::Justification::centredLeft);
    controlsContent.addAndMakeVisible(zoomLabel);

    pluginButton.setTooltip("Edit the plugin chain, add plugins, or scan for plugins.");
    controlsContent.addAndMakeVisible(pluginButton);

    gainLabel.setText("Input gain", juce::dontSendNotification);
    gainLabel.setJustificationType(juce::Justification::centredLeft);
    controlsContent.addAndMakeVisible(gainLabel);

    gainSlider.setRange(-gain_range::maximumDecibels, gain_range::maximumDecibels, gain_range::stepDecibels);
    gainSlider.setValue(0.0, juce::dontSendNotification);
    gainSlider.setNumDecimalPlacesToDisplay(1);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setTooltip("Per-file gain in dB. Non-zero values override normalization for the file.");
    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(
        juce::Slider::TextBoxRight, false, layout::gainTextBoxWidth, gainSlider.getTextBoxHeight()
    );
    gainSlider.setDoubleClickReturnValue(true, 0.0);
    gainSlider.setEnabled(false);
    controlsContent.addAndMakeVisible(gainSlider);

    normalizeBeforePluginToggle.setTooltip(
        "When enabled, files without a custom gain are normalized to the selected peak or loudness target "
        "before the plugin chain."
    );
    controlsContent.addAndMakeVisible(normalizeBeforePluginToggle);

    forceOverwriteToggle.setTooltip(
        "When enabled, normalizing or processing replaces an existing output file with the same name. "
        "The replaced file is moved to the system trash."
    );
    controlsContent.addAndMakeVisible(forceOverwriteToggle);

    controlsContent.addAndMakeVisible(normalizationTargetControl);

    settingsButton.setTooltip("Open audio device and preview settings.");
    controlsContent.addAndMakeVisible(settingsButton);
}

/// Lays out the compact row: the toggle, the playback buttons at the same width as in the expanded section,
/// the metrics, and the waveform in the remaining width.
void PreviewWorkspace::layoutCompact(juce::Rectangle<int> bounds, const int playbackWidth)
{
    const auto toggleBounds = bounds.removeFromLeft(layout::previewToggle::size);
    previewSectionButton.setBounds(
        toggleBounds.withSizeKeepingCentre(layout::previewToggle::size, layout::previewToggle::size)
    );
    bounds.removeFromLeft(layout::compactPreview::spacing);
    layoutPlaybackButtons(bounds.removeFromLeft(playbackWidth));
    bounds.removeFromLeft(layout::sectionSpacing);
    const auto metricsWidth = juce::jlimit(
        layout::compactPreview::metricsMinimumWidth,
        juce::jmax(
            layout::compactPreview::metricsMinimumWidth,
            bounds.getWidth() - layout::compactPreview::minimumWaveformWidth - layout::sectionSpacing
        ),
        juce::roundToInt(bounds.getWidth() * layout::compactPreview::metricsWidthProportion)
    );
    audioInfo.setBounds(bounds.removeFromLeft(metricsWidth));
    bounds.removeFromLeft(layout::sectionSpacing);
    thumbnail.setBounds(bounds);
}

/// Splits a playback row consistently in both workspace presentations.
void PreviewWorkspace::layoutPlaybackButtons(juce::Rectangle<int> bounds)
{
    startStopButton.setBounds(bounds.removeFromLeft((bounds.getWidth() - layout::sectionSpacing) / 2));
    bounds.removeFromLeft(layout::sectionSpacing);
    processButton.setBounds(bounds);
}
}  // namespace audiobatch
