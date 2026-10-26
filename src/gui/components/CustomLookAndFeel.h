/// Application-wide JUCE look-and-feel.
/// Declares CustomLookAndFeel with the project fonts and colour palette,
/// custom title bar buttons and title bar drawing for Windows,
/// and styled tooltips, popup menus, buttons, and combo boxes.

#pragma once
#include "gui/GuiLayout.h"

#include <JuceHeader.h>

namespace audiobatch
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
    explicit CustomLookAndFeel(bool darkModeEnabled);

    /// Switches the active theme palette without rebuilding the look-and-feel object.
    [[maybe_unused]] void setDarkTheme(const bool darkModeEnabled)
    {
        darkTheme = darkModeEnabled;
    }

    juce::Button* createDocumentWindowButton(int buttonType) override;

    void drawDocumentWindowTitleBar(
        juce::DocumentWindow& window,
        juce::Graphics& graphics,
        int width,
        int height,
        int titleSpaceX,
        int titleSpaceWidth,
        const juce::Image* icon,
        bool drawTitleTextOnLeft
    ) override;

    void drawTooltip(juce::Graphics& graphics, const juce::String& text, int width, int height) override;

    void drawToggleButton(
        juce::Graphics& graphics,
        juce::ToggleButton& button,
        bool shouldDrawButtonAsHighlighted,
        bool shouldDrawButtonAsDown
    ) override;

    void drawTableHeaderColumn(
        juce::Graphics& graphics,
        juce::TableHeaderComponent& header,
        const juce::String& columnName,
        int columnId,
        int width,
        int height,
        bool isMouseOver,
        bool isMouseDown,
        int columnFlags
    ) override;

    static juce::TextLayout layoutTooltipText(const juce::String& text, juce::Colour colour) noexcept;

    void drawMenuBarItem(
        juce::Graphics& graphics,
        int width,
        int height,
        int itemIndex,
        const juce::String& itemText,
        bool isMouseOverItem,
        bool isMenuOpen,
        bool isMouseOverBar,
        juce::MenuBarComponent& menuBar
    ) override;

    void drawPopupMenuItem(
        juce::Graphics& graphics,
        const juce::Rectangle<int>& area,
        bool isSeparator,
        bool isActive,
        bool isHighlighted,
        bool isTicked,
        bool hasSubMenu,
        const juce::String& text,
        const juce::String& shortcutKeyText,
        const juce::Drawable* icon,
        const juce::Colour* textColourToUse
    ) override;

    void getIdealPopupMenuItemSize(
        const juce::String& text,
        bool isSeparator,
        int standardMenuItemHeight,
        int& idealWidth,
        int& idealHeight
    ) override;

    juce::Font getMenuBarFont(juce::MenuBarComponent& menuBar, int itemIndex, const juce::String& itemText) override;

    juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;

    juce::Font getPopupMenuFont() override;

    /// Uses the monospaced font for combo box text so numeric choices align.
    juce::Font getComboBoxFont(juce::ComboBox& /*comboBox*/) override
    {
        return get_mono_font();
    }

    /// Returns the monospaced font used for tabular values and status text.
    [[nodiscard]] static juce::Font get_mono_font()
    {
        return getMonoBaseFont().withHeight(layout::theme::monoFontHeight);
    }

    [[nodiscard]] static const juce::Font& getTextFont();

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
}  // namespace audiobatch
