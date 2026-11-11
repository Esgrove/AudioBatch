/// Tests expanded heading and compact BandStrip geometry, typography, and retained control presentation.

#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "tests/CompactBandTestSupport.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/SpatialSpectrogramView.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>
#include <tuple>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Returns the actual left-aligned text area in strip coordinates, including look-and-feel label insets.
juce::Rectangle<int> bandLabelTextArea(juce::Label& label)
{
    return label.getLookAndFeel().getLabelBorderSize(label).subtractedFrom(label.getBounds());
}

/// Checks every expanded child remains visible, inside its strip, and disjoint from all other children.
void expectExpandedBandChildrenDoNotOverlap(const BandStrip& strip)
{
    std::vector<juce::Component*> visible;
    for (auto* child : strip.getChildren()) {
        ASSERT_TRUE(child->isVisible());
        EXPECT_TRUE(strip.getLocalBounds().contains(child->getBounds()));
        for (const auto* previous : visible) {
            EXPECT_FALSE(child->getBounds().intersects(previous->getBounds()));
        }
        visible.push_back(child);
    }
}

/// Exercises grow/shrink boundaries for one heading configuration at minimum and spare strip heights.
void expectExpandedBandHeadingThresholds(
    BandStrip& strip,
    const std::size_t band,
    const juce::String& text,
    const int inset
)
{
    auto* name = bandHeadingLabel(strip, bandName(band));
    auto* range = bandHeadingLabel(strip, text);
    auto* solo = compactControl<juce::Component>(strip, bandName(band) + " Solo");
    auto* link = compactControl<juce::Component>(strip, bandName(band) + " Link");
    auto* mid = compactControl<ControlSlider>(strip, bandName(band) + " Mid");
    auto* side = compactControl<ControlSlider>(strip, bandName(band) + " Side");
    ASSERT_NE(name, nullptr);
    ASSERT_NE(range, nullptr);
    ASSERT_NE(solo, nullptr);
    ASSERT_NE(link, nullptr);
    ASSERT_NE(mid, nullptr);
    ASSERT_NE(side, nullptr);
    const auto nameFont = name->getFont();
    const auto rangeFont = range->getFont();
    const int threshold = bandHeadingInlineWidth(*name, *range, inset);
    for (const int height : {BAND_STRIP_MIN_HEIGHT, BAND_STRIP_MIN_HEIGHT + 80}) {
        strip.setSize(threshold - 1, height);
        const int narrowMidY = mid->getY();
        const int narrowRowSpacing = side->getY() - mid->getY();
        for (const int width : {threshold - 1, threshold, threshold + 1, threshold + 100, threshold - 1}) {
            SCOPED_TRACE(width);
            SCOPED_TRACE(height);
            strip.setSize(width, height);
            const bool inlineRange = width >= threshold;
            EXPECT_EQ(name->getY(), 10);
            EXPECT_EQ(bandLabelTextArea(*name).getX(), 10);
            auto* midLabel = bandHeadingLabel(strip, "Mid");
            ASSERT_NE(midLabel, nullptr);
            EXPECT_EQ(bandLabelTextArea(*midLabel).getX(), 10);
            EXPECT_EQ(bandLabelTextArea(*name).getX(), bandLabelTextArea(*midLabel).getX());
            EXPECT_EQ(bandLabelTextArea(*range).getX(), inlineRange ? name->getRight() + 8 : 10);
            EXPECT_EQ(range->getY(), inlineRange ? name->getY() : name->getBottom());
            EXPECT_EQ(range->getX(), inlineRange ? name->getRight() + 8 : 10);
            EXPECT_EQ(range->getHeight(), 20);
            EXPECT_GE(name->getWidth(), bandHeadingTextWidth(*name));
            EXPECT_GE(range->getWidth(), bandHeadingTextWidth(*range));
            EXPECT_EQ(solo->getRight() + 6, link->getX());
            EXPECT_EQ(link->getRight(), strip.getWidth() - 10 - inset);
            EXPECT_EQ(solo->getWidth(), 20);
            EXPECT_EQ(link->getWidth(), 60);
            EXPECT_EQ(name->getFont(), nameFont);
            EXPECT_EQ(range->getFont(), rangeFont);
            EXPECT_EQ(mid->getHeight(), 26);
            EXPECT_EQ(side->getHeight(), 26);
            EXPECT_EQ(range->findColour(juce::Label::textColourId), juce::Colour(palette::GREY_MIDDLE_LIGHT));
            if (inlineRange) {
                EXPECT_EQ(range->getRight() + 8, solo->getX());
                EXPECT_LT(mid->getY(), narrowMidY);
                EXPECT_GE(side->getY() - mid->getY(), narrowRowSpacing);
                if (height > BAND_STRIP_MIN_HEIGHT) {
                    EXPECT_GT(side->getY() - mid->getY(), narrowRowSpacing);
                }
            } else {
                EXPECT_EQ(mid->getY(), narrowMidY);
                EXPECT_EQ(side->getY() - mid->getY(), narrowRowSpacing);
            }
            expectExpandedBandChildrenDoNotOverlap(strip);
        }
    }
}

