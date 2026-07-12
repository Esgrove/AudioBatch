#pragma once
#include <JuceHeader.h>

namespace juce
{
/// Application-specific look-and-feel with the project's fonts, colours, and popup styling.
class CustomLookAndFeel : public juce::LookAndFeel_V4
{
public:
    /// Builds the colour palette for the requested light or dark theme variant.
    explicit CustomLookAndFeel(bool darkModeEnabled);

    /// Switches the active theme palette without rebuilding the look-and-feel object.
    [[maybe_unused]] void setDarkTheme(const bool darkModeEnabled)
    {
        darkTheme = darkModeEnabled;
    }

    /// Creates the close, minimise, and maximise buttons with the project palette
    /// instead of the stock JUCE title bar button colours.
    /// The caller takes ownership of the returned button.
    juce::Button* createDocumentWindowButton(int buttonType) override;

    /// Draws the custom title bar with the project background colour and text font.
    /// Only used on Windows, where the app opts out of the native title bar.
    void drawDocumentWindowTitleBar(
        DocumentWindow& window,
        Graphics& graphics,
        int width,
        int height,
        int titleSpaceX,
        int titleSpaceWidth,
        const Image* icon,
        bool drawTitleTextOnLeft
    ) override;

    /// Draws tooltips with the project font and rounded background.
    void drawTooltip(Graphics& graphics, const String& text, int width, int height) override;

    /// Lays out tooltip text with balanced line lengths and a capped width.
    /// Shared by drawTooltip so the drawn text and its measured size always agree.
    static TextLayout layoutTooltipText(const String& text, Colour colour) noexcept;

    /// Draws popup menu items with the project monospaced font.
    /// JUCE offers no smaller hook for the menu item font, so the whole item painting is replicated here.
    void drawPopupMenuItem(
        Graphics& graphics,
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
    ) override;

    /// Returns the button label font, scaled down with the button height and capped at 16 points.
    Font getTextButtonFont(TextButton& button, int buttonHeight) override;

    /// Uses the monospaced font for combo box text so numeric choices align.
    Font getComboBoxFont(ComboBox& /*comboBox*/) override
    {
        return get_mono_font();
    }

    /// Returns the monospaced font used for tabular values and status text.
    [[nodiscard]] static Font get_mono_font()
    {
        return monoFont.withHeight(monoFontHeight);
    }

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
        // windowBackground
        0xff121315,
        // widgetBackground
        0xff1A1C20,
        // menuBackground
        0xff202329,
        // outline
        0xff30343C,
        // defaultText
        0xffDCE0E5,
        // defaultFill
        0xff686F79,
        // highlightedText
        0xffF3F5F7,
        // highlightedFill
        0xff76A8DA,
        // menuText
        0xffDCE0E5,
    };

    bool darkTheme;
};
}  // namespace juce
