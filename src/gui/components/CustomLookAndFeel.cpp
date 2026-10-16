/// Implementation of the CustomLookAndFeel styling.
/// Defines the shared colour palette constants and the bundled Roboto fonts,
/// the CustomDocumentWindowButton used for title bar buttons,
/// and the drawing code for the title bar, tooltips, and popup menu items.

#include "gui/components/CustomLookAndFeel.h"

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

/// Alpha of popup menu separator lines relative to the menu text colour.
constexpr float popupMenuSeparatorAlpha {0.3f};

/// Opacity of a column hover highlight relative to its pressed highlight.
constexpr float tableHeaderHoverAlpha {0.625f};
/// Sort-arrow colour matching JUCE's existing table header rendering.
constexpr juce::uint32 tableHeaderSortArrowColour {0x99000000};
}  // namespace

namespace juce
{
/// Aligns checkbox outlines with adjacent controls instead of applying JUCE's extra left inset.
void CustomLookAndFeel::drawToggleButton(
    Graphics& graphics,
    ToggleButton& button,
    const bool shouldDrawButtonAsHighlighted,
    const bool shouldDrawButtonAsDown
)
{
    const auto fontHeight = jmin(
        audiobatch::layout::theme::toggleFontHeight,
        static_cast<float>(button.getHeight()) * audiobatch::layout::theme::toggleFontHeightRatio
    );
    const auto tickWidth = fontHeight * audiobatch::layout::theme::toggleTickWidthRatio;
    drawTickBox(
        graphics,
        button,
        0.5f,
        (static_cast<float>(button.getHeight()) - tickWidth) * 0.5f,
        tickWidth,
        tickWidth,
        button.getToggleState(),
        button.isEnabled(),
        shouldDrawButtonAsHighlighted,
        shouldDrawButtonAsDown
    );
    graphics.setColour(button.findColour(ToggleButton::textColourId));
    graphics.setFont(fontHeight);
    if (!button.isEnabled()) {
        graphics.setOpacity(0.5f);
    }
    graphics.drawFittedText(
        button.getButtonText(),
        button.getLocalBounds().withTrimmedLeft(roundToInt(tickWidth) + audiobatch::layout::theme::toggleTextSpacing),
        Justification::centredLeft,
        1
    );
}

/// Returns the proportional font created from the embedded Roboto typeface.
/// The font is created on first use, so no font construction runs during static initialization.
const juce::Font& CustomLookAndFeel::getTextFont()
{
    // The typeface constructor leaves the name and style empty,
    // which withTypeface on default options would assert on in debug builds.
    static const Font font {FontOptions {
        Typeface::createSystemTypefaceFor(BinaryData::RobotoRegular_ttf, BinaryData::RobotoRegular_ttfSize)
    }};
    return font;
}

/// Returns the base monospaced font created from the embedded Roboto Mono typeface, before height is applied.
/// The font is created on first use, so no font construction runs during static initialization.
const juce::Font& CustomLookAndFeel::getMonoBaseFont()
{
    // The typeface constructor leaves the name and style empty,
    // which withTypeface on default options would assert on in debug builds.
    static const Font font {FontOptions {
        Typeface::createSystemTypefaceFor(BinaryData::RobotoMonoRegular_ttf, BinaryData::RobotoMonoRegular_ttfSize)
    }};
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
    {}

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

/// Builds the colour palette for the requested light or dark theme variant.
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

/// Creates the close, minimise, and maximise buttons with the project palette
/// instead of the stock JUCE title bar button colours.
/// The caller takes ownership of the returned button.
Button* CustomLookAndFeel::createDocumentWindowButton(const int buttonType)
{
    Path shape;
    constexpr auto crossThickness = 0.15f;

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
        fullscreenShape.startNewSubPath(audiobatch::layout::theme::fullscreenGlyphInset, 100.0f);
        fullscreenShape.lineTo(0.0f, 100.0f);
        fullscreenShape.lineTo(0.0f, 0.0f);
        fullscreenShape.lineTo(100.0f, 0.0f);
        fullscreenShape.lineTo(100.0f, audiobatch::layout::theme::fullscreenGlyphInset);
        fullscreenShape.addRectangle(
            audiobatch::layout::theme::fullscreenGlyphInset,
            audiobatch::layout::theme::fullscreenGlyphInset,
            100.0f,
            100.0f
        );
        PathStrokeType(audiobatch::layout::theme::fullscreenGlyphStrokeThickness)
            .createStrokedPath(fullscreenShape, fullscreenShape);

        return new CustomDocumentWindowButton(
            "maximise", CustomLookAndFeel::greyMiddleLight(), shape, fullscreenShape, darkTheme
        );
    }

    jassertfalse;
    return nullptr;
}

/// Draws the custom title bar with the project background colour and text font.
/// Only used on Windows, where the app opts out of the native title bar.
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

/// Draws popup menu items with the project monospaced font.
/// JUCE offers no smaller hook for the menu item font, so the whole item painting is replicated here.
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
        auto separatorArea = area.reduced(audiobatch::layout::theme::popupMenuSeparatorInset, 0);
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

