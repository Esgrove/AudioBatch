/// Application-wide JUCE look-and-feel.
/// Declares CustomLookAndFeel with the project fonts and colour palette,
/// custom title bar buttons and title bar drawing for Windows,
/// and styled tooltips, popup menus, buttons, and combo boxes.

#pragma once
#include <JuceHeader.h>

namespace juce
{
/// Opaque ARGB values of the project colour palette.
/// The CustomLookAndFeel colour accessors and its default colour scheme are built from these values.
namespace palette_argb
{
/// Palette colour #76A8DA.
constexpr juce::uint32 blue {0xff76A8DA};
/// Palette colour #69B786.
constexpr juce::uint32 green {0xff69B786};
/// Palette colour #121315.
constexpr juce::uint32 greyDark {0xff121315};
/// Palette colour #DCE0E5.
constexpr juce::uint32 greyLight {0xffDCE0E5};
/// Palette colour #30343C.
constexpr juce::uint32 greyMedium {0xff30343C};
/// Palette colour #202329.
constexpr juce::uint32 greyMediumDark {0xff202329};
/// Palette colour #686F79.
constexpr juce::uint32 greyMiddle {0xff686F79};
/// Palette colour #AAB0B9.
constexpr juce::uint32 greyMiddleLight {0xffAAB0B9};
/// Palette colour #1A1C20.
constexpr juce::uint32 greySemiDark {0xff1A1C20};
/// Palette colour #808791.
constexpr juce::uint32 greySemiLight {0xff808791};
/// Palette colour #F3F5F7.
constexpr juce::uint32 greySuperLight {0xffF3F5F7};
/// Palette colour #DD8B65.
constexpr juce::uint32 orange {0xffDD8B65};
/// Palette colour #D66565.
constexpr juce::uint32 red {0xffD66565};
/// Palette colour #CEAF6C.
constexpr juce::uint32 yellow {0xffCEAF6C};
}  // namespace palette_argb

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
        return juce::Colour {palette_argb::blue};
    }

    /// Palette colour #69B786.
    [[nodiscard]] static juce::Colour green() noexcept
    {
        return juce::Colour {palette_argb::green};
    }

    /// Palette colour #121315.
    [[nodiscard]] static juce::Colour greyDark() noexcept
    {
        return juce::Colour {palette_argb::greyDark};
    }

    /// Palette colour #DCE0E5.
    [[nodiscard]] static juce::Colour greyLight() noexcept
    {
        return juce::Colour {palette_argb::greyLight};
    }

    /// Palette colour #30343C.
    [[nodiscard]] static juce::Colour greyMedium() noexcept
    {
        return juce::Colour {palette_argb::greyMedium};
    }

    /// Palette colour #202329.
    [[nodiscard]] static juce::Colour greyMediumDark() noexcept
    {
        return juce::Colour {palette_argb::greyMediumDark};
    }

    /// Palette colour #686F79.
    [[nodiscard]] static juce::Colour greyMiddle() noexcept
    {
        return juce::Colour {palette_argb::greyMiddle};
    }

    /// Palette colour #AAB0B9.
    [[nodiscard]] static juce::Colour greyMiddleLight() noexcept
    {
        return juce::Colour {palette_argb::greyMiddleLight};
    }

    /// Palette colour #1A1C20.
    [[nodiscard]] static juce::Colour greySemiDark() noexcept
    {
        return juce::Colour {palette_argb::greySemiDark};
    }

    /// Palette colour #808791.
    [[nodiscard]] static juce::Colour greySemiLight() noexcept
    {
        return juce::Colour {palette_argb::greySemiLight};
    }

    /// Palette colour #F3F5F7.
    [[nodiscard]] static juce::Colour greySuperLight() noexcept
    {
        return juce::Colour {palette_argb::greySuperLight};
    }

    /// Palette colour #DD8B65.
    [[nodiscard]] static juce::Colour orange() noexcept
    {
        return juce::Colour {palette_argb::orange};
    }

    /// Palette colour #D66565.
    [[nodiscard]] static juce::Colour red() noexcept
    {
        return juce::Colour {palette_argb::red};
    }

    /// Palette colour #CEAF6C.
    [[nodiscard]] static juce::Colour yellow() noexcept
    {
        return juce::Colour {palette_argb::yellow};
    }

    static constexpr float monoFontHeight {15.0f};

private:
    const ColourScheme customColourScheme {
        // windowBackground
        palette_argb::greyDark,
        // widgetBackground
        palette_argb::greySemiDark,
        // menuBackground
        palette_argb::greyMediumDark,
        // outline
        palette_argb::greyMedium,
        // defaultText
        palette_argb::greyLight,
        // defaultFill
        palette_argb::greyMiddle,
        // highlightedText
        palette_argb::greySuperLight,
        // highlightedFill
        palette_argb::blue,
        // menuText
        palette_argb::greyLight,
    };

    bool darkTheme;
};
}  // namespace juce
