/// Application-wide JUCE look-and-feel.
/// Declares CustomLookAndFeel with the project fonts and colour palette,
/// custom title bar buttons and title bar drawing for Windows,
/// and styled tooltips, popup menus, buttons, and combo boxes.

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
        return getMonoBaseFont().withHeight(monoFontHeight);
    }

    /// Returns the proportional font created from the embedded Roboto typeface.
    /// The font is created on first use, so no font construction runs during static initialization.
    [[nodiscard]] static const juce::Font& getTextFont();

    /// Returns the base monospaced font created from the embedded Roboto Mono typeface, before height is applied.
    /// The font is created on first use, so no font construction runs during static initialization.
    [[nodiscard]] static const juce::Font& getMonoBaseFont();

    /// Palette colour #76A8DA.
    [[nodiscard]] static juce::Colour blue() noexcept
    {
        return {118, 168, 218};
    }

    /// Palette colour #69B786.
    [[nodiscard]] static juce::Colour green() noexcept
    {
        return {105, 183, 134};
    }

    /// Palette colour #121315.
    [[nodiscard]] static juce::Colour greyDark() noexcept
    {
        return {18, 19, 21};
    }

    /// Palette colour #DCE0E5.
    [[nodiscard]] static juce::Colour greyLight() noexcept
    {
        return {220, 224, 229};
    }

    /// Palette colour #30343C.
    [[nodiscard]] static juce::Colour greyMedium() noexcept
    {
        return {48, 52, 60};
    }

    /// Palette colour #202329.
    [[nodiscard]] static juce::Colour greyMediumDark() noexcept
    {
        return {32, 35, 41};
    }

    /// Palette colour #686F79.
    [[nodiscard]] static juce::Colour greyMiddle() noexcept
    {
        return {104, 111, 121};
    }

    /// Palette colour #AAB0B9.
    [[nodiscard]] static juce::Colour greyMiddleLight() noexcept
    {
        return {170, 176, 185};
    }

    /// Palette colour #1A1C20.
    [[nodiscard]] static juce::Colour greySemiDark() noexcept
    {
        return {26, 28, 32};
    }

    /// Palette colour #808791.
    [[nodiscard]] static juce::Colour greySemiLight() noexcept
    {
        return {128, 135, 145};
    }

    /// Palette colour #F3F5F7.
    [[nodiscard]] static juce::Colour greySuperLight() noexcept
    {
        return {243, 245, 247};
    }

    /// Palette colour #DD8B65.
    [[nodiscard]] static juce::Colour orange() noexcept
    {
        return {221, 139, 101};
    }

    /// Palette colour #D66565.
    [[nodiscard]] static juce::Colour red() noexcept
    {
        return {214, 101, 101};
    }

    /// Palette colour #CEAF6C.
    [[nodiscard]] static juce::Colour yellow() noexcept
    {
        return {206, 175, 108};
    }

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