        itemArea.reduce(
            jmin(
                audiobatch::layout::theme::popupMenuItemMaximumPadding,
                area.getWidth() / audiobatch::layout::theme::popupMenuItemPaddingDivisor
            ),
            0
        );

        auto font = getPopupMenuFont();
        const auto maxFontHeight
            = jmin(audiobatch::layout::theme::monoFontHeight, static_cast<float>(area.getHeight()) / 1.3f);
        if (font.getHeight() > maxFontHeight) {
            font.setHeight(maxFontHeight);
        }

        graphics.setFont(font);

        const auto iconArea = itemArea.removeFromLeft(roundToInt(maxFontHeight)).toFloat();

        if (icon != nullptr) {
            icon->drawWithin(
                graphics,
                iconArea,
                static_cast<int>(
                    static_cast<unsigned int>(RectanglePlacement::centred)
                    | static_cast<unsigned int>(RectanglePlacement::onlyReduceInSize)
                ),
                1.0f
            );
            itemArea.removeFromLeft(roundToInt(maxFontHeight * 0.5f));
        } else if (isTicked) {
            const auto tick = getTickShape(1.0f);
            graphics.fillPath(
                tick,
                tick.getTransformToScaleToFit(
                    iconArea.reduced(iconArea.getWidth() / audiobatch::layout::theme::popupMenuTickInsetDivisor, 0)
                        .toFloat(),
                    true
                )
            );
        }

        if (hasSubMenu) {
            const auto arrowHeight = 0.6f * getPopupMenuFont().getAscent();

            const auto arrowX = static_cast<float>(itemArea.removeFromRight(static_cast<int>(arrowHeight)).getX());
            const auto centreY = static_cast<float>(itemArea.getCentreY());

            Path path;
            path.startNewSubPath(arrowX, centreY - arrowHeight * 0.5f);
            path.lineTo(arrowX + arrowHeight * audiobatch::layout::theme::subMenuArrowWidthRatio, centreY);
            path.lineTo(arrowX, centreY + arrowHeight * 0.5f);

            graphics.strokePath(path, PathStrokeType(2.0f));
        }

        itemArea.removeFromRight(3);
        graphics.drawFittedText(text, itemArea, Justification::centredLeft, 1);

        if (shortcutKeyText.isNotEmpty()) {
            auto shortcutFont = font;
            shortcutFont.setHeight(shortcutFont.getHeight() * audiobatch::layout::theme::shortcutFontHeightRatio);
            shortcutFont.setHorizontalScale(audiobatch::layout::theme::shortcutFontHorizontalScale);
            graphics.setFont(shortcutFont);
            graphics.drawText(shortcutKeyText, itemArea, Justification::centredRight, true);
        }
    }
}

