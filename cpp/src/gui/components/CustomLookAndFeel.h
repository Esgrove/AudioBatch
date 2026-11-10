/// Application-wide JUCE look-and-feel, fonts, palette and desktop control drawing.

#pragma once
#include "gui/GuiLayout.h"

#include <JuceHeader.h>

namespace audiobatch
{
/// Opaque ARGB values shared by palette accessors and the default colour scheme.
namespace palette_argb
{
constexpr juce::uint32 BLUE {0xff76A8DA};
constexpr juce::uint32 GREEN {0xff69B786};
constexpr juce::uint32 GREY_DARK {0xff121315};
constexpr juce::uint32 GREY_LIGHT {0xffDCE0E5};
constexpr juce::uint32 GREY_MEDIUM {0xff30343C};
constexpr juce::uint32 GREY_MEDIUM_DARK {0xff202329};
constexpr juce::uint32 GREY_MIDDLE {0xff686F79};
constexpr juce::uint32 GREY_MIDDLE_LIGHT {0xffAAB0B9};
constexpr juce::uint32 GREY_SEMI_DARK {0xff1A1C20};
constexpr juce::uint32 GREY_SEMI_LIGHT {0xff808791};
constexpr juce::uint32 GREY_SUPER_LIGHT {0xffF3F5F7};
constexpr juce::uint32 ORANGE {0xffDD8B65};
constexpr juce::uint32 RED {0xffD66565};
constexpr juce::uint32 YELLOW {0xffCEAF6C};
}  // namespace palette_argb

/// Stateful application theme with the project's fonts, colours and popup styling.
class CustomLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit CustomLookAndFeel(bool darkModeEnabled);

    /// Palette colour #76A8DA.
    [[nodiscard]] static juce::Colour blue() noexcept
    {
        return juce::Colour {palette_argb::BLUE};
    }

    /// Palette colour #69B786.
    [[nodiscard]] static juce::Colour green() noexcept
    {
        return juce::Colour {palette_argb::GREEN};
    }

    /// Palette colour #121315.
    [[nodiscard]] static juce::Colour greyDark() noexcept
    {
        return juce::Colour {palette_argb::GREY_DARK};
    }

    /// Palette colour #DCE0E5.
    [[nodiscard]] static juce::Colour greyLight() noexcept
    {
        return juce::Colour {palette_argb::GREY_LIGHT};
    }

    /// Palette colour #30343C.
    [[nodiscard]] static juce::Colour greyMedium() noexcept
    {
        return juce::Colour {palette_argb::GREY_MEDIUM};
    }

    /// Palette colour #202329.
    [[nodiscard]] static juce::Colour greyMediumDark() noexcept
    {
        return juce::Colour {palette_argb::GREY_MEDIUM_DARK};
    }

    /// Palette colour #686F79.
    [[nodiscard]] static juce::Colour greyMiddle() noexcept
    {
        return juce::Colour {palette_argb::GREY_MIDDLE};
    }

    /// Palette colour #AAB0B9.
    [[nodiscard]] static juce::Colour greyMiddleLight() noexcept
    {
        return juce::Colour {palette_argb::GREY_MIDDLE_LIGHT};
    }

    /// Palette colour #1A1C20.
    [[nodiscard]] static juce::Colour greySemiDark() noexcept
    {
        return juce::Colour {palette_argb::GREY_SEMI_DARK};
    }

    /// Palette colour #808791.
    [[nodiscard]] static juce::Colour greySemiLight() noexcept
    {
        return juce::Colour {palette_argb::GREY_SEMI_LIGHT};
    }

    /// Palette colour #F3F5F7.
    [[nodiscard]] static juce::Colour greySuperLight() noexcept
    {
        return juce::Colour {palette_argb::GREY_SUPER_LIGHT};
    }

    /// Palette colour #DD8B65.
    [[nodiscard]] static juce::Colour orange() noexcept
    {
        return juce::Colour {palette_argb::ORANGE};
    }

    /// Palette colour #D66565.
    [[nodiscard]] static juce::Colour red() noexcept
    {
        return juce::Colour {palette_argb::RED};
    }

    /// Palette colour #CEAF6C.
    [[nodiscard]] static juce::Colour yellow() noexcept
    {
        return juce::Colour {palette_argb::YELLOW};
    }

    /// Returns the monospaced font used for tabular values and status text.
    [[nodiscard]] static juce::Font get_mono_font()
    {
        return getMonoBaseFont().withHeight(layout::theme::MONO_FONT_HEIGHT);
    }

    [[nodiscard]] static const juce::Font& getMonoBaseFont();
    [[nodiscard]] static juce::Font getTableHeaderFont(int headerHeight);
    [[nodiscard]] static const juce::Font& getTextFont();

    static juce::TextLayout layoutTooltipText(const juce::String& text, juce::Colour colour) noexcept;

    /// Switches the active palette without rebuilding the look-and-feel object.
    [[maybe_unused]] void setDarkTheme(const bool darkModeEnabled)
    {
        darkTheme = darkModeEnabled;
    }

    // juce::LookAndFeel_V4
    juce::Button* createDocumentWindowButton(int buttonType) override;

    /// Uses the monospaced font so numeric choices align.
    juce::Font getComboBoxFont(juce::ComboBox& /*comboBox*/) override
    {
        return get_mono_font();
    }

    juce::Font getMenuBarFont(juce::MenuBarComponent& menuBar, int itemIndex, const juce::String& itemText) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;

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
    void drawToggleButton(
        juce::Graphics& graphics,
        juce::ToggleButton& button,
        bool shouldDrawButtonAsHighlighted,
        bool shouldDrawButtonAsDown
    ) override;
    void drawTooltip(juce::Graphics& graphics, const juce::String& text, int width, int height) override;
    void getIdealPopupMenuItemSize(
        const juce::String& text,
        bool isSeparator,
        int standardMenuItemHeight,
        int& idealWidth,
        int& idealHeight
    ) override;

private:
    const ColourScheme customColourScheme {
        // windowBackground
        palette_argb::GREY_DARK,
        // widgetBackground
        palette_argb::GREY_SEMI_DARK,
        // menuBackground
        palette_argb::GREY_MEDIUM_DARK,
        // outline
        palette_argb::GREY_MEDIUM,
        // defaultText
        palette_argb::GREY_LIGHT,
        // defaultFill
        palette_argb::GREY_MIDDLE,
        // highlightedText
        palette_argb::GREY_SUPER_LIGHT,
        // highlightedFill
        palette_argb::BLUE,
        // menuText
        palette_argb::GREY_LIGHT,
    };
    /// True for the dark palette variant followed by the window buttons.
    bool darkTheme;
};
}  // namespace audiobatch