/// Measures representative digit widths and endpoints without walking thousands of equivalent numeric strings.
void expectCompactBoundaryValuesFit(ControlSlider& slider)
{
    auto* label = compactValueBox(slider);
    ASSERT_NE(label, nullptr);
    const auto font = label->getLookAndFeel().getLabelFont(*label);
    std::array values {-60.0, -59.99, -59.95, -11.11, -10.0, -9.99, -0.01, 0.0, 9.99, 11.11, 12.0};
    if (slider.getName() == "Width") {
        values = {0.0, 0.01, 0.09, 0.10, 0.99, 1.0, 1.01, 1.11, 1.88, 1.99, 2.0};
    } else if (slider.getName() == "Pan") {
        values = {-100.0, -99.9, -88.0, -33.0, -25.0, 0.0, 1.0, 25.0, 33.0, 99.9, 100.0};
    }
    for (const double value : values) {
        slider.setValue(value, juce::dontSendNotification);
        const auto textWidth = juce::GlyphArrangement::getStringWidth(font, slider.compactValueText());
        EXPECT_LE(textWidth, static_cast<float>(slider.getWidth() - 4));
    }
    EXPECT_EQ(slider.getComponentAt(slider.getLocalBounds().getCentre()), &slider);
}
}  // namespace

/// One compact row fits four nonoverlapping fields and worst-case glyphs without any separate band or control labels.
TEST(CompactBands, MinimumWidthGeometryAndBoundaryValuesRemainLegible)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        BandStrip strip(processor.getParameters(), processor.getEditorState(), band, processor.getHistory());
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(true);
        strip.setHeaderDisclosureInset(24);
        EXPECT_EQ(BAND_STRIP_COMPACT_HEIGHT, 28);
        for (const auto size : {juce::Point<int> {180, 28}, {181, 28}, {240, 28}, {400, 28}}) {
            SCOPED_TRACE(band);
            SCOPED_TRACE(size.toString().toStdString());
            strip.setSize(size.x, size.y);
            const auto title = bandName(band);
            auto* mid = compactControl<ControlSlider>(strip, title + " Mid");
            ASSERT_NE(mid, nullptr);
            int previousRight = 0;
            for (const auto* name : {"Mid", "Side", "Width", "Pan"}) {
                auto* slider = compactControl<ControlSlider>(strip, title + " " + name);
                ASSERT_NE(slider, nullptr);
                EXPECT_TRUE(slider->isVisible());
                EXPECT_EQ(slider->getY(), mid->getY());
                EXPECT_EQ(slider->getY(), 2);
                EXPECT_EQ(slider->getHeight(), 24);
                EXPECT_GT(slider->getX(), previousRight);
                EXPECT_GE(slider->getWidth(), slider->getName() == "Pan" ? 30 : 40);
                previousRight = slider->getRight();
                expectCompactBoundaryValuesFit(*slider);
            }
            EXPECT_EQ(strip.getWidth() - previousRight, 2);
            int visibleControls = 0;
            bool foundName = false;
            for (auto* child : strip.getChildren()) {
                if (const auto* label = dynamic_cast<juce::Label*>(child)) {
                    EXPECT_FALSE(label->isVisible());
                    foundName = foundName || label->getText() == title;
                }
                if (child->isVisible()) {
                    ++visibleControls;
                    EXPECT_TRUE(strip.getLocalBounds().contains(child->getBounds()));
                }
            }
            EXPECT_EQ(visibleControls, 4);
            EXPECT_TRUE(foundName);
        }
    }
}

