/// Declares ImageControlLookAndFeel, the look and feel of the ImageControl editor,
/// and the palette it shares with the AudioBatch application:
/// the same greys, accent colours, and embedded Roboto fonts.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace esgrove::image_control
{
/// Opaque ARGB values of the palette, the same as AudioBatch's palette.
namespace palette
{
constexpr juce::uint32 BLUE {0xff76A8DA};
constexpr juce::uint32 GREY_DARK {0xff121315};
constexpr juce::uint32 GREY_LIGHT {0xffDCE0E5};
constexpr juce::uint32 GREY_MEDIUM {0xff30343C};
constexpr juce::uint32 GREY_MEDIUM_DARK {0xff202329};
constexpr juce::uint32 GREY_MIDDLE {0xff686F79};
constexpr juce::uint32 GREY_MIDDLE_LIGHT {0xffAAB0B9};
constexpr juce::uint32 GREY_SEMI_DARK {0xff1A1C20};
constexpr juce::uint32 GREY_SUPER_LIGHT {0xffF3F5F7};
constexpr juce::uint32 ORANGE {0xffDD8B65};
constexpr juce::uint32 YELLOW {0xffCEAF6C};
}  // namespace palette

[[nodiscard]] juce::Font monoFont(float height);
[[nodiscard]] juce::Font textFont(float height);

/// The editor's look and feel: AudioBatch's dark colour scheme and control colours,
/// Roboto for text, and Roboto Mono for the value boxes of sliders so numbers line up.
class ImageControlLookAndFeel final : public juce::LookAndFeel_V4, private juce::FocusChangeListener
{
public:
    ImageControlLookAndFeel();

    ~ImageControlLookAndFeel() override;

    ImageControlLookAndFeel(const ImageControlLookAndFeel&) = delete;
    ImageControlLookAndFeel(ImageControlLookAndFeel&&) = delete;
    ImageControlLookAndFeel& operator=(const ImageControlLookAndFeel&) = delete;
    ImageControlLookAndFeel& operator=(ImageControlLookAndFeel&&) = delete;

    [[nodiscard]] std::unique_ptr<juce::FocusOutline> createFocusOutlineForComponent(
        juce::Component& component
    ) override;
    [[nodiscard]] juce::Font getLabelFont(juce::Label& label) override;
    [[nodiscard]] juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;

    [[nodiscard]] juce::Font getAlertWindowFont() override;
    [[nodiscard]] juce::Font getAlertWindowMessageFont() override;
    [[nodiscard]] juce::Font getAlertWindowTitleFont() override;
    [[nodiscard]] int getAlertWindowButtonHeight() override;
    void drawAlertBox(
        juce::Graphics& graphics,
        juce::AlertWindow& alert,
        const juce::Rectangle<int>& textArea,
        juce::TextLayout& textLayout
    ) override;

private:
    void globalFocusChanged(juce::Component* focusedComponent) override;
};
}  // namespace esgrove::image_control
