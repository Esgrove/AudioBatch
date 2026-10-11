/// Implementation of the CustomLookAndFeel styling.
/// Defines the shared colour palette constants and the bundled Roboto fonts,
/// the CustomDocumentWindowButton used for title bar buttons,
/// and the drawing code for the title bar, tooltips, and popup menu items.

#include "CustomLookAndFeel.h"

#include <cmath>
#include <utility>

namespace
{
/// Alpha of a title bar button glyph while the button is disabled or pressed.
constexpr float dimmedGlyphAlpha {0.6f};

/// Opacity of the title bar icon while the window is inactive.
constexpr float inactiveTitleIconOpacity {0.6f};

/// Alpha of the combo box outline while the combo box has keyboard focus.
constexpr float comboBoxFocusedOutlineAlpha {0.45f};

/// Alpha of the hyperlink button text colour.
constexpr float hyperlinkTextAlpha {0.9f};

/// Alpha of the highlighted popup menu item background.
constexpr float popupMenuHighlightAlpha {0.18f};

/// Alpha of the text button label while the button is toggled off.
constexpr float textButtonOffTextAlpha {0.96f};

/// Alpha of the linear slider track fill.
constexpr float sliderTrackAlpha {0.7f};

/// Alpha of the rotary slider value arc.
constexpr float rotarySliderFillAlpha {0.72f};

/// Alpha of the text editor selection highlight.
constexpr float textEditorHighlightAlpha {0.24f};

/// Inset of the inner rectangle of the fullscreen glyph, in the glyph's 100 by 100 path units.
constexpr float fullscreenGlyphInset {45.0f};

/// Stroke thickness of the fullscreen glyph, in the glyph's 100 by 100 path units.
constexpr float fullscreenGlyphStrokeThickness {30.0f};

/// Horizontal inset of popup menu separator lines, in pixels.
constexpr int popupMenuSeparatorInset {5};

/// Alpha of popup menu separator lines relative to the menu text colour.
constexpr float popupMenuSeparatorAlpha {0.3f};

/// Upper limit of the horizontal padding of popup menu items, in pixels.
constexpr int popupMenuItemMaximumPadding {5};

/// Popup menu item width divided by this gives the horizontal padding, up to the maximum padding.
constexpr int popupMenuItemPaddingDivisor {20};

/// Icon area width divided by this gives the horizontal inset of the tick mark.
constexpr int popupMenuTickInsetDivisor {5};

/// Width of the submenu arrow relative to its height.
constexpr float subMenuArrowWidthRatio {0.6f};

/// Height of the shortcut key text relative to the menu item font height.
constexpr float shortcutFontHeightRatio {0.75f};

/// Horizontal scale of the shortcut key text, which slightly condenses it.
constexpr float shortcutFontHorizontalScale {0.95f};

/// Upper limit of the text button label font height, in points.
constexpr float maximumButtonFontHeight {16.0f};

/// Text button label font height relative to the button height.
constexpr float buttonFontHeightRatio {0.6f};
}  // namespace

namespace juce
{

const juce::Font& CustomLookAndFeel::getTextFont()
{
    static const Font font {FontOptions().withTypeface(
        Typeface::createSystemTypefaceFor(BinaryData::RobotoRegular_ttf, BinaryData::RobotoRegular_ttfSize)
    )};
    return font;
}

const juce::Font& CustomLookAndFeel::getMonoBaseFont()
{
    static const Font font {FontOptions().withTypeface(
        Typeface::createSystemTypefaceFor(BinaryData::RobotoMonoRegular_ttf, BinaryData::RobotoMonoRegular_ttfSize)
    )};
    return font;
}

/// Title bar button (close, minimise, maximise) drawn with the project palette.
/// Created by createDocumentWindowButton, ownership passes to the DocumentWindow.
class CustomDocumentWindowButton : public Button
{
public:
    /// Stores the glyph paths and colours for one title bar button.
    /// The toggled shape is drawn when the button is in its toggled state,
    /// which the maximise button uses for the fullscreen glyph.
    CustomDocumentWindowButton(
        const String& name,
        const Colour buttonColour,
        Path normal,
        Path toggled,
        const bool darkMode
    ) :
        Button(name),
        colour(buttonColour),
        normalShape(std::move(normal)),
        toggledShape(std::move(toggled)),
        useDarkMode(darkMode)
    { }

    ~CustomDocumentWindowButton() override = default;

