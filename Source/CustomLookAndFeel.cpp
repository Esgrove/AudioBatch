#include "CustomLookAndFeel.h"

#include <utility>

namespace juce
{
[[maybe_unused]] const juce::Colour CustomLookAndFeel::blue {90, 189, 249};              // #5ABDF9
[[maybe_unused]] const juce::Colour CustomLookAndFeel::green {0, 210, 150};              // #00D296
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyDark {27, 28, 30};            // #1B1C1E
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyLight {233, 234, 239};        // #E9EAEF
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMedium {88, 90, 95};          // #585A5F
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMediumDark {62, 63, 67};      // #3E3F43
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMiddle {121, 123, 127};       // #797B7F
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMiddleLight {172, 177, 190};  // #ACB1BE
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greySemiDark {46, 47, 52};        // #2E2F34
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greySemiLight {140, 145, 157};    // #8C919D
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greySuperLight {247, 248, 251};   // #F7F8FB
[[maybe_unused]] const juce::Colour CustomLookAndFeel::orange {238, 125, 84};            // #EE7D54
[[maybe_unused]] const juce::Colour CustomLookAndFeel::red {225, 61, 66};                // #E13D42
[[maybe_unused]] const juce::Colour CustomLookAndFeel::yellow {246, 200, 99};            // #F6C863

const juce::Font CustomLookAndFeel::textFont {
    juce::Typeface::createSystemTypefaceFor(BinaryData::RobotoRegular_ttf, BinaryData::RobotoRegular_ttfSize)};

const juce::Font CustomLookAndFeel::monoFont {
    juce::Typeface::createSystemTypefaceFor(BinaryData::RobotoMonoRegular_ttf, BinaryData::RobotoMonoRegular_ttfSize)};

class CustomDocumentWindowButton : public Button
{
public:
    CustomDocumentWindowButton(const String& name, Colour c, Path normal, Path toggled, bool darkMode)
        : Button(name)
        , colour(c)
        , normalShape(std::move(normal))
        , toggledShape(std::move(toggled))
        , useDarkMode(darkMode)
    {}

    void paintButton(Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto background = useDarkMode ? CustomLookAndFeel::greyDark : CustomLookAndFeel::greyLight;

        g.fillAll(background);

        if (!isEnabled() || shouldDrawButtonAsDown) {
            g.setColour(colour.withAlpha(0.6f));
        } else {
            g.setColour(colour);
        }

        if (shouldDrawButtonAsHighlighted) {
            if (getName().equalsIgnoreCase("close")) {
                g.fillAll(CustomLookAndFeel::orange);
            } else {
                g.fillAll();
            }
            g.setColour(background);
        }

        auto& p = getToggleState() ? toggledShape : normalShape;
        auto reducedRect = Justification(Justification::centred)
                               .appliedToRectangle(Rectangle<int>(getHeight(), getHeight()), getLocalBounds())
                               .toFloat()
                               .reduced((float)getHeight() * 0.3f);

        g.fillPath(p, p.getTransformToScaleToFit(reducedRect, true));
    }

private:
    Colour colour;
    Path normalShape;
    Path toggledShape;
    bool useDarkMode {true};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CustomDocumentWindowButton)
};

