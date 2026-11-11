/// Implements ImageControlLookAndFeel: the AudioBatch colour scheme and control colours,
/// and the Roboto fonts embedded from the repository's shared assets.

#include "ui/ImageControlLookAndFeel.h"

#include "ImageControlBinaryData.h"
#include "ui/ControlSlider.h"
#include "ui/DisplayDisclosure.h"

#include <algorithm>
#include <memory>

namespace esgrove::image_control
{
namespace
{
/// Uses JUCE's managed focus overlay so sliders, toggles, and drawable buttons share one restrained treatment.
class ControlFocusOutline final : public juce::FocusOutline::OutlineWindowProperties
{
public:
    /// Keeps the overlay inside the control so callouts and adjacent rows do not clip its border.
    juce::Rectangle<int> getOutlineBounds(juce::Component& component) override
    {
        return component.getScreenBounds();
    }

    /// Draws a thin accent outline without changing the ordinary hover and selected states.
    void drawOutline(juce::Graphics& graphics, const int width, const int height) override
    {
        constexpr float thickness = 1.0F;
        constexpr float opacity = 0.7F;
        graphics.setColour(juce::Colour(palette::BLUE).withAlpha(opacity));
        graphics.drawRoundedRectangle(
            juce::Rectangle<float>(static_cast<float>(width), static_cast<float>(height)).reduced(thickness * 0.5F),
            corner_radius::CONTROL,
            thickness
        );
    }
};

/// Opacity of the slider track fill, as in AudioBatch.
constexpr float SLIDER_TRACK_ALPHA {0.7F};
/// Opacity of the text of a text button that is off, as in AudioBatch.
constexpr float TEXT_BUTTON_OFF_TEXT_ALPHA {0.96F};
/// Opacity of the text editor selection, as in AudioBatch.
constexpr float TEXT_EDITOR_HIGHLIGHT_ALPHA {0.24F};
/// Largest font height of a text button.
constexpr float MAX_BUTTON_FONT_HEIGHT {15.0F};
/// Font height of a text button relative to its height.
constexpr float BUTTON_FONT_HEIGHT_RATIO {0.55F};
/// Shared height of alert messages, field labels, and action text in logical component units.
constexpr float ALERT_FONT_HEIGHT {13.0F};
/// Height of the bold alert heading in logical component units.
constexpr float ALERT_TITLE_FONT_HEIGHT {16.0F};
/// Compact action height shared with the plugin's control rows.
constexpr int ALERT_BUTTON_HEIGHT {28};
/// Icon reservation used by JUCE's AlertWindow::updateLayout for every non-empty icon type.
constexpr int ALERT_ICON_SPACE {80};
/// Restrained symbol size inside JUCE's reserved icon column, in logical component units.
constexpr float ALERT_ICON_SIZE {24.0F};

/// Returns the embedded Roboto typeface, created on first use.
const juce::Typeface::Ptr& textTypeface()
{
    static const juce::Typeface::Ptr typeface = juce::Typeface::createSystemTypefaceFor(
        ImageControlBinaryData::RobotoRegular_ttf, ImageControlBinaryData::RobotoRegular_ttfSize
    );
    return typeface;
}

/// Returns the embedded Roboto SemiBold face without host font matching or synthetic weight.
const juce::Typeface::Ptr& semiboldTypeface()
{
    static const juce::Typeface::Ptr typeface = juce::Typeface::createSystemTypefaceFor(
        ImageControlBinaryData::RobotoSemiBold_ttf, ImageControlBinaryData::RobotoSemiBold_ttfSize
    );
    return typeface;
}

/// Returns the embedded Roboto Mono typeface, created on first use.
const juce::Typeface::Ptr& monoTypeface()
{
    static const juce::Typeface::Ptr typeface = juce::Typeface::createSystemTypefaceFor(
        ImageControlBinaryData::RobotoMonoRegular_ttf, ImageControlBinaryData::RobotoMonoRegular_ttfSize
    );
    return typeface;
}

/// Returns a colour of the palette.
juce::Colour colour(const juce::uint32 argb)
{
    return juce::Colour(argb);
}
}  // namespace

/// Applies AudioBatch's dark colour scheme, its slider, button, label, and text editor colours,
/// and Roboto as the default typeface.
ImageControlLookAndFeel::ImageControlLookAndFeel()
{
    setColourScheme({
        // windowBackground
        palette::GREY_DARK,
        // widgetBackground
        palette::GREY_SEMI_DARK,
        // menuBackground
        palette::GREY_MEDIUM_DARK,
        // outline
        palette::GREY_MEDIUM,
        // defaultText
        palette::GREY_LIGHT,
        // defaultFill
        palette::GREY_MIDDLE,
        // highlightedText
        palette::GREY_SUPER_LIGHT,
        // highlightedFill
        palette::BLUE,
        // menuText
        palette::GREY_LIGHT,
    });

    setColour(juce::AlertWindow::backgroundColourId, colour(palette::GREY_SEMI_DARK));
    setColour(juce::AlertWindow::textColourId, colour(palette::GREY_SUPER_LIGHT));
    setColour(juce::AlertWindow::outlineColourId, colour(palette::GREY_MEDIUM));
    setColour(juce::ResizableWindow::backgroundColourId, colour(palette::GREY_DARK));
    setColour(juce::TextButton::buttonColourId, colour(palette::GREY_MEDIUM));
    setColour(juce::TextButton::buttonOnColourId, colour(palette::BLUE));
    setColour(
        juce::TextButton::textColourOffId, colour(palette::GREY_SUPER_LIGHT).withAlpha(TEXT_BUTTON_OFF_TEXT_ALPHA)
    );
    setColour(juce::TextButton::textColourOnId, colour(palette::GREY_SUPER_LIGHT));
    setColour(juce::Slider::backgroundColourId, colour(palette::GREY_MEDIUM));
    setColour(juce::Slider::trackColourId, colour(palette::BLUE).withAlpha(SLIDER_TRACK_ALPHA));
    setColour(juce::Slider::thumbColourId, colour(palette::GREY_SUPER_LIGHT));
    setColour(juce::Slider::textBoxBackgroundColourId, colour(palette::GREY_MEDIUM_DARK));
    setColour(juce::Slider::textBoxTextColourId, colour(palette::GREY_SUPER_LIGHT));
    setColour(juce::Slider::textBoxOutlineColourId, colour(palette::GREY_MEDIUM));
    setColour(juce::Label::textColourId, colour(palette::GREY_LIGHT));
    setColour(juce::ToggleButton::textColourId, colour(palette::GREY_LIGHT));
    setColour(juce::ToggleButton::tickColourId, colour(palette::BLUE));
    setColour(juce::ToggleButton::tickDisabledColourId, colour(palette::GREY_MIDDLE));
    setColour(juce::TextEditor::backgroundColourId, colour(palette::GREY_SEMI_DARK));
    setColour(juce::TextEditor::highlightColourId, colour(palette::BLUE).withAlpha(TEXT_EDITOR_HIGHLIGHT_ALPHA));
    setColour(juce::TextEditor::outlineColourId, colour(palette::GREY_MEDIUM));
    setColour(juce::CaretComponent::caretColourId, colour(palette::BLUE));
    setColour(juce::TooltipWindow::backgroundColourId, colour(palette::GREY_MEDIUM_DARK));
    setColour(juce::TooltipWindow::textColourId, colour(palette::GREY_SUPER_LIGHT));

    setDefaultSansSerifTypeface(textTypeface());
    juce::Desktop::getInstance().addFocusChangeListener(this);
}

/// Detaches the desktop observer before JUCE releases this editor-owned look and feel.
ImageControlLookAndFeel::~ImageControlLookAndFeel()
{
    juce::Desktop::getInstance().removeFocusChangeListener(this);
}

/// Opts ordinary JUCE buttons, including drawables and toggles, into the shared focus outline.
/// JUCE notifies these listeners before creating its managed outline, so no component-specific paint hooks are needed.
void ImageControlLookAndFeel::globalFocusChanged(juce::Component* focusedComponent)
{
    if (focusedComponent != nullptr && &focusedComponent->getLookAndFeel() == this
        && dynamic_cast<DisplayDisclosure*>(focusedComponent) == nullptr
        && (dynamic_cast<juce::Button*>(focusedComponent) != nullptr
            || dynamic_cast<juce::Slider*>(focusedComponent) != nullptr
            || dynamic_cast<juce::ComboBox*>(focusedComponent) != nullptr))
    {
        focusedComponent->setHasFocusOutline(true);
    }
}

/// Supplies the same subdued outline to every JUCE control that opts into keyboard focus visibility.
std::unique_ptr<juce::FocusOutline> ImageControlLookAndFeel::createFocusOutlineForComponent(juce::Component& component)
{
    if (dynamic_cast<DisplayDisclosure*>(&component) != nullptr) {
        return nullptr;
    }
    return std::make_unique<juce::FocusOutline>(std::make_unique<ControlFocusOutline>());
}

/// Gives plugin sliders a stable numeric-only editor while leaving unrelated JUCE sliders unchanged.
juce::Label* ImageControlLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    if (auto* control = dynamic_cast<ControlSlider*>(&slider)) {
        return control->createValueBox().release();
    }
    return juce::LookAndFeel_V4::createSliderTextBox(slider);
}

