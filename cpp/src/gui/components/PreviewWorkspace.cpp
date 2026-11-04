/// Implementation of PreviewWorkspace:
/// the look of the preview controls, the chevron images of the section toggle,
/// and the expanded, wide, and compact layouts that reuse the same controls.

#include "gui/components/PreviewWorkspace.h"

#include "gui/AudioBatchComponentConstants.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"

#include <functional>
#include <memory>
#include <utility>

namespace audiobatch
{
namespace
{
constexpr auto CONTROLS_WIDTH_KEY {"previewControlsWidthScale"};
constexpr auto INFORMATION_WIDTH_KEY {"previewInformationWidthScale"};
}  // namespace

/// A native JUCE edge resizer whose constrainer updates a responsive width preference, not a fixed component size.
/// Each divider owns only its selected panel's width. The workspace handles the waveform and the untouched sibling.
class PreviewWidthDivider final : public juce::ResizableEdgeComponent, private juce::ComponentBoundsConstrainer
{
public:
    /// Observes an existing panel and adapts JUCE's right-edge dragging to workspace column widths.
    PreviewWidthDivider(
        juce::Component& target,
        const juce::String& name,
        std::function<int()> currentWidth,
        std::function<void(int)> resize,
        std::function<void()> commit,
        std::function<void()> reset
    ) :
        juce::ResizableEdgeComponent(&target, this, juce::ResizableEdgeComponent::rightEdge),
        panel(target),
        getPanelWidth(std::move(currentWidth)),
        onCommit(std::move(commit)),
        onReset(std::move(reset)),
        onResize(std::move(resize))
    {
        setName(name);
        setTitle(name);
        setDescription(
            "Drag left or right, or use arrow keys. Shift moves faster. Home or double-click resets the width."
        );
        setWantsKeyboardFocus(true);
    }

    /// Releases callbacks before the owning workspace destroys their observed panels.
    ~PreviewWidthDivider() override = default;

    PreviewWidthDivider(const PreviewWidthDivider&) = delete;
    PreviewWidthDivider(PreviewWidthDivider&&) = delete;
    PreviewWidthDivider& operator=(const PreviewWidthDivider&) = delete;
    PreviewWidthDivider& operator=(PreviewWidthDivider&&) = delete;

    /// Resizes only the selected column, with Shift providing a larger logical-pixel step and Home resetting it.
    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto code = key.getKeyCode();
        if (code == juce::KeyPress::homeKey) {
            onReset();
            onCommit();
            return true;
        }
        if (code != juce::KeyPress::leftKey && code != juce::KeyPress::rightKey) {
            return false;
        }
        const auto step = key.getModifiers().isShiftDown() ? layout::previewWidths::LARGE_KEYBOARD_STEP
                                                           : layout::previewWidths::KEYBOARD_STEP;
        onResize(getPanelWidth() + (code == juce::KeyPress::leftKey ? -step : step));
        onCommit();
        return true;
    }

    /// Refreshes the grip when keyboard traversal reaches the divider.
    void focusGained(const juce::Component::FocusChangeType cause) override
    {
        juce::ignoreUnused(cause);
        repaint();
    }

    /// Removes the focused grip when traversal leaves the divider.
    void focusLost(const juce::Component::FocusChangeType cause) override
    {
        juce::ignoreUnused(cause);
        repaint();
    }

    /// Resets only this divider's preference, leaving the sibling and the preview height unchanged.
    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        juce::ignoreUnused(event);
        onReset();
        onCommit();
    }

protected:
    /// Shows a quiet vertical grip only when the divider is hovered, focused, or being dragged.
    void paint(juce::Graphics& graphics) override
    {
        if (!isMouseOverOrDragging() && !hasKeyboardFocus(false)) {
            return;
        }
        graphics.setColour(CustomLookAndFeel::greyMiddleLight().withAlpha(layout::previewWidths::INDICATOR_ALPHA));
        auto indicator = getLocalBounds().toFloat().reduced(0.0f, layout::previewWidths::INDICATOR_INSET);
        indicator = indicator.withSizeKeepingCentre(layout::previewWidths::INDICATOR_WIDTH, indicator.getHeight());
        graphics.fillRoundedRectangle(indicator, layout::previewWidths::INDICATOR_WIDTH * 0.5f);
    }