CustomLookAndFeel::CustomLookAndFeel(bool darkModeEnabled) : darkTheme(darkModeEnabled)
{
    setColourScheme(customColourScheme);

    setColour(juce::ComboBox::arrowColourId, blue);
    // setColour(juce::ComboBox::backgroundColourId, greySemiDark);
    // setColour(juce::ComboBox::focusedOutlineColourId, greyMedium);
    // setColour(juce::ComboBox::outlineColourId, greySemiLight);
    // setColour(juce::FileTreeComponent::backgroundColourId, greySemiDark);
    // setColour(juce::HyperlinkButton::textColourId, greyMedium);
    // setColour(juce::PopupMenu::backgroundColourId, greySemiDark);
    // setColour(juce::PopupMenu::highlightedBackgroundColourId, green);
    // setColour(juce::ProgressBar::backgroundColourId, greyMedium);
    // setColour(juce::ProgressBar::foregroundColourId, green);
    // setColour(juce::ResizableWindow::backgroundColourId, greySemiDark);
    // setColour(juce::TextButton::buttonColourId, greyMedium);
    // setColour(juce::TextButton::buttonOnColourId, green);
    // setColour(juce::TextButton::textColourOffId, greyLight);
    // setColour(juce::TextButton::textColourOnId, greyLight);
    // setColour(juce::TextEditor::backgroundColourId, greySemiDark);
    // setColour(juce::TextEditor::highlightColourId, orange);
    // setColour(juce::TooltipWindow::backgroundColourId, greyDark);
    // setColour(juce::TooltipWindow::textColourId, green);
    setColour(juce::AlertWindow::backgroundColourId, greySemiDark);
    setColour(juce::CaretComponent::caretColourId, blue);
    setColour(juce::DialogWindow::backgroundColourId, greySemiDark);

    setDefaultSansSerifTypeface(textFont.getTypefacePtr());
    setUsingNativeAlertWindows(true);
}

Button* CustomLookAndFeel::createDocumentWindowButton(int buttonType)
{
    Path shape;
    auto crossThickness = 0.15f;

    if (buttonType == DocumentWindow::closeButton) {
        shape.addLineSegment({0.0f, 0.0f, 1.0f, 1.0f}, crossThickness);
        shape.addLineSegment({1.0f, 0.0f, 0.0f, 1.0f}, crossThickness);

        return new CustomDocumentWindowButton("close", CustomLookAndFeel::greyLight, shape, shape, darkTheme);
    }

    if (buttonType == DocumentWindow::minimiseButton) {
        shape.addLineSegment({0.0f, 0.5f, 1.0f, 0.5f}, crossThickness);

        return new CustomDocumentWindowButton("minimise", CustomLookAndFeel::greyLight, shape, shape, darkTheme);
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
            "maximise", CustomLookAndFeel::greyLight, shape, fullscreenShape, darkTheme);
    }

    jassertfalse;
    return nullptr;
}

void CustomLookAndFeel::drawDocumentWindowTitleBar(
    DocumentWindow& window,
    Graphics& g,
    int w,
    int h,
    int titleSpaceX,
    int titleSpaceW,
    const Image* icon,
    bool drawTitleTextOnLeft)
{
    if (w * h == 0) {
        return;
    }

    auto isActive = window.isActiveWindow();

    auto background = darkTheme ? greyDark : greyLight;
    auto text = darkTheme ? greyLight : greyDark;

    g.setColour(background);
    g.fillAll();

    auto font = textFont.withHeight((float)h * 0.54f);
    g.setFont(font);

    auto textW = font.getStringWidth(window.getName());
    auto iconW = 0;
    auto iconH = 0;

    if (icon != nullptr) {
        iconH = juce::roundToIntAccurate(font.getHeight());
        iconW = icon->getWidth() / icon->getHeight() * iconH + 4;
    }

    textW = jmin(titleSpaceW, textW + iconW);
    auto textX = drawTitleTextOnLeft ? titleSpaceX : jmax(titleSpaceX, (w - textW) / 2);

    if (textX + textW > titleSpaceX + titleSpaceW) {
        textX = titleSpaceX + titleSpaceW - textW;
    }

    if (icon != nullptr) {
        g.setOpacity(isActive ? 1.0f : 0.6f);
        g.drawImageWithin(*icon, textX, (h - iconH) / 2, iconW, iconH, RectanglePlacement::centred, false);
        textX += iconW;
        textW -= iconW;
    }

    textX += 4;
    g.setColour(text);
    g.drawText(window.getName(), textX, 0, textW, h, Justification::centredLeft, true);
}