/// Full heading glyphs select one or two rows at the exact measured boundary, with room for buttons and disclosure.
TEST(CompactBands, ExpandedHeadingFitUsesActualTextFontsAndReservedControls)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        for (const auto* text : {"Below 120 Hz", "1.25 kHz to 16.00 kHz"}) {
            for (const int inset : {0, 24}) {
                SCOPED_TRACE(band);
                SCOPED_TRACE(text);
                SCOPED_TRACE(inset);
                BandStrip strip(processor.getParameters(), processor.getEditorState(), band, processor.getHistory());
                strip.setLookAndFeel(&lookAndFeel);
                strip.setHeaderDisclosureInset(inset);
                strip.setRangeText(text);
                expectExpandedBandHeadingThresholds(strip, band, text, inset);
            }
        }
    }
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Band names retain an explicit embedded semibold face and fixed height through responsive and compact layouts.
TEST(CompactBands, ExpandedNamesUseEmbeddedSemiboldAtExistingHeight)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    const auto semibold = semiboldTextFont(16.0F);
    ASSERT_NE(semibold.getTypefacePtr(), nullptr);
    EXPECT_NE(semibold.getTypefacePtr(), textFont(16.0F).getTypefacePtr());
    EXPECT_TRUE(semibold.getTypefacePtr()->getName().containsIgnoreCase("Roboto"));
    const auto faceIdentity = semibold.getTypefacePtr()->getName() + " " + semibold.getTypefacePtr()->getStyle();
    EXPECT_TRUE(faceIdentity.containsIgnoreCase("SemiBold"));
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        BandStrip strip(processor.getParameters(), processor.getEditorState(), band, processor.getHistory());
        strip.setLookAndFeel(&lookAndFeel);
        strip.setRangeText("1.25 kHz to 16.00 kHz");
        auto* name = bandHeadingLabel(strip, bandName(band));
        auto* range = bandHeadingLabel(strip, "1.25 kHz to 16.00 kHz");
        ASSERT_NE(name, nullptr);
        ASSERT_NE(range, nullptr);
        const int threshold = bandHeadingInlineWidth(*name, *range, 0);
        for (const bool compact : {false, true, false}) {
            strip.setCompact(compact);
            for (const int width : {threshold - 1, threshold, threshold + 80}) {
                strip.setSize(width, compact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
                const auto drawingFont = lookAndFeel.getLabelFont(*name);
                EXPECT_EQ(name->getFont(), semibold);
                EXPECT_EQ(drawingFont.getTypefacePtr(), semibold.getTypefacePtr());
                EXPECT_EQ(drawingFont.getStyleFlags(), semibold.getStyleFlags());
                EXPECT_FLOAT_EQ(drawingFont.getHeight(), 16.0F);
                EXPECT_FLOAT_EQ(lookAndFeel.getLabelFont(*range).getHeight(), 14.0F);
            }
        }
    }
}

