#include "CustomLookAndFeel.h"

#include <cmath>
#include <utility>

namespace juce
{
/// Application palette constants shared by the custom look-and-feel.
[[maybe_unused]] const juce::Colour CustomLookAndFeel::blue {118, 168, 218};             // #76A8DA
[[maybe_unused]] const juce::Colour CustomLookAndFeel::green {105, 183, 134};            // #69B786
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyDark {18, 19, 21};            // #121315
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyLight {220, 224, 229};        // #DCE0E5
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMedium {48, 52, 60};          // #30343C
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMediumDark {32, 35, 41};      // #202329
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMiddle {104, 111, 121};       // #686F79
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greyMiddleLight {170, 176, 185};  // #AAB0B9
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greySemiDark {26, 28, 32};        // #1A1C20
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greySemiLight {128, 135, 145};    // #808791
[[maybe_unused]] const juce::Colour CustomLookAndFeel::greySuperLight {243, 245, 247};   // #F3F5F7
[[maybe_unused]] const juce::Colour CustomLookAndFeel::orange {221, 139, 101};           // #DD8B65
[[maybe_unused]] const juce::Colour CustomLookAndFeel::red {214, 101, 101};              // #D66565
[[maybe_unused]] const juce::Colour CustomLookAndFeel::yellow {206, 175, 108};           // #CEAF6C

const juce::Font CustomLookAndFeel::textFont {juce::FontOptions().withTypeface(
    juce::Typeface::createSystemTypefaceFor(BinaryData::RobotoRegular_ttf, BinaryData::RobotoRegular_ttfSize)
)};

const juce::Font CustomLookAndFeel::monoFont {juce::FontOptions().withTypeface(
    juce::Typeface::createSystemTypefaceFor(BinaryData::RobotoMonoRegular_ttf, BinaryData::RobotoMonoRegular_ttfSize)
)};

class CustomDocumentWindowButton : public Button
{
public:
    CustomDocumentWindowButton(const String& name, Colour c, Path normal, Path toggled, bool darkMode) :
        Button(name),
        colour(c),
        normalShape(std::move(normal)),
        toggledShape(std::move(toggled)),
        useDarkMode(darkMode)
    { }

    void paintButton(Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto background = useDarkMode ? CustomLookAndFeel::greyDark : CustomLookAndFeel::greyLight;
        auto glyphColour = colour;

        g.fillAll(background);

        if (!isEnabled() || shouldDrawButtonAsDown) {
            glyphColour = colour.withAlpha(0.6f);
        }

        if (shouldDrawButtonAsHighlighted) {
            if (getName().equalsIgnoreCase("close")) {
                g.fillAll(CustomLookAndFeel::orange);
            } else {
                g.fillAll(CustomLookAndFeel::greyMediumDark);
            }

            glyphColour = CustomLookAndFeel::greySuperLight;
        }

        g.setColour(glyphColour);

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

Button* CustomLookAndFeel::createDocumentWindowButton(int buttonType)
{
    Path shape;
    auto crossThickness = 0.15f;

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
    Graphics& g,
    int w,
    int h,
    int titleSpaceX,
    int titleSpaceW,
    const Image* icon,
    bool drawTitleTextOnLeft
)
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

    auto textW = juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(font, window.getName())));
    auto iconW = 0;
    auto iconH = 0;

    if (icon != nullptr) {
        iconH = juce::roundToIntAccurate(font.getHeight());
        iconW = icon->getWidth() / icon->getHeight() * iconH + 4;
    }

    auto contentW = jmin(titleSpaceW, textW + iconW);
    auto textX = drawTitleTextOnLeft ? titleSpaceX : jmax(titleSpaceX, (w - contentW) / 2);

    if (textX + contentW > titleSpaceX + titleSpaceW) {
        textX = titleSpaceX + titleSpaceW - contentW;
    }

    if (icon != nullptr) {
        g.setOpacity(isActive ? 1.0f : 0.6f);
        g.drawImageWithin(*icon, textX, (h - iconH) / 2, iconW, iconH, RectanglePlacement::centred, false);
        textX += iconW;
    }

    textX += 4;
    auto availableTextWidth = jmax(0, titleSpaceX + titleSpaceW - textX);
    g.setColour(text);
    g.drawText(window.getName(), textX, 0, availableTextWidth, h, Justification::centredLeft, true);
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
    const Colour* textColourToUse
)
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
                tick, tick.getTransformToScaleToFit(iconArea.reduced(iconArea.getWidth() / 5, 0).toFloat(), true)
            );
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