private:
    /// Leaves the component geometry to the workspace so a divider never moves or resizes its sibling panel.
    void applyBoundsToComponent(juce::Component& component, const juce::Rectangle<int> bounds) override
    {
        juce::ignoreUnused(component);
        onResize(bounds.getWidth() + columnInset);
    }

    /// Commits once after JUCE completes a pointer resize, rather than saving every drag sample.
    void resizeEnd() override
    {
        onCommit();
    }

    /// Converts the borrowed control's inset width back to its complete workspace column.
    void resizeStart() override
    {
        columnInset = getPanelWidth() - panel.getWidth();
    }

    /// Borrows a panel owned by the enclosing workspace, which outlives this divider.
    juce::Component& panel;
    std::function<int()> getPanelWidth;
    std::function<void()> onCommit;
    std::function<void()> onReset;
    std::function<void(int)> onResize;
    int columnInset {0};
};

/// Creates the controls and adds them to the workspace, with the playback controls disabled until a file loads.
/// The settings must outlive the workspace, and are read only after the owner configures their storage.
PreviewWorkspace::PreviewWorkspace(AudioPreviewPlayer& previewPlayer, juce::ApplicationProperties& settings) :
    normalizationTargetControl(settings),
    thumbnail(previewPlayer.getFormatManager(), previewPlayer),
    properties(settings)
{
    configurePreviewSectionButton();
    controlsContent.addAndMakeVisible(previewSectionButton);

    controlsViewport.setViewedComponent(&controlsContent, false);
    controlsViewport.setScrollBarsShown(true, false);
    controlsViewport.setScrollBarThickness(layout::scrollBarThickness);
    controlsViewport.setName("Preview and processing controls");
    addAndMakeVisible(controlsViewport);

    addAndMakeVisible(thumbnail);

    setPlaybackState(false, false);
    startStopButton.setTooltip("Start or stop preview playback for the selected file.");
    addAndMakeVisible(startStopButton);

    addAndMakeVisible(audioInfo);

    processButton.setTooltip("Process selected files through the plugin chain.");
    processButton.setColour(juce::TextButton::buttonColourId, CustomLookAndFeel::blue());
    processButton.setColour(juce::TextButton::textColourOffId, CustomLookAndFeel::greyDark());
    addAndMakeVisible(processButton);

    configureSecondaryControls();
    controlsWidthDivider = std::make_unique<PreviewWidthDivider>(
        controlsViewport,
        "Resize preview controls width",
        [this] {
            return layout::previewColumns(
                       getLocalBounds().reduced(layout::compactPreview::padding),
                       true,
                       controlsWidthScale,
                       informationWidthScale
            )
                .controls.getWidth();
        },
        [this](const int width) { setPanelWidth(false, width); },
        [this] { saveWidthSettings(); },
        [this] {
            controlsWidthScale = layout::previewWidths::DEFAULT_CONTROLS_SCALE;
            resized();
        }
    );
    informationWidthDivider = std::make_unique<PreviewWidthDivider>(
        audioInfo,
        "Resize preview information width",
        [this] { return audioInfo.getWidth(); },
        [this](const int width) { setPanelWidth(true, width); },
        [this] { saveWidthSettings(); },
        [this] {
            informationWidthScale = layout::previewWidths::DEFAULT_INFORMATION_SCALE;
            resized();
        }
    );
    addAndMakeVisible(*controlsWidthDivider);
    addAndMakeVisible(*informationWidthDivider);
}

/// Destroys the width dividers before their borrowed panel targets and settings disappear.
PreviewWorkspace::~PreviewWorkspace() = default;

/// Loads responsive width preferences after the owning view has configured application storage.
void PreviewWorkspace::loadWidthSettings()
{
    if (const auto* settings = properties.getUserSettings()) {
        controlsWidthScale = layout::constrainedPreviewWidthScale(
            settings->getDoubleValue(CONTROLS_WIDTH_KEY, layout::previewWidths::DEFAULT_CONTROLS_SCALE), false
        );
        informationWidthScale = layout::constrainedPreviewWidthScale(
            settings->getDoubleValue(INFORMATION_WIDTH_KEY, layout::previewWidths::DEFAULT_INFORMATION_SCALE), true
        );
    }
    resized();
}

/// Writes only the width preferences when the user commits a resize or reset.
void PreviewWorkspace::saveWidthSettings()
{
    if (auto* settings = properties.getUserSettings()) {
        settings->setValue(CONTROLS_WIDTH_KEY, controlsWidthScale);
        settings->setValue(INFORMATION_WIDTH_KEY, informationWidthScale);
        properties.saveIfNeeded();
    }
}