/// Returns Roboto Mono at a height.
juce::Font monoFont(const float height)
{
    return juce::Font(juce::FontOptions(monoTypeface())).withHeight(height);
}

/// Returns the embedded Roboto SemiBold at an explicit height without synthesizing a heavier regular face.
juce::Font semiboldTextFont(const float height)
{
    return juce::Font(juce::FontOptions(semiboldTypeface())).withHeight(height);
}

/// Returns Roboto at a height.
juce::Font textFont(const float height)
{
    return juce::Font(juce::FontOptions(textTypeface())).withHeight(height);
}

/// Uses Roboto Mono for the value boxes of sliders, so their numbers line up, and Roboto for other labels.
juce::Font ImageControlLookAndFeel::getLabelFont(juce::Label& label)
{
    const float height = label.getFont().getHeight();
    if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr) {
        return monoFont(height);
    }
    return label.getFont();
}

/// Keeps alert actions at the dialog's control font height and scales ordinary editor buttons to their bounds.
juce::Font ImageControlLookAndFeel::getTextButtonFont(juce::TextButton& button, const int buttonHeight)
{
    if (button.findParentComponentOfClass<juce::AlertWindow>() != nullptr) {
        return getAlertWindowFont();
    }
    return textFont(std::min(MAX_BUTTON_FONT_HEIGHT, static_cast<float>(buttonHeight) * BUTTON_FONT_HEIGHT_RATIO));
}

