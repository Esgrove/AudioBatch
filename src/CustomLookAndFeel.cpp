#include "CustomLookAndFeel.h"

#include <cmath>
#include <utility>

namespace juce
{
/// Application palette constants shared by the custom look-and-feel.
// #76A8DA
[[maybe_unused]] const juce::Colour CustomLookAndFeel::blue {118, 168, 218};
// #69B786
[[maybe_unused]] const juce::Colour CustomLookAndFeel::green {105, 183, 134};
// #121315
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyDark {18, 19, 21};
// #DCE0E5
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyLight {220, 224, 229};
// #30343C
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMedium {48, 52, 60};
// #202329
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMediumDark {32, 35, 41};
// #686F79
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMiddle {104, 111, 121};
// #AAB0B9
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMiddleLight {170, 176, 185};
// #1A1C20
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greySemiDark {26, 28, 32};
// #808791
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greySemiLight {128, 135, 145};
// #F3F5F7
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greySuperLight {243, 245, 247};
// #DD8B65
[[maybe_unused]] const juce::Colour CustomLookAndFeel::orange {221, 139, 101};
// #D66565
[[maybe_unused]] const juce::Colour CustomLookAndFeel::red {214, 101, 101};
// #CEAF6C
[[maybe_unused]] const juce::Colour CustomLookAndFeel::yellow {206, 175, 108};

const juce::Font CustomLookAndFeel::textFont {juce::FontOptions().withTypeface(
    juce::Typeface::createSystemTypefaceFor(BinaryData::RobotoRegular_ttf, BinaryData::RobotoRegular_ttfSize)
)};

const juce::Font CustomLookAndFeel::monoFont {juce::FontOptions().withTypeface(
    juce::Typeface::createSystemTypefaceFor(BinaryData::RobotoMonoRegular_ttf, BinaryData::RobotoMonoRegular_ttfSize)
)};

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

    /// Fills the button background for the current hover and pressed state and draws the glyph path.
    /// The close button highlights in orange, the others use a neutral grey.
    void paintButton(
        Graphics& graphics,
        const bool shouldDrawButtonAsHighlighted,
        const bool shouldDrawButtonAsDown
    ) override
    {
        const auto background = useDarkMode ? CustomLookAndFeel::greyDark : CustomLookAndFeel::greyLight;
        auto glyphColour = colour;

        graphics.fillAll(background);

        if (!isEnabled() || shouldDrawButtonAsDown) {
            glyphColour = colour.withAlpha(0.6f);
        }

        if (shouldDrawButtonAsHighlighted) {
            if (getName().equalsIgnoreCase("close")) {
                graphics.fillAll(CustomLookAndFeel::orange);
            } else {
                graphics.fillAll(CustomLookAndFeel::greyMediumDark);
            }

            glyphColour = CustomLookAndFeel::greySuperLight;
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
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CustomDocumentWindowButton)
};

CustomLookAndFeel::CustomLookAndFeel(const bool darkModeEnabled) : darkTheme(darkModeEnabled)
{
    setColourScheme(customColourScheme);

    setColour(juce::ComboBox::arrowColourId, blue);
    setColour(juce::ComboBox::backgroundColourId, greySemiDark);
    setColour(juce::ComboBox::focusedOutlineColourId, blue.withAlpha(0.45f));
    setColour(juce::ComboBox::outlineColourId, greyMedium);
    // setColour(juce::FileTreeComponent::backgroundColourId, greySemiDark);
    setColour(juce::HyperlinkButton::textColourId, blue.withAlpha(0.9f));
    setColour(juce::PopupMenu::backgroundColourId, greySemiDark);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, blue.withAlpha(0.18f));
    setColour(juce::PopupMenu::highlightedTextColourId, greySuperLight);
    setColour(juce::PopupMenu::textColourId, greyLight);
    // setColour(juce::ProgressBar::backgroundColourId, greyMedium);
    // setColour(juce::ProgressBar::foregroundColourId, green);
    setColour(juce::ResizableWindow::backgroundColourId, greyDark);
    setColour(juce::TextButton::buttonColourId, greyMedium);
    setColour(juce::TextButton::buttonOnColourId, green);
    setColour(juce::TextButton::textColourOffId, greySuperLight.withAlpha(0.96f));
    setColour(juce::TextButton::textColourOnId, greySuperLight);
    setColour(juce::Slider::backgroundColourId, greyMedium);
    setColour(juce::Slider::trackColourId, blue.withAlpha(0.7f));
    setColour(juce::Slider::thumbColourId, greySuperLight);
    setColour(juce::Slider::rotarySliderFillColourId, blue.withAlpha(0.72f));
    setColour(juce::Slider::rotarySliderOutlineColourId, greyMedium);
    setColour(juce::Slider::textBoxBackgroundColourId, greyMediumDark);
    setColour(juce::Slider::textBoxTextColourId, greySuperLight);
    setColour(juce::Slider::textBoxOutlineColourId, greyMedium);
    setColour(juce::TextEditor::backgroundColourId, greySemiDark);
    setColour(juce::TextEditor::highlightColourId, blue.withAlpha(0.24f));
    setColour(juce::TextEditor::outlineColourId, greyMedium);
    setColour(juce::TooltipWindow::backgroundColourId, greyMediumDark);
    setColour(juce::TooltipWindow::textColourId, greySuperLight);
    setColour(juce::AlertWindow::backgroundColourId, greySemiDark);
    setColour(juce::CaretComponent::caretColourId, blue);
    setColour(juce::DialogWindow::backgroundColourId, greySemiDark);

