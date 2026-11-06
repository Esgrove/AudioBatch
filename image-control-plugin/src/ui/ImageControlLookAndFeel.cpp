/// Implements ImageControlLookAndFeel: the AudioBatch colour scheme and control colours,
/// and the Roboto fonts embedded from the repository's shared assets.

#include "ui/ImageControlLookAndFeel.h"

#include "ImageControlBinaryData.h"

#include <algorithm>

namespace esgrove::image_control
{
namespace
{
/// Opacity of the slider track fill, as in AudioBatch.
constexpr float SLIDER_TRACK_ALPHA {0.7F};
/// Opacity of the text of a text button that is off, as in AudioBatch.
constexpr float TEXT_BUTTON_OFF_TEXT_ALPHA {0.96F};
/// Opacity of the text editor selection, as in AudioBatch.
constexpr float TEXT_EDITOR_HIGHLIGHT_ALPHA {0.24F};
/// Largest font height of a text button.
constexpr float MAX_BUTTON_FONT_HEIGHT {15.0F};
/// Font height of a text button relative to its height.
constexpr float BUTTON_FONT_HEIGHT_RATIO {0.55F};

/// Returns the embedded Roboto typeface, created on first use.
const juce::Typeface::Ptr& textTypeface()
{
    static const juce::Typeface::Ptr typeface = juce::Typeface::createSystemTypefaceFor(
        ImageControlBinaryData::RobotoRegular_ttf, ImageControlBinaryData::RobotoRegular_ttfSize
    );
    return typeface;
}

/// Returns the embedded Roboto Mono typeface, created on first use.
const juce::Typeface::Ptr& monoTypeface()
{
    static const juce::Typeface::Ptr typeface = juce::Typeface::createSystemTypefaceFor(
        ImageControlBinaryData::RobotoMonoRegular_ttf, ImageControlBinaryData::RobotoMonoRegular_ttfSize
    );
    return typeface;
}

/// Returns a colour of the palette.
juce::Colour colour(const juce::uint32 argb)
{
    return juce::Colour(argb);
}
}  // namespace

/// Applies AudioBatch's dark colour scheme, its slider, button, label, and text editor colours,
/// and Roboto as the default typeface.
ImageControlLookAndFeel::ImageControlLookAndFeel()
{
    setColourScheme({
        // windowBackground
        palette::GREY_DARK,
        // widgetBackground
        palette::GREY_SEMI_DARK,
        // menuBackground
        palette::GREY_MEDIUM_DARK,
        // outline
        palette::GREY_MEDIUM,
        // defaultText
        palette::GREY_LIGHT,
        // defaultFill
        palette::GREY_MIDDLE,
        // highlightedText
        palette::GREY_SUPER_LIGHT,
        // highlightedFill
        palette::BLUE,
        // menuText
        palette::GREY_LIGHT,
    });

    setColour(juce::ResizableWindow::backgroundColourId, colour(palette::GREY_DARK));
    setColour(juce::TextButton::buttonColourId, colour(palette::GREY_MEDIUM));
    setColour(juce::TextButton::buttonOnColourId, colour(palette::BLUE));
    setColour(
        juce::TextButton::textColourOffId, colour(palette::GREY_SUPER_LIGHT).withAlpha(TEXT_BUTTON_OFF_TEXT_ALPHA)
    );
    setColour(juce::TextButton::textColourOnId, colour(palette::GREY_SUPER_LIGHT));
    setColour(juce::Slider::backgroundColourId, colour(palette::GREY_MEDIUM));
    setColour(juce::Slider::trackColourId, colour(palette::BLUE).withAlpha(SLIDER_TRACK_ALPHA));
    setColour(juce::Slider::thumbColourId, colour(palette::GREY_SUPER_LIGHT));
    setColour(juce::Slider::textBoxBackgroundColourId, colour(palette::GREY_MEDIUM_DARK));
    setColour(juce::Slider::textBoxTextColourId, colour(palette::GREY_SUPER_LIGHT));
    setColour(juce::Slider::textBoxOutlineColourId, colour(palette::GREY_MEDIUM));
    setColour(juce::Label::textColourId, colour(palette::GREY_LIGHT));
    setColour(juce::ToggleButton::textColourId, colour(palette::GREY_LIGHT));
    setColour(juce::ToggleButton::tickColourId, colour(palette::BLUE));
    setColour(juce::ToggleButton::tickDisabledColourId, colour(palette::GREY_MIDDLE));
    setColour(juce::TextEditor::backgroundColourId, colour(palette::GREY_SEMI_DARK));
    setColour(juce::TextEditor::highlightColourId, colour(palette::BLUE).withAlpha(TEXT_EDITOR_HIGHLIGHT_ALPHA));
    setColour(juce::TextEditor::outlineColourId, colour(palette::GREY_MEDIUM));
    setColour(juce::CaretComponent::caretColourId, colour(palette::BLUE));
    setColour(juce::TooltipWindow::backgroundColourId, colour(palette::GREY_MEDIUM_DARK));
    setColour(juce::TooltipWindow::textColourId, colour(palette::GREY_SUPER_LIGHT));

    setDefaultSansSerifTypeface(textTypeface());
}

/// Returns Roboto Mono at a height.
juce::Font ImageControlLookAndFeel::getMonoFont(const float height)
{
    return juce::Font(juce::FontOptions(monoTypeface())).withHeight(height);
}

/// Returns Roboto at a height.
juce::Font ImageControlLookAndFeel::getTextFont(const float height)
{
    return juce::Font(juce::FontOptions(textTypeface())).withHeight(height);
}

/// Uses Roboto Mono for the value boxes of sliders, so their numbers line up, and Roboto for other labels.
juce::Font ImageControlLookAndFeel::getLabelFont(juce::Label& label)
{
    const float height = label.getFont().getHeight();
    if (dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr) {
        return getMonoFont(height);
    }
    return label.getFont();
}

/// Uses Roboto for text buttons, scaled to the button like AudioBatch's buttons.
juce::Font ImageControlLookAndFeel::getTextButtonFont(juce::TextButton& /*button*/, const int buttonHeight)
{
    return getTextFont(std::min(MAX_BUTTON_FONT_HEIGHT, static_cast<float>(buttonHeight) * BUTTON_FONT_HEIGHT_RATIO));
}
}  // namespace esgrove::image_control