/// Uses the embedded Roboto for alert field labels and other dialog controls on the message thread.
juce::Font ImageControlLookAndFeel::getAlertWindowFont()
{
    return textFont(ALERT_FONT_HEIGHT);
}

/// Uses the same compact Roboto size when JUCE measures and paints alert messages and text editors.
juce::Font ImageControlLookAndFeel::getAlertWindowMessageFont()
{
    return textFont(ALERT_FONT_HEIGHT);
}

/// Gives alert headings a restrained bold Roboto hierarchy without inheriting platform title fonts.
juce::Font ImageControlLookAndFeel::getAlertWindowTitleFont()
{
    return textFont(ALERT_TITLE_FONT_HEIGHT).boldened();
}

/// Keeps dialog actions consistent with the plugin's compact control rows.
int ImageControlLookAndFeel::getAlertWindowButtonHeight()
{
    return ALERT_BUTTON_HEIGHT;
}

/// Paints the plugin panel and a small semantic icon on the message thread, preserving JUCE's measured text layout.
/// The supplied text area includes the icon column and child controls, so only the measured text height is painted.
void ImageControlLookAndFeel::drawAlertBox(
    juce::Graphics& graphics,
    juce::AlertWindow& alert,
    const juce::Rectangle<int>& textArea,
    juce::TextLayout& textLayout
)
{
    constexpr float outlineThickness = 1.0F;
    const auto panel = alert.getLocalBounds().toFloat().reduced(outlineThickness * 0.5F);
    graphics.setColour(alert.findColour(juce::AlertWindow::backgroundColourId));
    graphics.fillRoundedRectangle(panel, corner_radius::PANEL);
    graphics.setColour(alert.findColour(juce::AlertWindow::outlineColourId));
    graphics.drawRoundedRectangle(panel, corner_radius::PANEL, outlineThickness);

    auto contentArea = textArea.getIntersection(alert.getLocalBounds()).toFloat();
    if (alert.getAlertType() != juce::MessageBoxIconType::NoIcon) {
        const auto iconArea
            = contentArea.removeFromLeft(std::min(static_cast<float>(ALERT_ICON_SPACE), contentArea.getWidth()));
        const float iconSize = std::min({ALERT_ICON_SIZE, iconArea.getWidth(), iconArea.getHeight()});
        const auto iconBounds = juce::Rectangle<float>(iconSize, iconSize)
                                    .withCentre({iconArea.getCentreX(), iconArea.getY() + iconSize * 0.5F});
        const bool isWarning = alert.getAlertType() == juce::MessageBoxIconType::WarningIcon;
        juce::Path icon;
        if (isWarning) {
            icon.addTriangle(
                iconBounds.getCentreX(),
                iconBounds.getY(),
                iconBounds.getRight(),
                iconBounds.getBottom(),
                iconBounds.getX(),
                iconBounds.getBottom()
            );
            icon = icon.createPathWithRoundedCorners(2.0F);
        } else {
            icon.addEllipse(iconBounds);
        }
        graphics.setColour(colour(isWarning ? palette::YELLOW : palette::BLUE));
        graphics.strokePath(icon, juce::PathStrokeType(outlineThickness));
        graphics.setFont(getAlertWindowTitleFont());
        const juce::String symbol = isWarning ? "!" : "?";
        graphics.drawText(
            alert.getAlertType() == juce::MessageBoxIconType::InfoIcon ? "i" : symbol,
            iconBounds,
            juce::Justification::centred,
            false
        );
    }

    const juce::Graphics::ScopedSaveState savedState(graphics);
    contentArea.setHeight(std::min(contentArea.getHeight(), textLayout.getHeight()));
    graphics.reduceClipRegion(contentArea.getSmallestIntegerContainer());
    graphics.setColour(alert.findColour(juce::AlertWindow::textColourId));
    textLayout.draw(graphics, contentArea);
}