/// Solo's artwork is smaller in every visual state while its hit target, focus, and host attachment stay intact.
TEST(CompactBands, SoloArtworkKeepsTwentyUnitHitTargetAndAutomation)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        BandStrip strip(processor.getParameters(), processor.getEditorState(), band, processor.getHistory());
        strip.setLookAndFeel(&lookAndFeel);
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        strip.setVisible(true);
        auto* solo = compactControl<juce::DrawableButton>(strip, bandName(band) + " Solo");
        ASSERT_NE(solo, nullptr);
        EXPECT_EQ(solo->getLocalBounds(), juce::Rectangle<int>(20, 20));
        EXPECT_EQ(solo->getImageBounds(), juce::Rectangle<float>(2.0F, 2.0F, 16.0F, 16.0F));
        EXPECT_TRUE(solo->getWantsKeyboardFocus());
        EXPECT_EQ(solo->getExplicitFocusOrder(), 1);
        EXPECT_NE(lookAndFeel.createFocusOutlineForComponent(*solo), nullptr);
        for (const auto point : {juce::Point<int> {0, 0}, {19, 0}, {0, 19}, {19, 19}, {10, 10}}) {
            EXPECT_EQ(strip.getComponentAt(solo->getPosition() + point), solo);
        }
        auto& parameter = *processor.getParameters().getParameter(soloParameterId(band));
        for (const bool enabled : {false, true, false}) {
            parameter.setValueNotifyingHost(enabled ? 1.0F : 0.0F);
            EXPECT_EQ(solo->getToggleState(), enabled);
            for (const auto state : {juce::Button::buttonNormal, juce::Button::buttonOver, juce::Button::buttonDown}) {
                solo->setState(state);
                const auto* image = solo->getCurrentImage();
                ASSERT_NE(image, nullptr);
                const auto art = image->getOutlineAsPath().getBounds();
                EXPECT_NEAR(art.getWidth(), 16.0F, 0.00001F);
                EXPECT_GE(art.getHeight(), 15.0F);
                EXPECT_LE(art.getHeight(), 16.0F);
                EXPECT_NEAR(art.getCentreX(), 10.0F, 0.001F);
                EXPECT_NEAR(art.getCentreY(), 10.0F, 0.001F);
            }
        }
    }
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Surface rounding stays centralized, scale-independent, and well below half the height of ordinary controls.
TEST(CompactBands, SharedRadiiKeepPanelsPlotMasksAndControlsProportionate)
{
    EXPECT_FLOAT_EQ(corner_radius::PANEL, 8.0F);
    EXPECT_FLOAT_EQ(corner_radius::PLOT, 6.0F);
    EXPECT_FLOAT_EQ(SPECTROGRAM_PLOT_CORNER_RADIUS, corner_radius::PLOT);
    EXPECT_FLOAT_EQ(corner_radius::CONTROL, 4.0F);
    EXPECT_FLOAT_EQ(corner_radius::BUTTON, 7.0F);
    EXPECT_FLOAT_EQ(corner_radius::CALLOUT, 10.0F);
    EXPECT_LT(corner_radius::CONTROL, 24.0F * 0.5F);
    EXPECT_LT(corner_radius::BUTTON, 20.0F * 0.5F);
    EXPECT_LT(corner_radius::PLOT, corner_radius::PANEL);
    ImageControlLookAndFeel lookAndFeel;
    juce::Component content;
    content.setSize(240, 120);
    const juce::CallOutBox callout(content, juce::Rectangle<int>(10, 10, 20, 20), nullptr);
    EXPECT_FLOAT_EQ(lookAndFeel.getCallOutBoxCornerSize(callout), corner_radius::CALLOUT);
}