    CustomDocumentWindowButton(const CustomDocumentWindowButton&) = delete;
    CustomDocumentWindowButton& operator=(const CustomDocumentWindowButton&) = delete;
    CustomDocumentWindowButton(CustomDocumentWindowButton&&) = delete;
    CustomDocumentWindowButton& operator=(CustomDocumentWindowButton&&) = delete;

protected:
    /// Fills the button background for the current hover and pressed state and draws the glyph path.
    /// The close button highlights in orange, the others use a neutral grey.
    void paintButton(
        Graphics& graphics,
        const bool shouldDrawButtonAsHighlighted,
        const bool shouldDrawButtonAsDown
    ) override
    {
        const auto background = useDarkMode ? CustomLookAndFeel::greyDark() : CustomLookAndFeel::greyLight();
        auto glyphColour = colour;

        graphics.fillAll(background);

        if (!isEnabled() || shouldDrawButtonAsDown) {
            glyphColour = colour.withAlpha(dimmedGlyphAlpha);
        }

        if (shouldDrawButtonAsHighlighted) {
            if (getName().equalsIgnoreCase("close")) {
                graphics.fillAll(CustomLookAndFeel::orange());
            } else {
                graphics.fillAll(CustomLookAndFeel::greyMediumDark());
            }

            glyphColour = CustomLookAndFeel::greySuperLight();
        }

        graphics.setColour(glyphColour);

        const auto& glyphPath = getToggleState() ? toggledShape : normalShape;
        const auto reducedRect = Justification(Justification::centred)
                                     .appliedToRectangle(Rectangle(getHeight(), getHeight()), getLocalBounds())
                                     .toFloat()
                                     .reduced(static_cast<float>(getHeight()) * 0.3f);

        graphics.fillPath(glyphPath, glyphPath.getTransformToScaleToFit(reducedRect, true));
    }

private:
    Colour colour;
    Path normalShape;
    Path toggledShape;
    bool useDarkMode {true};
    JUCE_LEAK_DETECTOR(CustomDocumentWindowButton)
};

CustomLookAndFeel::CustomLookAndFeel(const bool darkModeEnabled) : darkTheme(darkModeEnabled)
{
    setColourScheme(customColourScheme);

    setColour(juce::ComboBox::arrowColourId, blue());
    setColour(juce::ComboBox::backgroundColourId, greySemiDark());
    setColour(juce::ComboBox::focusedOutlineColourId, blue().withAlpha(comboBoxFocusedOutlineAlpha));
    setColour(juce::ComboBox::outlineColourId, greyMedium());
    setColour(juce::HyperlinkButton::textColourId, blue().withAlpha(hyperlinkTextAlpha));
    setColour(juce::PopupMenu::backgroundColourId, greySemiDark());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, blue().withAlpha(popupMenuHighlightAlpha));
    setColour(juce::PopupMenu::highlightedTextColourId, greySuperLight());
    setColour(juce::PopupMenu::textColourId, greyLight());
    setColour(juce::ResizableWindow::backgroundColourId, greyDark());
    setColour(juce::TextButton::buttonColourId, greyMedium());
    setColour(juce::TextButton::buttonOnColourId, green());
    setColour(juce::TextButton::textColourOffId, greySuperLight().withAlpha(textButtonOffTextAlpha));
    setColour(juce::TextButton::textColourOnId, greySuperLight());
    setColour(juce::Slider::backgroundColourId, greyMedium());
    setColour(juce::Slider::trackColourId, blue().withAlpha(sliderTrackAlpha));
    setColour(juce::Slider::thumbColourId, greySuperLight());
    setColour(juce::Slider::rotarySliderFillColourId, blue().withAlpha(rotarySliderFillAlpha));
    setColour(juce::Slider::rotarySliderOutlineColourId, greyMedium());
    setColour(juce::Slider::textBoxBackgroundColourId, greyMediumDark());
    setColour(juce::Slider::textBoxTextColourId, greySuperLight());
    setColour(juce::Slider::textBoxOutlineColourId, greyMedium());
    setColour(juce::TextEditor::backgroundColourId, greySemiDark());
    setColour(juce::TextEditor::highlightColourId, blue().withAlpha(textEditorHighlightAlpha));
    setColour(juce::TextEditor::outlineColourId, greyMedium());
    setColour(juce::TooltipWindow::backgroundColourId, greyMediumDark());
    setColour(juce::TooltipWindow::textColourId, greySuperLight());
    setColour(juce::AlertWindow::backgroundColourId, greySemiDark());
    setColour(juce::CaretComponent::caretColourId, blue());
    setColour(juce::DialogWindow::backgroundColourId, greySemiDark());