/// Rounds the native Settings bubble while retaining JUCE's arrow, shadow, placement, and modal behavior.
float ImageControlLookAndFeel::getCallOutBoxCornerSize(const juce::CallOutBox& /*box*/)
{
    return corner_radius::CALLOUT;
}

/// Paints modestly rounded buttons with the usual enabled, hover, down, and connected-edge treatments.
void ImageControlLookAndFeel::drawButtonBackground(
    juce::Graphics& graphics,
    juce::Button& button,
    const juce::Colour& background,
    const bool highlighted,
    const bool down
)
{
    constexpr float outlineThickness = 1.0F;
    constexpr float focusedSaturation = 1.3F;
    constexpr float restingSaturation = 0.9F;
    constexpr float pressedContrast = 0.2F;
    constexpr float hoverContrast = 0.05F;
    const auto bounds = button.getLocalBounds().toFloat().reduced(outlineThickness * 0.5F);
    auto base
        = background.withMultipliedSaturation(button.hasKeyboardFocus(true) ? focusedSaturation : restingSaturation)
              .withMultipliedAlpha(button.isEnabled() ? 1.0F : 0.5F);
    if (down || highlighted) {
        base = base.contrasting(down ? pressedContrast : hoverContrast);
    }
    const bool flatLeft = button.isConnectedOnLeft();
    const bool flatRight = button.isConnectedOnRight();
    const bool flatTop = button.isConnectedOnTop();
    const bool flatBottom = button.isConnectedOnBottom();
    juce::Path outline;
    outline.addRoundedRectangle(
        bounds.getX(),
        bounds.getY(),
        bounds.getWidth(),
        bounds.getHeight(),
        corner_radius::BUTTON,
        corner_radius::BUTTON,
        !(flatLeft || flatTop),
        !(flatRight || flatTop),
        !(flatLeft || flatBottom),
        !(flatRight || flatBottom)
    );
    graphics.setColour(base);
    graphics.fillPath(outline);
    graphics.setColour(button.findColour(juce::ComboBox::outlineColourId));
    graphics.strokePath(outline, juce::PathStrokeType(outlineThickness));
}

/// Rounds selector surfaces without changing their label placement, arrow geometry, or enabled-state feedback.
void ImageControlLookAndFeel::drawComboBox(
    juce::Graphics& graphics,
    const int width,
    const int height,
    const bool /*down*/,
    const int /*buttonX*/,
    const int /*buttonY*/,
    const int /*buttonWidth*/,
    const int /*buttonHeight*/,
    juce::ComboBox& box
)
{
    constexpr float outlineThickness = 1.0F;
    const auto bounds = juce::Rectangle<int>(width, height).toFloat();
    const float radius
        = box.findParentComponentOfClass<juce::ChoicePropertyComponent>() != nullptr ? 0.0F : corner_radius::CONTROL;
    graphics.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    graphics.fillRoundedRectangle(bounds, radius);
    graphics.setColour(box.findColour(juce::ComboBox::outlineColourId));
    graphics.drawRoundedRectangle(bounds.reduced(outlineThickness * 0.5F), radius, outlineThickness);
    constexpr int arrowRightInset = 30;
    constexpr int arrowWidth = 20;
    constexpr float arrowInset = 3.0F;
    constexpr float arrowTopOffset = 2.0F;
    const auto arrowZone = juce::Rectangle<int>(width - arrowRightInset, 0, arrowWidth, height).toFloat();
    juce::Path arrow;
    arrow.startNewSubPath(arrowZone.getX() + arrowInset, arrowZone.getCentreY() - arrowTopOffset);
    arrow.lineTo(arrowZone.getCentreX(), arrowZone.getCentreY() + arrowInset);
    arrow.lineTo(arrowZone.getRight() - arrowInset, arrowZone.getCentreY() - arrowTopOffset);
    constexpr float enabledArrowAlpha = 0.9F;
    constexpr float disabledArrowAlpha = 0.2F;
    graphics.setColour(box.findColour(juce::ComboBox::arrowColourId)
                           .withAlpha(box.isEnabled() ? enabledArrowAlpha : disabledArrowAlpha));
    graphics.strokePath(arrow, juce::PathStrokeType(2.0F));
}
}  // namespace esgrove::image_control