    setDefaultSansSerifTypeface(textFont.getTypefacePtr());
    setUsingNativeAlertWindows(true);
}

Button* CustomLookAndFeel::createDocumentWindowButton(const int buttonType)
{
    Path shape;
    const auto crossThickness = 0.15f;

    if (buttonType == DocumentWindow::closeButton) {
        shape.addLineSegment({0.0f, 0.0f, 1.0f, 1.0f}, crossThickness);
        shape.addLineSegment({1.0f, 0.0f, 0.0f, 1.0f}, crossThickness);

        return new CustomDocumentWindowButton("close", CustomLookAndFeel::greyMiddleLight, shape, shape, darkTheme);
    }

    if (buttonType == DocumentWindow::minimiseButton) {
        shape.addLineSegment({0.0f, 0.5f, 1.0f, 0.5f}, crossThickness);

        return new CustomDocumentWindowButton("minimise", CustomLookAndFeel::greyMiddleLight, shape, shape, darkTheme);
    }

    if (buttonType == DocumentWindow::maximiseButton) {
        shape.addLineSegment({0.5f, 0.0f, 0.5f, 1.0f}, crossThickness);
        shape.addLineSegment({0.0f, 0.5f, 1.0f, 0.5f}, crossThickness);

        Path fullscreenShape;
        fullscreenShape.startNewSubPath(45.0f, 100.0f);
        fullscreenShape.lineTo(0.0f, 100.0f);
        fullscreenShape.lineTo(0.0f, 0.0f);
        fullscreenShape.lineTo(100.0f, 0.0f);
        fullscreenShape.lineTo(100.0f, 45.0f);
        fullscreenShape.addRectangle(45.0f, 45.0f, 100.0f, 100.0f);
        PathStrokeType(30.0f).createStrokedPath(fullscreenShape, fullscreenShape);

        return new CustomDocumentWindowButton(
            "maximise", CustomLookAndFeel::greyMiddleLight, shape, fullscreenShape, darkTheme
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

    const auto background = darkTheme ? greyDark : greyLight;
    const auto text = darkTheme ? greyLight : greyDark;

    graphics.setColour(background);
    graphics.fillAll();

    const auto font = textFont.withHeight(static_cast<float>(height) * 0.54f);
    graphics.setFont(font);

    const auto textWidth = juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(font, window.getName())));
    auto iconWidth = 0;
    auto iconHeight = 0;

    if (icon != nullptr) {
        iconHeight = juce::roundToIntAccurate(font.getHeight());
        iconWidth = icon->getWidth() / icon->getHeight() * iconHeight + 4;
    }

    const auto contentWidth = jmin(titleSpaceWidth, textWidth + iconWidth);
    auto textX = drawTitleTextOnLeft ? titleSpaceX : jmax(titleSpaceX, (width - contentWidth) / 2);

    if (textX + contentWidth > titleSpaceX + titleSpaceWidth) {
        textX = titleSpaceX + titleSpaceWidth - contentWidth;
    }

    if (icon != nullptr) {
        graphics.setOpacity(isActive ? 1.0f : 0.6f);
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
        auto separatorArea = area.reduced(5, 0);
        separatorArea.removeFromTop(roundToInt(static_cast<float>(separatorArea.getHeight()) * 0.5f - 0.5f));
        graphics.setColour(findColour(PopupMenu::textColourId).withAlpha(0.3f));
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

        itemArea.reduce(jmin(5, area.getWidth() / 20), 0);

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
                tick, tick.getTransformToScaleToFit(iconArea.reduced(iconArea.getWidth() / 5, 0).toFloat(), true)
            );
        }

        if (hasSubMenu) {
            const auto arrowHeight = 0.6f * getPopupMenuFont().getAscent();

            const auto arrowX = static_cast<float>(itemArea.removeFromRight(static_cast<int>(arrowHeight)).getX());
            const auto centreY = static_cast<float>(itemArea.getCentreY());

            Path path;
            path.startNewSubPath(arrowX, centreY - arrowHeight * 0.5f);
            path.lineTo(arrowX + arrowHeight * 0.6f, centreY);
            path.lineTo(arrowX, centreY + arrowHeight * 0.5f);

            graphics.strokePath(path, PathStrokeType(2.0f));
        }

        itemArea.removeFromRight(3);
        graphics.drawFittedText(text, itemArea, Justification::centredLeft, 1);

        if (shortcutKeyText.isNotEmpty()) {
            auto shortcutFont = font;
            shortcutFont.setHeight(shortcutFont.getHeight() * 0.75f);
            shortcutFont.setHorizontalScale(0.95f);
            graphics.setFont(shortcutFont);
            graphics.drawText(shortcutKeyText, itemArea, Justification::centredRight, true);
        }
    }
}

Font CustomLookAndFeel::getTextButtonFont(TextButton& /*button*/, const int buttonHeight)
{
    return textFont.withHeight(jmin(16.0f, static_cast<float>(buttonHeight) * 0.6f));
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
    attributedText.append(text, textFont.withHeight(tooltipFontSize), colour);

    TextLayout layout;
    layout.createLayoutWithBalancedLineLengths(attributedText, static_cast<float>(maxToolTipWidth));
    return layout;
}
}  // namespace juce