void CustomLookAndFeel::drawPopupMenuItem(
    Graphics& g,
    const Rectangle<int>& area,
    bool isSeparator,
    bool isActive,
    bool isHighlighted,
    bool isTicked,
    bool hasSubMenu,
    const String& text,
    const String& shortcutKeyText,
    const Drawable* icon,
    const Colour* textColourToUse)
{
    if (isSeparator) {
        auto r = area.reduced(5, 0);
        r.removeFromTop(roundToInt(((float)r.getHeight() * 0.5f) - 0.5f));
        g.setColour(findColour(PopupMenu::textColourId).withAlpha(0.3f));
        g.fillRect(r.removeFromTop(1));
    } else {
        auto textColour = (textColourToUse == nullptr) ? findColour(PopupMenu::textColourId) : *textColourToUse;

        auto r = area.reduced(1);

        if (isHighlighted && isActive) {
            g.setColour(findColour(PopupMenu::highlightedBackgroundColourId));
            g.fillRect(r);
            g.setColour(findColour(PopupMenu::highlightedTextColourId));
        } else {
            g.setColour(textColour.withMultipliedAlpha(isActive ? 1.0f : 0.5f));
        }

        r.reduce(jmin(5, area.getWidth() / 20), 0);

        auto font = get_mono_font();
        auto maxFontHeight = monoFontHeight;
        if (font.getHeight() > maxFontHeight) {
            font.setHeight(maxFontHeight);
        }

        g.setFont(font);

        auto iconArea = r.removeFromLeft(roundToInt(maxFontHeight)).toFloat();

        if (icon != nullptr) {
            icon->drawWithin(g, iconArea, RectanglePlacement::centred | RectanglePlacement::onlyReduceInSize, 1.0f);
            r.removeFromLeft(roundToInt(maxFontHeight * 0.5f));
        } else if (isTicked) {
            auto tick = getTickShape(1.0f);
            g.fillPath(
                tick, tick.getTransformToScaleToFit(iconArea.reduced(iconArea.getWidth() / 5, 0).toFloat(), true));
        }

        if (hasSubMenu) {
            auto arrowH = 0.6f * getPopupMenuFont().getAscent();

            auto x = static_cast<float>(r.removeFromRight((int)arrowH).getX());
            auto halfH = static_cast<float>(r.getCentreY());

            Path path;
            path.startNewSubPath(x, halfH - arrowH * 0.5f);
            path.lineTo(x + arrowH * 0.6f, halfH);
            path.lineTo(x, halfH + arrowH * 0.5f);

            g.strokePath(path, PathStrokeType(2.0f));
        }

        r.removeFromRight(3);
        g.drawFittedText(text, r, Justification::centredLeft, 1);

        if (shortcutKeyText.isNotEmpty()) {
            auto f2 = font;
            f2.setHeight(f2.getHeight() * 0.75f);
            f2.setHorizontalScale(0.95f);
            g.setFont(f2);
            g.drawText(shortcutKeyText, r, Justification::centredRight, true);
        }
    }
}

Font CustomLookAndFeel::getTextButtonFont(TextButton&, int buttonHeight)
{
    return textFont.withHeight(jmin(16.0f, (float)buttonHeight * 0.6f));
}

void CustomLookAndFeel::drawTooltip(Graphics& g, const String& text, int width, int height)
{
    Rectangle bounds {width, height};
    auto cornerSize = 5.0f;
    g.setColour(findColour(TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle(bounds.toFloat(), cornerSize);
    layoutTooltipText(text, findColour(TooltipWindow::textColourId))
        .draw(g, {static_cast<float>(width), static_cast<float>(height)});
}

TextLayout CustomLookAndFeel::layoutTooltipText(const String& text, Colour colour) noexcept
{
    const float tooltipFontSize = 13.0f;
    const int maxToolTipWidth = 400;

    AttributedString s;
    s.setJustification(Justification::centred);
    s.append(text, textFont.withHeight(tooltipFontSize), colour);

    TextLayout tl;
    tl.createLayoutWithBalancedLineLengths(s, (float)maxToolTipWidth);
    return tl;
}
}  // namespace juce
