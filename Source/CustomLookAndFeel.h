#pragma once
#include <JuceHeader.h>

namespace juce
{
class CustomLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit CustomLookAndFeel(bool darkModeEnabled);

    [[maybe_unused]] void setDarkTheme(bool darkModeEnabled) { darkTheme = darkModeEnabled; }

    // override title bar button colours
    juce::Button* createDocumentWindowButton(int buttonType) override;

    // override title bar colour and font
    void drawDocumentWindowTitleBar(
        DocumentWindow& window,
        Graphics& g,
        int w,
        int h,
        int titleSpaceX,
        int titleSpaceW,
        const Image* icon,
        bool drawTitleTextOnLeft) override;

    // overridden to set tooltip font
    void drawTooltip(Graphics& g, const String& text, int width, int height) override;

    TextLayout layoutTooltipText(const String& text, Colour colour) noexcept;

    // this whole thing has to be overridden to set the popup menu font :(
    void drawPopupMenuItem(
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
        const Colour* textColourToUse) override;

    Font getTextButtonFont(TextButton&, int buttonHeight) override;

    Font getComboBoxFont(ComboBox&) override { return get_mono_font(); }
    [[nodiscard]] static Font get_mono_font() { return monoFont.withHeight(monoFontHeight); }

    static const juce::Font monoFont;
    static const juce::Font textFont;

    [[maybe_unused]] static const juce::Colour blue;
    [[maybe_unused]] static const juce::Colour green;
    [[maybe_unused]] static const juce::Colour greyDark;
    [[maybe_unused]] static const juce::Colour greyLight;
    [[maybe_unused]] static const juce::Colour greyMedium;
    [[maybe_unused]] static const juce::Colour greyMediumDark;
    [[maybe_unused]] static const juce::Colour greyMiddle;
    [[maybe_unused]] static const juce::Colour greyMiddleLight;
    [[maybe_unused]] static const juce::Colour greySemiDark;
    [[maybe_unused]] static const juce::Colour greySemiLight;
    [[maybe_unused]] static const juce::Colour greySuperLight;
    [[maybe_unused]] static const juce::Colour orange;
    [[maybe_unused]] static const juce::Colour red;
    [[maybe_unused]] static const juce::Colour yellow;

    static constexpr float monoFontHeight {15.0f};

private:
    const ColourScheme customColourScheme {
        0xff1B1C1E,  // windowBackground
        0xff3E3F43,  // widgetBackground
        0xff585A5F,  // menuBackground
        0xff8C919D,  // outline
        0xffF7F8FB,  // defaultText
        0xff797B7F,  // defaultFill
        0xffF7F8FB,  // highlightedText
        0xff5ABDF9,  // highlightedFill
        0xffF7F8FB,  // menuText
    };

    bool darkTheme;
};
}  // namespace juce