/// Draws column titles one logical pixel larger with the embedded Roboto face.
/// Preserves JUCE's hover feedback, text inset, and reserved sort-arrow space.
void CustomLookAndFeel::drawTableHeaderColumn(
    Graphics& graphics,
    TableHeaderComponent& header,
    const String& columnName,
    int /*columnId*/,
    const int width,
    const int height,
    const bool isMouseOver,
    const bool isMouseDown,
    const int columnFlags
)
{
    const auto highlightColour = header.findColour(TableHeaderComponent::highlightColourId);
    if (isMouseDown) {
        graphics.fillAll(highlightColour);
    } else if (isMouseOver) {
        graphics.fillAll(highlightColour.withMultipliedAlpha(tableHeaderHoverAlpha));
    }

    Rectangle<int> area(width, height);
    area.reduce(audiobatch::layout::theme::tableHeaderTextInset, 0);
    const auto flags = static_cast<unsigned int>(columnFlags);
    constexpr auto sortedForwards = static_cast<unsigned int>(TableHeaderComponent::sortedForwards);
    constexpr auto sortedBackwards = static_cast<unsigned int>(TableHeaderComponent::sortedBackwards);
    if ((flags & (sortedForwards | sortedBackwards)) != 0) {
        Path sortArrow;
        const auto tip = (flags & sortedForwards) != 0 ? -audiobatch::layout::theme::tableHeaderSortArrowTip
                                                       : audiobatch::layout::theme::tableHeaderSortArrowTip;
        sortArrow.addTriangle(0.0f, 0.0f, 0.5f, tip, 1.0f, 0.0f);
        graphics.setColour(Colour(tableHeaderSortArrowColour));
        graphics.fillPath(
            sortArrow, sortArrow.getTransformToScaleToFit(area.removeFromRight(height / 2).reduced(2).toFloat(), true)
        );
    }

    graphics.setColour(header.findColour(TableHeaderComponent::textColourId));
    graphics.setFont(
        getTextFont().withHeight(
            static_cast<float>(height) * audiobatch::layout::theme::tableHeaderFontHeightRatio
            + audiobatch::layout::theme::tableHeaderFontHeightIncrease
        )
    );
    graphics.drawFittedText(columnName, area, Justification::centredLeft, 1);
}

/// Uses the same embedded monospaced font for popup sizing and item painting.
Font CustomLookAndFeel::getPopupMenuFont()
{
    return get_mono_font();
}

/// Returns the button label font, scaled down with the button height and capped at 16 points.
Font CustomLookAndFeel::getTextButtonFont(TextButton& /*button*/, const int buttonHeight)
{
    return getTextFont().withHeight(jmin(
        audiobatch::layout::theme::maximumButtonFontHeight,
        static_cast<float>(buttonHeight) * audiobatch::layout::theme::buttonFontHeightRatio
    ));
}

/// Draws tooltips with the project font and rounded background.
void CustomLookAndFeel::drawTooltip(Graphics& graphics, const String& text, const int width, const int height)
{
    const Rectangle bounds {width, height};
    graphics.setColour(findColour(TooltipWindow::backgroundColourId));
    graphics.fillRoundedRectangle(bounds.toFloat(), audiobatch::layout::theme::tooltipCornerRadius);
    layoutTooltipText(text, findColour(TooltipWindow::textColourId))
        .draw(graphics, {static_cast<float>(width), static_cast<float>(height)});
}

/// Lays out tooltip text with balanced line lengths and a capped width.
/// Shared by drawTooltip so the drawn text and its measured size always agree.
TextLayout CustomLookAndFeel::layoutTooltipText(const String& text, const Colour colour) noexcept
{
    AttributedString attributedText;
    attributedText.setJustification(Justification::centred);
    attributedText.append(text, getTextFont().withHeight(audiobatch::layout::theme::tooltipFontHeight), colour);

    TextLayout layout;
    layout.createLayoutWithBalancedLineLengths(
        attributedText, static_cast<float>(audiobatch::layout::theme::tooltipMaximumWidth)
    );
    return layout;
}
}  // namespace juce
