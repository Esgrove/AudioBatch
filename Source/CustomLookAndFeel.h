#pragma once
#include <JuceHeader.h>

namespace juce
{
class CustomLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit CustomLookAndFeel(bool dark_mode_enabled);

    [[maybe_unused]] void setDarkTheme(bool dark_mode_enabled) { dark_theme = dark_mode_enabled; }

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
    [[nodiscard]] static Font get_mono_font() { return mono_font.withHeight(mono_height); }

    static const juce::Font mono_font;
    static const juce::Font text_font;

    [[maybe_unused]] static const juce::Colour blue;
    [[maybe_unused]] static const juce::Colour green;
    [[maybe_unused]] static const juce::Colour grey_dark;
    [[maybe_unused]] static const juce::Colour grey_light;
    [[maybe_unused]] static const juce::Colour grey_medium;
    [[maybe_unused]] static const juce::Colour grey_medium_dark;
    [[maybe_unused]] static const juce::Colour grey_middle;
    [[maybe_unused]] static const juce::Colour grey_middle_light;
    [[maybe_unused]] static const juce::Colour grey_semi_dark;
    [[maybe_unused]] static const juce::Colour grey_semi_light;
    [[maybe_unused]] static const juce::Colour grey_super_light;
    [[maybe_unused]] static const juce::Colour orange;
    [[maybe_unused]] static const juce::Colour red;
    [[maybe_unused]] static const juce::Colour yellow;

    static constexpr float mono_height {15.0f};

private:
    bool dark_theme;
};
}  // namespace juce