    setDefaultSansSerifTypeface(getTextFont().getTypefacePtr());
    setUsingNativeAlertWindows(true);
}

Button* CustomLookAndFeel::createDocumentWindowButton(const int buttonType)
{
    Path shape;
    const auto crossThickness = 0.15f;

    if (buttonType == DocumentWindow::closeButton) {
        shape.addLineSegment({0.0f, 0.0f, 1.0f, 1.0f}, crossThickness);
        shape.addLineSegment({1.0f, 0.0f, 0.0f, 1.0f}, crossThickness);

        return new CustomDocumentWindowButton("close", CustomLookAndFeel::greyMiddleLight(), shape, shape, darkTheme);
    }

    if (buttonType == DocumentWindow::minimiseButton) {
        shape.addLineSegment({0.0f, 0.5f, 1.0f, 0.5f}, crossThickness);

        return new CustomDocumentWindowButton(
            "minimise", CustomLookAndFeel::greyMiddleLight(), shape, shape, darkTheme
        );
    }

    if (buttonType == DocumentWindow::maximiseButton) {
        shape.addLineSegment({0.5f, 0.0f, 0.5f, 1.0f}, crossThickness);
        shape.addLineSegment({0.0f, 0.5f, 1.0f, 0.5f}, crossThickness);

        Path fullscreenShape;
        fullscreenShape.startNewSubPath(fullscreenGlyphInset, 100.0f);
        fullscreenShape.lineTo(0.0f, 100.0f);
        fullscreenShape.lineTo(0.0f, 0.0f);
        fullscreenShape.lineTo(100.0f, 0.0f);
        fullscreenShape.lineTo(100.0f, fullscreenGlyphInset);
        fullscreenShape.addRectangle(fullscreenGlyphInset, fullscreenGlyphInset, 100.0f, 100.0f);
        PathStrokeType(fullscreenGlyphStrokeThickness).createStrokedPath(fullscreenShape, fullscreenShape);

        return new CustomDocumentWindowButton(
            "maximise", CustomLookAndFeel::greyMiddleLight(), shape, fullscreenShape, darkTheme
        );
    }

    jassertfalse;
    return nullptr;
}

void CustomLookAndFeel::drawDocumentWindowTitleBar(
    DocumentWindow& window,
    Graphics& graphics,
    const int width,
    const int height,
    const int titleSpaceX,
    const int titleSpaceWidth,
    const Image* icon,
    const bool drawTitleTextOnLeft
)
{
    if (width * height == 0) {
        return;
    }

    const auto isActive = window.isActiveWindow();

    const auto background = darkTheme ? greyDark() : greyLight();
    const auto text = darkTheme ? greyLight() : greyDark();

    graphics.setColour(background);
    graphics.fillAll();

    const auto font = getTextFont().withHeight(static_cast<float>(height) * 0.54f);
    graphics.setFont(font);

    const auto textWidth = juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(font, window.getName())));
    auto iconWidth = 0;
    auto iconHeight = 0;

    if (icon != nullptr) {
        iconHeight = juce::roundToIntAccurate(static_cast<double>(font.getHeight()));
        iconWidth = icon->getWidth() / icon->getHeight() * iconHeight + 4;
    }

    const auto contentWidth = jmin(titleSpaceWidth, textWidth + iconWidth);
    auto textX = drawTitleTextOnLeft ? titleSpaceX : jmax(titleSpaceX, (width - contentWidth) / 2);

    if (textX + contentWidth > titleSpaceX + titleSpaceWidth) {
        textX = titleSpaceX + titleSpaceWidth - contentWidth;
    }

    if (icon != nullptr) {
        graphics.setOpacity(isActive ? 1.0f : inactiveTitleIconOpacity);
        graphics.drawImageWithin(
            *icon, textX, (height - iconHeight) / 2, iconWidth, iconHeight, RectanglePlacement::centred, false
        );
        textX += iconWidth;
    }

    textX += 4;
    const auto availableTextWidth = jmax(0, titleSpaceX + titleSpaceWidth - textX);
    graphics.setColour(text);
    graphics.drawText(window.getName(), textX, 0, availableTextWidth, height, Justification::centredLeft, true);
}