/// Live range changes reflow expanded headings at unchanged strip size, and never change compact geometry or names.
TEST(CompactBands, LiveHeadingTextChangesReflowWithoutTogglingCompactControls)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 1, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setHeaderDisclosureInset(24);
    strip.setRangeText("120 Hz to 800 Hz");
    auto* name = bandHeadingLabel(strip, "Low Mid");
    auto* range = bandHeadingLabel(strip, "120 Hz to 800 Hz");
    ASSERT_NE(name, nullptr);
    ASSERT_NE(range, nullptr);
    const int shortThreshold = bandHeadingInlineWidth(*name, *range, 24);
    strip.setRangeText("1.25 kHz to 16.00 kHz");
    const int longThreshold = bandHeadingInlineWidth(*name, *range, 24);
    ASSERT_GT(longThreshold, shortThreshold);
    strip.setSize(shortThreshold, BAND_STRIP_MIN_HEIGHT + 80);
    for (const bool compact : {false, true, false}) {
        strip.setCompact(compact);
        strip.setSize(shortThreshold, compact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT + 80);
        std::vector<std::tuple<juce::Component*, juce::Rectangle<int>, bool>> geometry;
        for (auto* child : strip.getChildren()) {
            geometry.emplace_back(child, child->getBounds(), child->isVisible());
        }
        auto* mid = compactControl<ControlSlider>(strip, "Low Mid Mid");
        ASSERT_NE(mid, nullptr);
        const auto midText = mid->compactValueText();
        for (const auto* text : {"120 Hz to 800 Hz", "1.25 kHz to 16.00 kHz", "120 Hz to 800 Hz"}) {
            strip.setRangeText(text);
            EXPECT_EQ(range->getText(), text);
            if (compact) {
                for (const auto& [child, bounds, visible] : geometry) {
                    EXPECT_EQ(child->getBounds(), bounds);
                    EXPECT_EQ(child->isVisible(), visible);
                }
                EXPECT_EQ(mid->compactValueText(), midText);
            } else {
                EXPECT_TRUE(name->isVisible());
                EXPECT_TRUE(range->isVisible());
                EXPECT_EQ(range->getY(), juce::String(text) == "120 Hz to 800 Hz" ? name->getY() : name->getBottom());
                const auto bounds = range->getBounds();
                const auto midBounds = mid->getBounds();
                strip.setRangeText(text);
                EXPECT_EQ(range->getBounds(), bounds);
                EXPECT_EQ(mid->getBounds(), midBounds);
            }
        }
    }
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Width alone selects short suffixes or inline full names without showing another label row or replacing controls.
TEST(CompactBands, SuffixesAndInlineFullNamesIdentifyAllFourValues)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    const std::array titles {"Low Mid", "Low Side", "Low Width", "Low Pan"};
    const std::array shortValues {"+0.5M", "-1.2S", "1.00W", "C"};
    const std::array fullValues {"+0.5 MID", "-1.2 SIDE", "1.00 WIDTH", "C PAN"};
    std::array<juce::Component::SafePointer<ControlSlider>, 4> controls;
    int fullNameWidth = 4 + 3 * 2;
    for (std::size_t index = 0; index < titles.size(); ++index) {
        auto* slider = compactControl<ControlSlider>(strip, titles.at(index));
        ASSERT_NE(slider, nullptr);
        controls.at(index) = slider;
        fullNameWidth += slider->compactValueRequiredWidth(true);
        if (slider->getName() == "Mid") {
            slider->setValue(0.5, juce::dontSendNotification);
        } else if (slider->getName() == "Side") {
            slider->setValue(-1.2, juce::dontSendNotification);
        }
    }
    EXPECT_GT(fullNameWidth, 180);
    EXPECT_LE(fullNameWidth, 400);
    for (const int width : {180, 240, 400, 240, 180}) {
        SCOPED_TRACE(width);
        strip.setSize(width, BAND_STRIP_COMPACT_HEIGHT);
        const auto& expected = width >= fullNameWidth ? fullValues : shortValues;
        for (std::size_t index = 0; index < titles.size(); ++index) {
            auto* slider = compactControl<ControlSlider>(strip, titles.at(index));
            ASSERT_EQ(slider, controls.at(index).getComponent());
            EXPECT_EQ(slider->compactValueText(), expected.at(index));
            EXPECT_EQ(slider->getY(), 2);
            EXPECT_EQ(slider->getHeight(), 24);
            if (width == 400) {
                EXPECT_GE(slider->getWidth(), slider->compactValueRequiredWidth(true));
            }
        }
        for (auto* child : strip.getChildren()) {
            if (dynamic_cast<juce::Label*>(child) != nullptr) {
                EXPECT_FALSE(child->isVisible());
            }
        }
    }
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// All four controls change naming mode together at the measured worst-case width, never from automated values.
TEST(CompactBands, FullNamesReadinessUsesWorstCaseGlyphsAndRemainsStableUnderAutomation)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    const std::array titles {"Low Mid", "Low Side", "Low Width", "Low Pan"};
    const std::array parameterIds {
        midGainParameterId(0),
        sideGainParameterId(0),
        widthParameterId(0),
        panParameterId(0),
    };
    const std::array shortSuffixes {"M", "S", "W", ""};
    const std::array fullSuffixes {" MID", " SIDE", " WIDTH", " PAN"};
    int fullNameWidth = 4 + 3 * 2;
    for (const auto* title : titles) {
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        fullNameWidth += slider->compactValueRequiredWidth(true);
    }
    for (const int width : {fullNameWidth - 1, fullNameWidth, fullNameWidth + 1}) {
        SCOPED_TRACE(width);
        strip.setSize(width, BAND_STRIP_COMPACT_HEIGHT);
        const bool fullNames = width >= fullNameWidth;
        const auto& suffixes = fullNames ? fullSuffixes : shortSuffixes;
        for (std::size_t index = 0; index < titles.size(); ++index) {
            auto* slider = compactControl<ControlSlider>(strip, titles.at(index));
            ASSERT_NE(slider, nullptr);
            auto& parameter = *processor.getParameters().getParameter(parameterIds.at(index));
            for (const float value : {0.0F, 0.5F, 1.0F}) {
                parameter.setValueNotifyingHost(value);
                EXPECT_TRUE(slider->compactValueText().endsWith(suffixes.at(index)));
                if (!fullNames) {
                    EXPECT_FALSE(slider->compactValueText().contains(" "));
                }
                EXPECT_GE(slider->getWidth(), slider->compactValueRequiredWidth(fullNames));
            }
            expectCompactBoundaryValuesFit(*slider);
        }
    }
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// A mode round trip preserves all existing controls, identities, full units, tooltips, and expanded geometry.
TEST(CompactBands, FullLayoutRestoresWithoutReplacingControlsOrAttachments)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setRangeText("20 Hz - 120 Hz");
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    std::vector<std::pair<juce::Component::SafePointer<juce::Component>, juce::Rectangle<int>>> bounds;
    std::vector<std::pair<juce::Component::SafePointer<juce::Label>, juce::Font>> labelFonts;
    std::vector<std::pair<juce::Component::SafePointer<ControlSlider>, juce::Font>> valueFonts;
    for (auto* child : strip.getChildren()) {
        bounds.emplace_back(child, child->getBounds());
        if (auto* label = dynamic_cast<juce::Label*>(child)) {
            labelFonts.emplace_back(label, label->getFont());
        }
        if (auto* slider = dynamic_cast<ControlSlider*>(child)) {
            auto* label = compactValueBox(*slider);
            ASSERT_NE(label, nullptr);
            valueFonts.emplace_back(slider, label->getFont());
        }
    }
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    auto* side = compactControl<ControlSlider>(strip, "Low Side");
    auto* width = compactControl<ControlSlider>(strip, "Low Width");
    ASSERT_NE(mid, nullptr);
    ASSERT_NE(side, nullptr);
    ASSERT_NE(width, nullptr);
    const auto tooltip = mid->getTooltip();
    const auto componentId = mid->getComponentID();
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    EXPECT_EQ(mid, compactControl<ControlSlider>(strip, "Low Mid"));
    EXPECT_EQ(mid->getComponentID(), componentId);
    EXPECT_EQ(mid->compactValueText(), "+0.0M");
    EXPECT_EQ(side->compactValueText(), "+0.0S");
    EXPECT_EQ(width->compactValueText(), "1.00W");
    EXPECT_TRUE(mid->getTooltip().contains("Return"));
    EXPECT_TRUE(mid->getTooltip().contains("dB"));
    EXPECT_TRUE(width->getTooltip().contains("multiplier"));
    mid->setValue(static_cast<double>(BAND_GAIN_MIN_DB), juce::dontSendNotification);
    EXPECT_EQ(mid->compactValueText(), "-infM");
    EXPECT_EQ(mid->getTextFromValue(mid->getValue()), "-inf dB");
    for (const auto* title : {"Low Recover", "Low Solo", "Low Link"}) {
        auto* control = compactControl<juce::Component>(strip, title);
        ASSERT_NE(control, nullptr);
        EXPECT_FALSE(control->isVisible());
    }
    strip.setCompact(false);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    for (const auto& [child, originalBounds] : bounds) {
        ASSERT_NE(child, nullptr);
        EXPECT_TRUE(child->isVisible());
        EXPECT_EQ(child->getBounds(), originalBounds);
    }
    for (const auto& [label, originalFont] : labelFonts) {
        ASSERT_NE(label, nullptr);
        EXPECT_EQ(label->getFont(), originalFont);
    }
    for (const auto& [slider, originalFont] : valueFonts) {
        ASSERT_NE(slider, nullptr);
        auto* label = compactValueBox(*slider);
        ASSERT_NE(label, nullptr);
        EXPECT_EQ(label->getFont(), originalFont);
    }
    EXPECT_EQ(mid->getTooltip(), tooltip);
    EXPECT_EQ(mid->getTextBoxWidth(), 78);
    EXPECT_EQ(mid->getTextBoxHeight(), 22);
    EXPECT_EQ(mid->getTextBoxPosition(), juce::Slider::TextBoxRight);
    EXPECT_EQ(mid->getTextFromValue(mid->getValue()), "-inf dB");
    EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_GT(compactParameterValue(processor, midGainParameterId(0)), BAND_GAIN_MIN_DB);
}
}  // namespace esgrove::image_control::test
