/// Declares ImageControlLookAndFeel, the look and feel of the ImageControl editor,
/// and the palette it shares with the AudioBatch application:
/// the same greys, accent colours, and embedded Roboto fonts.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

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
}  // namespace palette

/// The editor's look and feel: AudioBatch's dark colour scheme and control colours,
/// Roboto for text, and Roboto Mono for the value boxes of sliders so numbers line up.
class ImageControlLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ImageControlLookAndFeel();

    [[nodiscard]] static juce::Font getMonoFont(float height);
    [[nodiscard]] static juce::Font getTextFont(float height);

    [[nodiscard]] juce::Font getLabelFont(juce::Label& label) override;
    [[nodiscard]] juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;
};
}  // namespace esgrove::image_control