/// Updates only the requested column, preserving its sibling and reserving the waveform's minimum width.
/// Window-size clamping never flows back into either saved factor.
/// Below the application's minimum width, a resize that would disturb its squeezed sibling is ignored.
void PreviewWorkspace::setPanelWidth(const bool information, const int width)
{
    if (isCompact()) {
        return;
    }
    const auto bounds = getLocalBounds().reduced(layout::compactPreview::padding);
    const auto baseline = layout::previewColumnBaseline(bounds);
    const auto columns = layout::previewColumns(bounds, true, controlsWidthScale, informationWidthScale);
    const auto otherWidth = information ? columns.controls.getWidth() : columns.information.getWidth();
    const auto availableWidth = juce::jmax(
        0, bounds.getWidth() - layout::sectionSpacing * 2 - layout::previewWidths::MINIMUM_WAVEFORM_WIDTH - otherWidth
    );
    const auto baselineWidth = information ? baseline.information.getWidth() : baseline.controls.getWidth();
    const auto minimumScale = information ? layout::previewWidths::MINIMUM_INFORMATION_SCALE
                                          : layout::previewWidths::MINIMUM_CONTROLS_SCALE;
    const auto minimumWidth = juce::roundToInt(baselineWidth * minimumScale);
    if (availableWidth < minimumWidth) {
        return;
    }
    const auto scale = width <= minimumWidth
        ? minimumScale
        : layout::constrainedPreviewWidthScale(
              static_cast<double>(juce::jmin(availableWidth, width)) / baselineWidth, information
          );
    const auto proposed = layout::previewColumns(
        bounds, true, information ? controlsWidthScale : scale, information ? scale : informationWidthScale
    );
    const auto proposedOtherWidth = information ? proposed.controls.getWidth() : proposed.information.getWidth();
    if (proposedOtherWidth != otherWidth) {
        return;
    }
    (information ? informationWidthScale : controlsWidthScale) = scale;
    resized();
}

/// Reports whether the workspace shows the compact row, in which the secondary controls are hidden.
bool PreviewWorkspace::isCompact() const noexcept
{
    return !controlsViewport.isVisible();
}

/// Switches between the expanded section and the compact row, hiding the secondary controls in the compact row.
/// The section toggle sits in the top left corner of the controls while expanded, and leads the compact row.
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
    if (shouldBeCompact
        && (controlsWidthDivider->hasKeyboardFocus(false) || informationWidthDivider->hasKeyboardFocus(false)))
    {
        previewSectionButton.grabKeyboardFocus();
    }
    audioInfo.setCompact(shouldBeCompact);
    resized();
}

/// Shows the playback state on the play button:
/// a red "Stop" while playing, a green "Play" while stopped, and a grey "Play" while no file is loaded.
void PreviewWorkspace::setPlaybackState(const bool enabled, const bool playing)
{
    const bool showStop = enabled && playing;
    startStopButton.setEnabled(enabled);
    startStopButton.setButtonText(showStop ? "Stop" : "Play");
    auto colour = CustomLookAndFeel::greyMedium();
    if (enabled) {
        colour = playing ? CustomLookAndFeel::red() : CustomLookAndFeel::green();
    }
    startStopButton.setColour(juce::TextButton::buttonColourId, colour);
}