void CustomLookAndFeel::drawPopupMenuItem(
    Graphics& graphics,
    const Rectangle<int>& area,
    const bool isSeparator,
    const bool isActive,
    const bool isHighlighted,
    const bool isTicked,
    const bool hasSubMenu,
    const String& text,
    const String& shortcutKeyText,
    const Drawable* icon,
    const Colour* textColourToUse
)
{
    if (isSeparator) {
        auto separatorArea = area.reduced(popupMenuSeparatorInset, 0);
        separatorArea.removeFromTop(roundToInt(static_cast<float>(separatorArea.getHeight()) * 0.5f - 0.5f));
        graphics.setColour(findColour(PopupMenu::textColourId).withAlpha(popupMenuSeparatorAlpha));
        graphics.fillRect(separatorArea.removeFromTop(1));
    } else {
        const auto textColour = textColourToUse == nullptr ? findColour(PopupMenu::textColourId) : *textColourToUse;

        auto itemArea = area.reduced(1);

        if (isHighlighted && isActive) {
            graphics.setColour(findColour(PopupMenu::highlightedBackgroundColourId));
            graphics.fillRect(itemArea);
            graphics.setColour(findColour(PopupMenu::highlightedTextColourId));
        } else {
            graphics.setColour(textColour.withMultipliedAlpha(isActive ? 1.0f : 0.5f));
        }

        itemArea.reduce(jmin(popupMenuItemMaximumPadding, area.getWidth() / popupMenuItemPaddingDivisor), 0);

        auto font = get_mono_font();
        const auto maxFontHeight = monoFontHeight;
        if (font.getHeight() > maxFontHeight) {
            font.setHeight(maxFontHeight);
        }

        graphics.setFont(font);

        const auto iconArea = itemArea.removeFromLeft(roundToInt(maxFontHeight)).toFloat();

        if (icon != nullptr) {
            icon->drawWithin(
                graphics, iconArea, RectanglePlacement::centred | RectanglePlacement::onlyReduceInSize, 1.0f
            );
            itemArea.removeFromLeft(roundToInt(maxFontHeight * 0.5f));
        } else if (isTicked) {
            const auto tick = getTickShape(1.0f);
            graphics.fillPath(
                tick,
                tick.getTransformToScaleToFit(
                    iconArea.reduced(iconArea.getWidth() / popupMenuTickInsetDivisor, 0).toFloat(), true
                )
            );
        }

        if (hasSubMenu) {
            const auto arrowHeight = 0.6f * getPopupMenuFont().getAscent();

            const auto arrowX = static_cast<float>(itemArea.removeFromRight(static_cast<int>(arrowHeight)).getX());
            const auto centreY = static_cast<float>(itemArea.getCentreY());

            Path path;
            path.startNewSubPath(arrowX, centreY - arrowHeight * 0.5f);
            path.lineTo(arrowX + arrowHeight * subMenuArrowWidthRatio, centreY);
            path.lineTo(arrowX, centreY + arrowHeight * 0.5f);

            graphics.strokePath(path, PathStrokeType(2.0f));
        }

        itemArea.removeFromRight(3);
        graphics.drawFittedText(text, itemArea, Justification::centredLeft, 1);

        if (shortcutKeyText.isNotEmpty()) {
            auto shortcutFont = font;
            shortcutFont.setHeight(shortcutFont.getHeight() * shortcutFontHeightRatio);
            shortcutFont.setHorizontalScale(shortcutFontHorizontalScale);
            graphics.setFont(shortcutFont);
            graphics.drawText(shortcutKeyText, itemArea, Justification::centredRight, true);
        }
    }
}

Font CustomLookAndFeel::getTextButtonFont(TextButton& /*button*/, const int buttonHeight)
{
    return getTextFont().withHeight(
        jmin(maximumButtonFontHeight, static_cast<float>(buttonHeight) * buttonFontHeightRatio)
    );
}

void CustomLookAndFeel::drawTooltip(Graphics& graphics, const String& text, const int width, const int height)
{
    const Rectangle bounds {width, height};
    constexpr auto cornerSize = 5.0f;
    graphics.setColour(findColour(TooltipWindow::backgroundColourId));
    graphics.fillRoundedRectangle(bounds.toFloat(), cornerSize);
    layoutTooltipText(text, findColour(TooltipWindow::textColourId))
        .draw(graphics, {static_cast<float>(width), static_cast<float>(height)});
}

TextLayout CustomLookAndFeel::layoutTooltipText(const String& text, const Colour colour) noexcept
{
    const float tooltipFontSize = 13.0f;
    const int maxToolTipWidth = 400;

    AttributedString attributedText;
    attributedText.setJustification(Justification::centred);
    attributedText.append(text, getTextFont().withHeight(tooltipFontSize), colour);

    TextLayout layout;
    layout.createLayoutWithBalancedLineLengths(attributedText, static_cast<float>(maxToolTipWidth));
    return layout;
}
}  // namespace juce