/// Lays out the metrics, the waveform, and the controls in columns,
/// or in one row with the toggle, the playback buttons, the metrics, and the waveform when compact.
/// A wide workspace selects wide layouts before applying the independently remembered width scales.
/// The controls and the pinned playback row share the same margins, so the column is centred,
/// and the scroll bar takes width only while the controls overflow the column.
void PreviewWorkspace::resized()
{
    auto workspace = getLocalBounds().reduced(layout::compactPreview::padding);
    workspace.setSize(juce::jmax(0, workspace.getWidth()), juce::jmax(0, workspace.getHeight()));
    const auto columns = layout::previewColumns(workspace, !isCompact(), controlsWidthScale, informationWidthScale);
    controlsWidthDivider->setVisible(!isCompact());
    informationWidthDivider->setVisible(!isCompact());
    if (workspace.isEmpty()) {
        return;
    }
    if (isCompact()) {
        layoutCompact(
            workspace, columns.controls.getWidth() - layout::previewToggle::size - layout::compactPreview::spacing
        );
        return;
    }

    controlsWidthDivider->setBounds(
        columns.controls.getRight(),
        workspace.getY(),
        juce::jmax(0, columns.information.getX() - columns.controls.getRight()),
        workspace.getHeight()
    );
    informationWidthDivider->setBounds(
        columns.information.getRight(),
        workspace.getY(),
        juce::jmax(0, columns.waveform.getX() - columns.information.getRight()),
        workspace.getHeight()
    );
    audioInfo.setWide(columns.wide);
    audioInfo.setBounds(columns.information);
    thumbnail.setBounds(columns.waveform);
    auto controls
        = columns.controls.reduced(layout::information::horizontalPadding, layout::information::verticalPadding);
    layoutPlaybackButtons(controls.removeFromBottom(layout::processButtonHeight));
    controls.removeFromBottom(layout::controlsSectionSpacing);
    controlsViewport.setBounds(controls);
    const auto contentHeight = columns.wide ? layout::wideControlsContentHeight : layout::controlsContentHeight;
    const bool overflows = contentHeight > controls.getHeight();
    controlsContent.setSize(
        juce::jmax(0, controls.getWidth() - (overflows ? layout::scrollBarThickness : 0)),
        juce::jmax(controls.getHeight(), contentHeight)
    );
    previewChainToggle.setVisible(columns.wide);
    if (columns.wide) {
        layoutWideControls(controlsContent.getLocalBounds());
    } else {
        layoutNarrowControls(controlsContent.getLocalBounds());
    }
}

/// Lays out the zoom and gain sliders across the given width, each below its caption,
/// with the section toggle in the top left corner, before the Zoom caption.
/// Returns the area below them.
juce::Rectangle<int> PreviewWorkspace::layoutSliders(juce::Rectangle<int> content)
{
    auto zoomRow = content.removeFromTop(layout::sliderRowHeight);
    previewSectionButton.setBounds(
        zoomRow.removeFromLeft(layout::previewToggle::size)
            .withSizeKeepingCentre(layout::previewToggle::size, layout::previewToggle::size)
    );
    zoomLabel.setBounds(zoomRow);
    zoomSlider.setBounds(content.removeFromTop(layout::sliderRowHeight));
    content.removeFromTop(layout::controlsSectionSpacing);
    gainLabel.setBounds(content.removeFromTop(layout::sliderRowHeight));
    gainSlider.setBounds(content.removeFromTop(layout::sliderRowHeight));
    content.removeFromTop(layout::controlsSectionSpacing);
    return content;
}

/// Lays out the controls in one column below the sliders.
void PreviewWorkspace::layoutNarrowControls(juce::Rectangle<int> content)
{
    content = layoutSliders(content);
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

/// Lays out the sliders across the full width, and below them two columns of three rows as tall as a button.
/// The left column has the toggle that plays the preview through the plugin chain,
/// in line with the normalization toggles of the right column, and the plugin chain and settings buttons.
/// The right column has the normalization toggles and the normalization target over two rows.
void PreviewWorkspace::layoutWideControls(juce::Rectangle<int> content)
{
    content = layoutSliders(content);
    auto left = content.removeFromLeft((content.getWidth() - layout::widePreview::columnSpacing) / 2);
    content.removeFromLeft(layout::widePreview::columnSpacing);
    auto right = content;

    previewChainToggle.setBounds(left.removeFromTop(layout::controlButtonHeight));
    left.removeFromTop(layout::controlsSectionSpacing);
    pluginButton.setBounds(left.removeFromTop(layout::controlButtonHeight));
    left.removeFromTop(layout::controlsSectionSpacing);
    settingsButton.setBounds(left.removeFromTop(layout::controlButtonHeight));

    auto normalizeRow = right.removeFromTop(layout::controlButtonHeight);
    normalizeBeforePluginToggle.setBounds(
        normalizeRow.removeFromLeft((normalizeRow.getWidth() - layout::sectionSpacing) / 2)
    );
    normalizeRow.removeFromLeft(layout::sectionSpacing);
    forceOverwriteToggle.setBounds(normalizeRow);
    right.removeFromTop(layout::controlsSectionSpacing);
    normalizationTargetControl.setBounds(right.removeFromTop(layout::normalizationTargetHeight));
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
/// the zoom and gain sliders, the processing toggles, the preview chain toggle of the wide layout,
/// and the plugin chain and settings buttons.
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

    previewChainToggle.setTooltip("When enabled, the preview plays through the plugin chain.");
    controlsContent.addChildComponent(previewChainToggle);

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
