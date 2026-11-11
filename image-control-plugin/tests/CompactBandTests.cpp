/// Tests expanded heading and compact BandStrip geometry, plus production pointer, wheel, key, and numeric input.
/// The same parameter attachments must preserve local history, Link, automation exclusion, and balanced host gestures.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/DisplayDisclosure.h"
#include "ui/HistoryAttachments.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/SpatialSpectrogramView.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Observes one parameter's host gestures and always detaches before its processor is destroyed.
class CompactGestureCounter final : private juce::AudioProcessorParameter::Listener
{
public:
    /// Observes a processor-owned parameter that outlives this counter.
    explicit CompactGestureCounter(juce::RangedAudioParameter& observed) : parameter(observed)
    {
        parameter.addListener(this);
    }

    /// Removes the observer even when an assertion returns from the test early.
    ~CompactGestureCounter() override
    {
        parameter.removeListener(this);
    }

    CompactGestureCounter(const CompactGestureCounter&) = delete;
    CompactGestureCounter(CompactGestureCounter&&) = delete;
    CompactGestureCounter& operator=(const CompactGestureCounter&) = delete;
    CompactGestureCounter& operator=(CompactGestureCounter&&) = delete;

    /// Returns host begin and end notifications in delivery order.
    [[nodiscard]] const std::vector<bool>& gestureBoundaries() const
    {
        return boundaries;
    }

private:
    /// Value expectations are checked independently against the processor's plain parameter values.
    void parameterValueChanged(int /*index*/, float /*value*/) override {}

    /// Records each host gesture boundary in delivery order.
    void parameterGestureChanged(int /*index*/, const bool beginning) override
    {
        boundaries.push_back(beginning);
    }

    /// Processor-owned parameter that remains valid until this observer detaches.
    juce::RangedAudioParameter& parameter;
    /// Ordered host begin and end notifications.
    mutable std::vector<bool> boundaries;
};

/// Finds a direct child by full accessible title, independent of child ordering.
/// The returned observer can be null and is valid only while the parent owns the component.
template<typename Control>
Control* compactControl(const juce::Component& parent, const juce::String& title)
{
    for (auto* child : parent.getChildren()) {
        if (child->getTitle() == title) {
            return dynamic_cast<Control*>(child);
        }
    }
    return nullptr;
}

/// Finds a strip-owned heading by its displayed text, valid until the strip is destroyed.
juce::Label* bandHeadingLabel(const BandStrip& strip, const juce::String& text)
{
    for (auto* child : strip.getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child); label != nullptr && label->getText() == text) {
            return label;
        }
    }
    return nullptr;
}

/// Measures uncompressed heading text and the actual look-and-feel border at its fixed display font.
int bandHeadingTextWidth(juce::Label& label)
{
    const auto font = label.getLookAndFeel().getLabelFont(label);
    const int glyphWidth = static_cast<int>(std::ceil(juce::GlyphArrangement::getStringWidth(font, label.getText())));
    return glyphWidth + label.getLookAndFeel().getLabelBorderSize(label).getLeftAndRight();
}

/// Returns the actual left-aligned text area in strip coordinates, including look-and-feel label insets.
juce::Rectangle<int> bandLabelTextArea(juce::Label& label)
{
    return label.getLookAndFeel().getLabelBorderSize(label).subtractedFrom(label.getBounds());
}

/// Includes both margins, Solo, Link, their gap, and two readable inline text gaps in the fit boundary.
int bandHeadingInlineWidth(juce::Label& name, juce::Label& range, const int disclosureInset)
{
    return 20 + 20 + 60 + 6 + 8 * 2 + disclosureInset + bandHeadingTextWidth(name) + bandHeadingTextWidth(range);
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

/// Finds the slider's JUCE-owned numeric label, valid until its presentation or look and feel changes.
juce::Label* compactValueBox(const ControlSlider& slider)
{
    for (auto* child : slider.getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child)) {
            return label;
        }
    }
    return nullptr;
}

/// Delivers native-editor Return messages and queued parameter synchronization with a bounded native pump.
void dispatchCompactChanges()
{
#if JUCE_MAC
    for (int pass = 0; pass < 8; ++pass) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, static_cast<Boolean>(true));
    }
#elif JUCE_WINDOWS
    MSG message {};
    for (int pass = 0; pass < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0; ++pass) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#endif
}

/// Creates production-handler pointer input with stable local pointer-down coordinates.
juce::MouseEvent compactMouse(
    juce::Component& control,
    const juce::Point<float> position = {12.0F, 12.0F},
    const juce::Point<float> down = {12.0F, 12.0F},
    const juce::ModifierKeys modifiers = juce::ModifierKeys::leftButtonModifier
)
{
    const auto now = juce::Time::getCurrentTime();
    return {
        juce::Desktop::getInstance().getMainMouseSource(),
        position,
        modifiers,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        &control,
        &control,
        now,
        down,
        now,
        1,
        position != down,
    };
}

/// Clicks through the real window peer so hit testing, focus, label release, and slider listeners all participate.
bool clickCompactComponent(juce::Component& root, juce::Component& target)
{
    auto* peer = root.getPeer();
    if (peer == nullptr || !target.isShowing()) {
        return false;
    }
    const auto position = root.getLocalPoint(&target, target.getLocalBounds().getCentre()).toFloat();
    peer->grabFocus();
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 1000.0;
    while (!peer->contains(position.roundToInt(), true) && juce::Time::getMillisecondCounterHiRes() < deadline) {
        dispatchCompactChanges();
    }
    if (root.getComponentAt(position) != &target || !peer->contains(position.roundToInt(), true)) {
        return false;
    }
    const juce::ScopedValueSetter<juce::ModifierKeys> restoreModifiers(juce::ModifierKeys::currentModifiers, {});
    auto eventTime = juce::Time::currentTimeMillis();
    for (const juce::ModifierKeys modifiers :
         {juce::ModifierKeys {}, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), juce::ModifierKeys {}})
    {
        juce::ModifierKeys::currentModifiers = modifiers;
        peer->handleMouseEvent(
            juce::MouseInputSource::InputSourceType::mouse,
            position,
            modifiers,
            juce::MouseInputSource::defaultPressure,
            juce::MouseInputSource::defaultOrientation,
            eventTime
        );
        eventTime += 20;
    }
    return true;
}

/// Produces a single positive mouse-wheel notch regardless of platform wheel calibration.
juce::MouseWheelDetails compactNotch()
{
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = 60.0F / 256.0F;
    return wheel;
}

/// Waits for native editor callbacks without assuming one platform event pump delivers asynchronous messages.
bool waitForCompactEditorClose(const juce::Label& label)
{
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
    do {
        dispatchCompactChanges();
        if (!label.isBeingEdited()) {
            return true;
        }
    } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    return false;
}

/// Opens the real JUCE text editor with Return and commits through its own Return-key handling.
bool enterCompactValue(ControlSlider& slider, const juce::String& text)
{
    if (!slider.keyPressed(juce::KeyPress(juce::KeyPress::returnKey))) {
        return false;
    }
    const auto* label = compactValueBox(slider);
    if (label == nullptr || label->getCurrentTextEditor() == nullptr) {
        return false;
    }
    auto* editor = label->getCurrentTextEditor();
    editor->setText(text, false);
    // Return posts a message even when the editor leaves the key available to its parent.
    editor->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    if (waitForCompactEditorClose(*label)) {
        return true;
    }
    slider.hideTextBox(true);
    return false;
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

/// Returns exact-entry text and expected plain values for each compact control without duplicating its parser.
std::pair<juce::String, double> compactEntryExample(const ControlSlider& slider)
{
    if (slider.getName() == "Width") {
        return {"1.37", 1.37};
    }
    if (slider.getName() == "Pan") {
        return {"25L", -25.0};
    }
    return {"-3.27", -3.27};
}

/// Returns native default text and plain values for all four compact controls.
std::pair<juce::String, double> compactDefaultExample(const ControlSlider& slider)
{
    if (slider.getName() == "Width") {
        return {"1.00", 1.0};
    }
    if (slider.getName() == "Pan") {
        return {"C", 0.0};
    }
    return {"0.0 dB", 0.0};
}

/// Reads the processor's current plain value rather than relying on a slider's cached representation.
float compactParameterValue(ImageControlProcessor& processor, const juce::String& parameterId)
{
    return plainValue(*processor.getParameters().getParameter(parameterId));
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

/// Heading row changes retain uncommitted numeric text and linked Mid/Side gesture ownership through grow and shrink.
TEST(CompactBands, ResponsiveHeadingPreservesNumericDraftAndLinkedGestures)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setLookAndFeel(&lookAndFeel);
    strip.setRangeText("Below 120 Hz");
    auto* name = bandHeadingLabel(strip, "Low");
    auto* range = bandHeadingLabel(strip, "Below 120 Hz");
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    ASSERT_NE(name, nullptr);
    ASSERT_NE(range, nullptr);
    ASSERT_NE(mid, nullptr);
    const int threshold = bandHeadingInlineWidth(*name, *range, 0);
    strip.setSize(threshold, BAND_STRIP_MIN_HEIGHT + 80);
    strip.addToDesktop(0);
    strip.setVisible(true);
    const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
    const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
    ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    const auto* label = compactValueBox(*mid);
    ASSERT_NE(label, nullptr);
    const juce::Component::SafePointer<juce::TextEditor> editor(label->getCurrentTextEditor());
    ASSERT_NE(editor, nullptr);
    editor->setText("-3.27", false);
    editor->setHighlightedRegion({1, 4});
    for (const int width : {threshold, threshold + 80, threshold - 1, threshold}) {
        strip.setSize(width, strip.getHeight());
        for (const auto* text : {"Below 16.00 kHz", "Below 120 Hz"}) {
            strip.setRangeText(text);
            strip.setRangeOnSecondRow(juce::String(text) == "Below 16.00 kHz");
            EXPECT_EQ(compactValueBox(*mid), label);
            ASSERT_EQ(label->getCurrentTextEditor(), editor.getComponent());
            EXPECT_EQ(editor->getText(), "-3.27");
            EXPECT_EQ(editor->getHighlightedRegion(), (juce::Range<int> {1, 4}));
            EXPECT_TRUE(editor->hasKeyboardFocus(true));
            EXPECT_FALSE(history.canUndo());
            EXPECT_TRUE(midGestures.gestureBoundaries().empty());
            EXPECT_TRUE(sideGestures.gestureBoundaries().empty());
        }
    }
    mid->hideTextBox(true);
    EXPECT_NEAR(mid->getValue(), 0.0, 0.0001);
    mid->mouseDown(compactMouse(*mid));
    mid->mouseDrag(compactMouse(*mid, {32.0F, 12.0F}));
    ASSERT_TRUE(history.isEditing());
    const double edited = mid->getValue();
    for (const int width : {threshold, threshold + 80, threshold - 1, threshold}) {
        strip.setSize(width, strip.getHeight());
        for (const auto* text : {"Below 16.00 kHz", "Below 120 Hz"}) {
            strip.setRangeText(text);
            strip.setRangeOnSecondRow(juce::String(text) == "Below 16.00 kHz");
            EXPECT_TRUE(history.isEditing());
            EXPECT_DOUBLE_EQ(mid->getValue(), edited);
            EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true}));
            EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true}));
        }
    }
    mid->mouseDrag(compactMouse(*mid, {52.0F, 12.0F}));
    EXPECT_GT(mid->getValue(), edited);
    mid->endDrag();
    EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
    EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), 0.0F, 0.0001F);
    EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), 0.0F, 0.0001F);
    EXPECT_FALSE(history.canUndo());
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

/// Pan keeps directional compact text and the unchanged parameter parser for numeric entry and malformed tokens.
TEST(CompactBands, PanDirectionsAndNumericEntryUseExistingParameterParser)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    auto* pan = compactControl<ControlSlider>(strip, "Low Pan");
    ASSERT_NE(pan, nullptr);
    for (const auto& [text, value] :
         std::array {
             std::pair {"C", 0.0},
             std::pair {"25L", -25.0},
             std::pair {"33R", 33.0},
             std::pair {"100L", -100.0},
             std::pair {"100R", 100.0},
         })
    {
        SCOPED_TRACE(text);
        ASSERT_TRUE(enterCompactValue(*pan, text));
        EXPECT_NEAR(pan->getValue(), value, 0.0001);
        EXPECT_EQ(pan->compactValueText(), text);
    }
    ASSERT_TRUE(enterCompactValue(*pan, "-25 %"));
    EXPECT_EQ(pan->compactValueText(), "25L");
    for (const auto* malformed : {"25LR", "33RR", "25%L", "nan", "inf", "1e9999", "25 percent"}) {
        ASSERT_TRUE(enterCompactValue(*pan, malformed));
        EXPECT_NEAR(pan->getValue(), -25.0, 0.0001);
    }
    EXPECT_TRUE(pan->getTooltip().contains("percent pan"));
}

/// Pan edits and automation never invoke gain-only Link or add host-originated changes to local history.
TEST(CompactBands, PanHistoryAndAutomationRemainIndependentOfLinkedGains)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    auto* pan = compactControl<ControlSlider>(strip, "Low Pan");
    ASSERT_NE(pan, nullptr);
    auto& parameter = *processor.getParameters().getParameter(panParameterId(0));
    const CompactGestureCounter panGestures(parameter);
    const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
    const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
    EXPECT_TRUE(pan->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_EQ(panGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
    EXPECT_TRUE(history.undo());
    EXPECT_FALSE(history.canUndo());
    parameter.setValueNotifyingHost(parameter.convertTo0to1(-25.0F));
    EXPECT_NEAR(pan->getValue(), -25.0, 0.0001);
    EXPECT_EQ(pan->compactValueText(), "25L");
    EXPECT_FALSE(history.canUndo());
    EXPECT_TRUE(history.canRedo());
    EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), 0.0F, 0.0001F);
    EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), 0.0F, 0.0001F);
    EXPECT_TRUE(midGestures.gestureBoundaries().empty());
    EXPECT_TRUE(sideGestures.gestureBoundaries().empty());
}

/// Resizing between short and full inline names preserves the native numeric editor and active parameter gesture.
TEST(CompactBands, WideResizePreservesNumericEditorAndActivePanGesture)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& history = processor.getHistory();
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    auto* pan = compactControl<ControlSlider>(strip, "Low Pan");
    ASSERT_NE(pan, nullptr);
    ASSERT_TRUE(pan->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    const auto* label = compactValueBox(*pan);
    ASSERT_NE(label, nullptr);
    const juce::Component::SafePointer<juce::TextEditor> editor(label->getCurrentTextEditor());
    ASSERT_NE(editor, nullptr);
    editor->setText("33R", false);
    strip.setSize(400, BAND_STRIP_COMPACT_HEIGHT);
    EXPECT_EQ(compactValueBox(*pan), label);
    EXPECT_EQ(label->getCurrentTextEditor(), editor.getComponent());
    EXPECT_EQ(editor->getText(), "33R");
    EXPECT_EQ(pan->compactValueText(), "C PAN");
    pan->setCompactValueFullNames(false);
    EXPECT_EQ(label->getCurrentTextEditor(), editor.getComponent());
    EXPECT_EQ(pan->compactValueText(), "C");
    pan->setCompactValueFullNames(true);
    EXPECT_EQ(label->getCurrentTextEditor(), editor.getComponent());
    EXPECT_EQ(editor->getText(), "33R");
    EXPECT_EQ(pan->compactValueText(), "C PAN");
    EXPECT_FALSE(history.canUndo());
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    EXPECT_EQ(label->getCurrentTextEditor(), editor.getComponent());
    pan->hideTextBox(false);
    EXPECT_NEAR(pan->getValue(), 33.0, 0.0001);
    EXPECT_TRUE(history.undo());
    const CompactGestureCounter gestures(*processor.getParameters().getParameter(panParameterId(0)));
    pan->mouseDown(compactMouse(*pan));
    pan->mouseDrag(compactMouse(*pan, {32.0F, 12.0F}));
    EXPECT_TRUE(history.isEditing());
    strip.setSize(400, BAND_STRIP_COMPACT_HEIGHT);
    EXPECT_TRUE(history.isEditing());
    EXPECT_EQ(gestures.gestureBoundaries(), (std::vector<bool> {true}));
    pan->endDrag();
    EXPECT_EQ(gestures.gestureBoundaries(), (std::vector<bool> {true, false}));
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(pan->getValue(), 0.0, 0.0001);
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

/// Coarse and fine wheel and key actions reach the slider rather than an overlaid editable label.
TEST(CompactBands, WheelAndArrowsKeepFineStepsAndHostShortcutRouting)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    for (const auto* title : {"Low Mid", "Low Side", "Low Width", "Low Pan"}) {
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        auto* routed = dynamic_cast<ControlSlider*>(slider->getComponentAt(juce::Point<int> {12, 12}));
        ASSERT_EQ(routed, slider);
        const double initial = slider->getValue();
        routed->mouseWheelMove(compactMouse(*routed, {12.0F, 12.0F}, {12.0F, 12.0F}, {}), compactNotch());
        const double coarseWheel = slider->getValue() - initial;
        EXPECT_GT(coarseWheel, 0.0);
        slider->setValue(initial, juce::dontSendNotification);
        routed->mouseWheelMove(
            compactMouse(*routed, {12.0F, 12.0F}, {12.0F, 12.0F}, juce::ModifierKeys::shiftModifier), compactNotch()
        );
        const double fineWheel = slider->getValue() - initial;
        EXPECT_GT(fineWheel, 0.0);
        EXPECT_LE(fineWheel, coarseWheel + 0.00001);
        slider->setValue(initial, juce::dontSendNotification);
        EXPECT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        const double coarseKey = slider->getValue() - initial;
        slider->setValue(initial, juce::dontSendNotification);
        EXPECT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)));
        const double fineKey = slider->getValue() - initial;
        EXPECT_GT(fineKey, 0.0);
        EXPECT_LT(fineKey, coarseKey);
        for (const int modifier :
             {juce::ModifierKeys::commandModifier, juce::ModifierKeys::ctrlModifier, juce::ModifierKeys::altModifier})
        {
            EXPECT_FALSE(slider->keyPressed(juce::KeyPress(juce::KeyPress::rightKey, modifier, 0)));
        }
        EXPECT_FALSE(processor.getHistory().isEditing());
    }
}

/// Compact dragging remains relative and fine Shift movement is smaller, with double-click returning to default.
TEST(CompactBands, RelativeDragFineAdjustmentAndResetUseExistingSliderGestures)
{
    for (const auto* title : {"Low Mid", "Low Side", "Low Width", "Low Pan"}) {
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(true);
        strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        const double initial = slider->getValue();
        slider->mouseDown(compactMouse(*slider));
        EXPECT_DOUBLE_EQ(slider->getValue(), initial);
        slider->mouseDrag(compactMouse(*slider, {52.0F, 12.0F}));
        const double coarse = slider->getValue() - initial;
        slider->endDrag();
        EXPECT_GT(coarse, 0.0);
        EXPECT_TRUE(processor.getHistory().undo());
        const int fineModifiers = juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier;
        slider->mouseDown(compactMouse(*slider, {12.0F, 12.0F}, {12.0F, 12.0F}, fineModifiers));
        slider->mouseDrag(compactMouse(*slider, {52.0F, 12.0F}, {12.0F, 12.0F}, fineModifiers));
        const double fine = slider->getValue() - initial;
        slider->endDrag();
        EXPECT_GT(fine, 0.0);
        EXPECT_LT(fine, coarse);
        slider->mouseDoubleClick(compactMouse(*slider));
        EXPECT_NEAR(slider->getValue(), initial, 0.0001);
        EXPECT_FALSE(processor.getHistory().isEditing());
    }
}

/// Return opens the existing JUCE editor, preserving hundredth-step entry, gain Off, and malformed-token rejection.
TEST(CompactBands, ReturnNumericEntryUsesFullParameterParserAndPreservesMalformedValues)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    for (const auto* title : {"Low Mid", "Low Side", "Low Width", "Low Pan"}) {
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        const bool gain = slider->getName() == "Mid" || slider->getName() == "Side";
        const auto [entry, expected] = compactEntryExample(*slider);
        ASSERT_TRUE(enterCompactValue(*slider, entry));
        EXPECT_NEAR(slider->getValue(), expected, 0.0001);
        const double accepted = slider->getValue();
        for (const auto* invalid : {"", "junk", "nan", "inf", "1e9999", "++2", "1.2junk", "1 dB dB", "1 2"}) {
            SCOPED_TRACE(invalid);
            ASSERT_TRUE(enterCompactValue(*slider, invalid));
            EXPECT_DOUBLE_EQ(slider->getValue(), accepted);
        }
        if (gain) {
            ASSERT_TRUE(enterCompactValue(*slider, "-inf"));
            EXPECT_DOUBLE_EQ(slider->getValue(), BAND_GAIN_MIN_DB);
        }
        EXPECT_FALSE(processor.getHistory().isEditing());
    }
}

/// Every compact Mid input shares balanced Mid/Side gestures and exactly one undo command when Link is enabled.
TEST(CompactBands, LinkedInputPathsShareOneHistoryCommandAndBalancedHostGestures)
{
    for (int input = 0; input < 5; ++input) {
        SCOPED_TRACE(input);
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        auto& midParameter = *processor.getParameters().getParameter(midGainParameterId(0));
        auto& sideParameter = *processor.getParameters().getParameter(sideGainParameterId(0));
        setParameterInGesture(midParameter, -6.0F);
        setParameterInGesture(sideParameter, -6.0F);
        const CompactGestureCounter midGestures(midParameter);
        const CompactGestureCounter sideGestures(sideParameter);
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(true);
        strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
        strip.addToDesktop(0);
        strip.setVisible(true);
        auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
        ASSERT_NE(mid, nullptr);
        if (input == 0) {
            mid->mouseDown(compactMouse(*mid));
            mid->mouseDrag(compactMouse(*mid, {32.0F, 12.0F}));
            mid->endDrag();
        } else if (input == 1) {
            EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        } else if (input == 2) {
            mid->mouseWheelMove(compactMouse(*mid), compactNotch());
        } else if (input == 3) {
            ASSERT_TRUE(enterCompactValue(*mid, "-3.27 dB"));
        } else {
            mid->mouseDoubleClick(compactMouse(*mid));
        }
        const float edited = compactParameterValue(processor, midGainParameterId(0));
        EXPECT_GT(edited, -6.0F);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), edited, 0.0001F);
        EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
        EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
        EXPECT_FALSE(history.isEditing());
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), -6.0F, 0.0001F);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), -6.0F, 0.0001F);
        EXPECT_FALSE(history.canUndo());
        EXPECT_TRUE(history.redo());
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), edited, 0.0001F);
    }
}

/// Host updates remain silent during a linked gesture, and untouched Side Off is never captured into local history.
TEST(CompactBands, AutomationExcludedAndLinkedSideOffPreserved)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    ASSERT_NE(mid, nullptr);
    auto& midParameter = *processor.getParameters().getParameter(midGainParameterId(0));
    mid->mouseDown(compactMouse(*mid));
    std::thread automation([&midParameter] { midParameter.setValueNotifyingHost(midParameter.convertTo0to1(-9.0F)); });
    automation.join();
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
    do {
        dispatchCompactChanges();
    } while ((mid->getValue() < -9.0001 || mid->getValue() > -8.9999)
             && juce::Time::getMillisecondCounterHiRes() < deadline);
    EXPECT_NEAR(mid->getValue(), -9.0, 0.0001);
    EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), 0.0F, 0.0001F);
    mid->endDrag();
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.isEditing());
    auto& sideParameter = *processor.getParameters().getParameter(sideGainParameterId(0));
    setParameterInGesture(sideParameter, BAND_GAIN_MIN_DB);
    EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_FLOAT_EQ(compactParameterValue(processor, sideGainParameterId(0)), BAND_GAIN_MIN_DB);
    setParameterInGesture(sideParameter, -12.0F);
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), -9.0F, 0.0001F);
    EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), -12.0F, 0.0001F);
}

/// Mode switches finish drags once, ignore stale mouse movement, and preserve the linked edit as one command.
TEST(CompactBands, ModeSwitchClosesActiveGesturesInBothDirections)
{
    for (const bool initiallyCompact : {false, true}) {
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
        const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(initiallyCompact);
        strip.setSize(240, initiallyCompact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
        auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
        ASSERT_NE(mid, nullptr);
        mid->mouseDown(compactMouse(*mid));
        mid->mouseDrag(compactMouse(*mid, {32.0F, 12.0F}));
        EXPECT_TRUE(history.isEditing());
        const double edited = mid->getValue();
        strip.setCompact(!initiallyCompact);
        EXPECT_FALSE(history.isEditing());
        mid->mouseDrag(compactMouse(*mid, {52.0F, 12.0F}));
        mid->mouseUp(compactMouse(*mid, {52.0F, 12.0F}));
        EXPECT_DOUBLE_EQ(mid->getValue(), edited);
        EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
        EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), 0.0F, 0.0001F);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), 0.0F, 0.0001F);
        EXPECT_FALSE(history.canUndo());
    }
}

/// Hiding a native numeric editor commits valid contents once and preserves the old value for malformed contents.
TEST(CompactBands, ModeSwitchFinishesNumericEntrySafely)
{
    for (const bool initiallyCompact : {false, true}) {
        for (const bool valid : {false, true}) {
            ImageControlProcessor processor;
            ImageControlLookAndFeel lookAndFeel;
            BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
            strip.setLookAndFeel(&lookAndFeel);
            strip.setCompact(initiallyCompact);
            strip.setSize(240, initiallyCompact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
            strip.addToDesktop(0);
            strip.setVisible(true);
            auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
            ASSERT_NE(mid, nullptr);
            ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
            auto* label = compactValueBox(*mid);
            ASSERT_NE(label, nullptr);
            ASSERT_NE(label->getCurrentTextEditor(), nullptr);
            label->getCurrentTextEditor()->setText(valid ? "-3.27" : "broken", false);
            strip.setCompact(!initiallyCompact);
            EXPECT_NEAR(mid->getValue(), valid ? -3.27 : 0.0, 0.0001);
            EXPECT_FALSE(processor.getHistory().isEditing());
            auto* restoredLabel = compactValueBox(*mid);
            ASSERT_NE(restoredLabel, nullptr);
            EXPECT_FALSE(restoredLabel->isBeingEdited());
            EXPECT_EQ(processor.getHistory().canUndo(), valid);
        }
    }
}

/// Teardown discards uncommitted text while attachments still exist and releases editor focus before slider teardown.
TEST(CompactBands, TeardownWithFocusedNumericEditorDoesNotCommitOrLeakGestures)
{
    for (const bool compact : {false, true}) {
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
        const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
        {
            BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
            strip.setLookAndFeel(&lookAndFeel);
            strip.setCompact(compact);
            strip.setSize(240, compact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
            strip.addToDesktop(0);
            strip.setVisible(true);
            auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
            ASSERT_NE(mid, nullptr);
            ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
            const auto* label = compactValueBox(*mid);
            ASSERT_NE(label, nullptr);
            ASSERT_NE(label->getCurrentTextEditor(), nullptr);
            label->getCurrentTextEditor()->setText("-3.27 dB", false);
        }
        EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), 0.0F, 0.0001F);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), 0.0F, 0.0001F);
        EXPECT_FALSE(history.isEditing());
        EXPECT_FALSE(history.canUndo());
        EXPECT_TRUE(midGestures.gestureBoundaries().empty());
        EXPECT_TRUE(sideGestures.gestureBoundaries().empty());
    }
}

/// Hidden controls hand focus to Mid, while retained Mid/Side/Width/Pan keep focus across both presentations.
TEST(CompactBands, FocusMovesFromHiddenControlsAndRetainedValuesKeepFocus)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    strip.toFront(true);
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    ASSERT_NE(mid, nullptr);
    for (const auto* title : {"Low Recover", "Low Pan", "Low Solo", "Low Link", "Low Mid", "Low Side", "Low Width"}) {
        strip.setCompact(false);
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        auto* control = compactControl<juce::Component>(strip, title);
        ASSERT_NE(control, nullptr);
        control->grabKeyboardFocus();
        ASSERT_TRUE(control->hasKeyboardFocus(false));
        strip.setCompact(true);
        strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
        if (control->isVisible()) {
            EXPECT_TRUE(control->hasKeyboardFocus(false));
        } else {
            EXPECT_TRUE(mid->hasKeyboardFocus(false));
        }
        auto* retained = juce::Component::getCurrentlyFocusedComponent();
        strip.setCompact(false);
        EXPECT_EQ(juce::Component::getCurrentlyFocusedComponent(), retained);
    }
}

/// Screen readers retain full band names, editable slider roles, and unit-bearing values in compact mode.
TEST(CompactBands, AccessibilityKeepsFullNamesUnitsAndEditing)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    strip.addToDesktop(0);
    for (const auto* title : {"Low Mid", "Low Side", "Low Width", "Low Pan"}) {
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        auto* handler = slider->getAccessibilityHandler();
        ASSERT_NE(handler, nullptr);
        EXPECT_EQ(handler->getRole(), juce::AccessibilityRole::slider);
        EXPECT_EQ(handler->getTitle(), title);
        auto* value = handler->getValueInterface();
        ASSERT_NE(value, nullptr);
        EXPECT_FALSE(value->isReadOnly());
        const auto [initialText, initialValue] = compactDefaultExample(*slider);
        const auto [entry, expected] = compactEntryExample(*slider);
        EXPECT_EQ(value->getCurrentValueAsString(), initialText);
        value->setValueAsString(entry);
        EXPECT_NEAR(slider->getValue(), expected, 0.0001);
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_NEAR(slider->getValue(), initialValue, 0.0001);
    }
}
/// Real expanded and compact fields keep their value font, text origin, and fixed suffix outside selection.
TEST(CompactBands, NumericEditorKeepsFieldPresentationAndNoneditableSuffixes)
{
    for (const bool compact : {false, true}) {
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(compact);
        strip.setSize(240, compact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
        strip.addToDesktop(0);
        strip.setVisible(true);
        for (const auto* title : {"Low Mid", "Low Side", "Low Width", "Low Pan", "Low Recover"}) {
            auto* slider = compactControl<ControlSlider>(strip, title);
            ASSERT_NE(slider, nullptr);
            if (!slider->isVisible()) {
                continue;
            }
            auto* label = compactValueBox(*slider);
            ASSERT_NE(label, nullptr);
            if (slider->getName() == "Recover") {
                setParameterInGesture(*processor.getParameters().getParameter(recoverParameterId(0)), 25.0F);
                EXPECT_EQ(slider->numericValueSuffix(), "%");
            }
            const auto bounds = label->getBounds();
            const auto font = label->getLookAndFeel().getLabelFont(*label);
            const auto number = slider->numericValueText();
            const auto suffix = slider->numericValueSuffix();
            ASSERT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
            auto* editor = label->getCurrentTextEditor();
            ASSERT_NE(editor, nullptr);
            EXPECT_EQ(editor->getText(), number);
            EXPECT_EQ(editor->getHighlightedText(), number);
            EXPECT_EQ(editor->getFont(), font);
            EXPECT_EQ(editor->getJustificationType(), juce::Justification::centredLeft);
            EXPECT_EQ(editor->getBorder(), juce::BorderSize<int>());
            EXPECT_EQ(editor->getLeftIndent(), 0);
            EXPECT_EQ(editor->getTopIndent(), 0);
            EXPECT_TRUE(editor->findColour(juce::TextEditor::backgroundColourId).isTransparent());
            EXPECT_TRUE(editor->findColour(juce::TextEditor::focusedOutlineColourId).isTransparent());
            EXPECT_EQ(
                editor->findColour(juce::TextEditor::textColourId),
                slider->findColour(juce::Slider::textBoxTextColourId)
            );
            EXPECT_EQ(label->getBounds(), bounds);
            const auto area = label->getBorderSize().subtractedFrom(label->getLocalBounds());
            const int suffixWidth = static_cast<int>(std::ceil(juce::GlyphArrangement::getStringWidth(font, suffix)));
            const int numberWidth
                = std::max(1, static_cast<int>(std::ceil(juce::GlyphArrangement::getStringWidth(font, number))));
            EXPECT_EQ(editor->getX(), area.getX() + (area.getWidth() - numberWidth - suffixWidth) / 2);
            EXPECT_LE(editor->getRight() + suffixWidth, area.getRight());
            const auto editorBounds = editor->getBounds();
            editor->selectAll();
            EXPECT_EQ(editor->getHighlightedText(), number);
            slider->hideTextBox(true);
            ASSERT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
            ASSERT_NE(label->getCurrentTextEditor(), nullptr);
            EXPECT_EQ(label->getCurrentTextEditor()->getBounds(), editorBounds);
            slider->hideTextBox(true);
        }
        EXPECT_FALSE(processor.getHistory().canUndo());
    }
}

/// Production construction and a compact/full round trip keep Recover's click-opened editor numeric and nudgeable.
TEST(CompactBands, EditorRecoveryLabelClickKeepsPercentFixedAndArrowGesturesIndependent)
{
    for (const bool initiallyCompact : {false, true}) {
        SCOPED_TRACE(initiallyCompact);
        ImageControlProcessor processor;
        auto& state = processor.getEditorState();
        state.setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
        state.setProperty(COMPACT_BAND_CONTROLS_PROPERTY, initiallyCompact, nullptr);
        auto& parameter = *processor.getParameters().getParameter(recoverParameterId(0));
        setParameterInGesture(parameter, 25.0F);
        const CompactGestureCounter recoveryGestures(parameter);
        const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
        const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
        ImageControlEditor editor(processor);
        editor.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        editor.setAlwaysOnTop(true);
        editor.setVisible(true);
        editor.setTopLeftPosition(80, 100);
        editor.toFront(true);
        dispatchCompactChanges();
        if (initiallyCompact) {
            juce::Button* disclosure = nullptr;
            for (auto* child : editor.getChildren()) {
                if (child->getComponentID() == "band-controls-disclosure") {
                    disclosure = dynamic_cast<juce::Button*>(child);
                }
            }
            ASSERT_NE(disclosure, nullptr);
            disclosure->onClick();
        }
        BandStrip* strip = nullptr;
        for (auto* child : editor.getChildren()) {
            if (auto* candidate = dynamic_cast<BandStrip*>(child);
                candidate != nullptr && compactControl<ControlSlider>(*candidate, "Low Recover") != nullptr)
            {
                strip = candidate;
            }
        }
        ASSERT_NE(strip, nullptr);
        auto* slider = compactControl<ControlSlider>(*strip, "Low Recover");
        ASSERT_NE(slider, nullptr);
        auto* label = compactValueBox(*slider);
        ASSERT_NE(label, nullptr);
        ASSERT_TRUE(slider->isShowing());
        ASSERT_TRUE(label->isEditableOnSingleClick());
        ASSERT_TRUE(clickCompactComponent(editor, *label));
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(label->getCurrentTextEditor()->getText(), "25");
        EXPECT_EQ(label->getCurrentTextEditor()->getHighlightedText(), "25");
        EXPECT_EQ(slider->numericValueSuffix(), "%");
        auto* peer = editor.getPeer();
        ASSERT_NE(peer, nullptr);
        EXPECT_TRUE(peer->handleKeyPress(juce::KeyPress(juce::KeyPress::upKey)));
        EXPECT_NEAR(slider->getValue(), 27.0, 0.0001);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(label->getCurrentTextEditor()->getText(), "27");
        EXPECT_TRUE(
            peer->handleKeyPress(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0))
        );
        EXPECT_DOUBLE_EQ(slider->getValue(), 26.0);
        EXPECT_FLOAT_EQ(plainValue(parameter), 26.0F);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(label->getCurrentTextEditor()->getText(), "26");
        EXPECT_TRUE(peer->handleKeyPress(juce::KeyPress(juce::KeyPress::returnKey)));
        EXPECT_TRUE(waitForCompactEditorClose(*label));
        EXPECT_EQ(recoveryGestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false}));
        EXPECT_TRUE(midGestures.gestureBoundaries().empty());
        EXPECT_TRUE(sideGestures.gestureBoundaries().empty());
        EXPECT_FALSE(processor.getHistory().isEditing());
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_NEAR(slider->getValue(), 27.0, 0.0001);
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_NEAR(slider->getValue(), 25.0, 0.0001);
        EXPECT_FALSE(processor.getHistory().canUndo());
    }
}

/// Long valid drafts scroll within the numeric region so their trailing precision remains reachable.
TEST(CompactBands, NumericEditorKeepsLongDraftCaretVisibleWithoutCoveringUnit)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    ASSERT_NE(mid, nullptr);
    ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    auto* label = compactValueBox(*mid);
    ASSERT_NE(label, nullptr);
    auto* editor = label->getCurrentTextEditor();
    ASSERT_NE(editor, nullptr);
    editor->selectAll();
    editor->insertTextAtCaret("-12.34567890123456789");
    dispatchCompactChanges();
    const auto caret = editor->getCaretRectangleForCharIndex(editor->getCaretPosition());
    EXPECT_GE(caret.getX(), 0);
    EXPECT_LE(caret.getX(), editor->getWidth());
    EXPECT_LT(editor->getWidth(), editor->getTextWidth());
    EXPECT_LT(editor->getRight(), label->getWidth());
    mid->hideTextBox(true);
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Unitless frequency edits use the displayed scale while explicit units retain the strict host parser contract.
TEST(CompactBands, FrequencyEditorKeepsHertzAndKilohertzFixedWithoutChangingHostParser)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    ControlSlider slider;
    slider.setLookAndFeel(&lookAndFeel);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 100, 24);
    const HistorySliderAttachment attachment(
        processor.getParameters(), crossoverParameterId(1), slider, processor.getHistory()
    );
    slider.setSize(240, 24);
    slider.addToDesktop(0);
    slider.setVisible(true);
    auto& parameter = *processor.getParameters().getParameter(crossoverParameterId(1));
    for (const auto& [value, number, suffix, entry, expected] :
         std::array {
             std::tuple {800.0F, "800", " Hz", "850", 850.0F},
             std::tuple {1500.0F, "1.50", " kHz", "1.75", 1750.0F},
             std::tuple {1500.0F, "1.50", " kHz", "900 Hz", 900.0F},
         })
    {
        setParameterInGesture(parameter, value);
        ASSERT_TRUE(slider.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        auto* label = compactValueBox(slider);
        ASSERT_NE(label, nullptr);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(label->getCurrentTextEditor()->getText(), number);
        EXPECT_EQ(slider.numericValueSuffix(), suffix);
        slider.hideTextBox(true);
        ASSERT_TRUE(enterCompactValue(slider, entry));
        EXPECT_NEAR(slider.getValue(), expected, 0.01);
    }
    EXPECT_NEAR(slider.getValueFromText("1.75"), MIN_FREQUENCY, 0.01);
    EXPECT_NEAR(slider.getValueFromText("1.75 kHz"), 1750.0, 0.01);
    const double accepted = slider.getValue();
    for (const auto* malformed : {"1 kHz kHz", "1 Hz Hz", "1.5junk", "", "nan", "inf"}) {
        ASSERT_TRUE(enterCompactValue(slider, malformed));
        EXPECT_DOUBLE_EQ(slider.getValue(), accepted);
    }
}

/// Untouched rounded readouts retain precision, and legacy recovery Off input displays the numeric zero percentage.
TEST(CompactBands, NumericEditorPreservesUntouchedPrecisionAndRecoveryOff)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    for (const auto* title : {"Low Mid", "Low Pan", "Low Recover"}) {
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        auto parameterId = midGainParameterId(0);
        if (slider->getName() == "Pan") {
            parameterId = panParameterId(0);
        } else if (slider->getName() == "Recover") {
            parameterId = recoverParameterId(0);
        }
        auto& parameter = *processor.getParameters().getParameter(parameterId);
        setParameterInGesture(parameter, slider->getName() == "Pan" ? -25.4F : 3.27F);
        const double original = slider->getValue();
        EXPECT_TRUE(enterCompactValue(*slider, slider->numericValueText()));
        EXPECT_DOUBLE_EQ(slider->getValue(), original);
        EXPECT_FALSE(processor.getHistory().canUndo());
        if (slider->getName() == "Recover") {
            EXPECT_TRUE(enterCompactValue(*slider, "Off"));
            EXPECT_NEAR(slider->getValue(), 0.0, 0.0001);
            EXPECT_EQ(slider->numericValueText(), "0");
            EXPECT_EQ(slider->numericValueSuffix(), "%");
            EXPECT_TRUE(processor.getHistory().undo());
            EXPECT_DOUBLE_EQ(slider->getValue(), original);
            EXPECT_DOUBLE_EQ(original, 3.0);
            EXPECT_TRUE(enterCompactValue(*slider, "37.5"));
            EXPECT_DOUBLE_EQ(slider->getValue(), 38.0);
            EXPECT_FLOAT_EQ(plainValue(parameter), 38.0F);
            EXPECT_EQ(slider->numericValueText(), "38");
            EXPECT_TRUE(processor.getHistory().undo());
            EXPECT_DOUBLE_EQ(slider->getValue(), original);
            EXPECT_TRUE(processor.getHistory().redo());
            EXPECT_DOUBLE_EQ(slider->getValue(), 38.0);
            for (const auto* invalid : {"nan", "inf", "37%%", "37 trailing"}) {
                EXPECT_TRUE(enterCompactValue(*slider, invalid));
                EXPECT_DOUBLE_EQ(slider->getValue(), 38.0);
                EXPECT_FLOAT_EQ(plainValue(parameter), 38.0F);
            }
            EXPECT_TRUE(enterCompactValue(*slider, "0.1"));
            EXPECT_DOUBLE_EQ(slider->getValue(), 0.0);
            EXPECT_EQ(slider->numericValueText(), "0");
            EXPECT_TRUE(processor.getHistory().undo());
            EXPECT_DOUBLE_EQ(slider->getValue(), 38.0);
            EXPECT_TRUE(processor.getHistory().undo());
            EXPECT_DOUBLE_EQ(slider->getValue(), original);
            EXPECT_FALSE(processor.getHistory().canUndo());
        }
    }
}

/// Vertical arrows in the real numeric editor retain coarse/fine conventions, balanced Link gestures, and undo.
TEST(CompactBands, NumericEditorArrowsReuseSliderNudgesAndLinkedHistory)
{
    for (const bool compact : {false, true}) {
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        auto& midParameter = *processor.getParameters().getParameter(midGainParameterId(0));
        auto& sideParameter = *processor.getParameters().getParameter(sideGainParameterId(0));
        setParameterInGesture(midParameter, -6.0F);
        setParameterInGesture(sideParameter, -6.0F);
        const CompactGestureCounter midGestures(midParameter);
        const CompactGestureCounter sideGestures(sideParameter);
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(compact);
        strip.setSize(240, compact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
        strip.addToDesktop(0);
        strip.setVisible(true);
        auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
        ASSERT_NE(mid, nullptr);
        ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        auto* label = compactValueBox(*mid);
        ASSERT_NE(label, nullptr);
        for (const int modifier :
             {juce::ModifierKeys::commandModifier, juce::ModifierKeys::ctrlModifier, juce::ModifierKeys::altModifier})
        {
            ASSERT_NE(label->getCurrentTextEditor(), nullptr);
            EXPECT_FALSE(label->getCurrentTextEditor()->keyPressed(juce::KeyPress(juce::KeyPress::upKey, modifier, 0)));
            EXPECT_NEAR(mid->getValue(), -6.0, 0.0001);
        }
        ASSERT_TRUE(label->getCurrentTextEditor()->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
        const double coarse = mid->getValue();
        EXPECT_NEAR(coarse, -5.28, 0.0001);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(label->getCurrentTextEditor()->getText(), mid->numericValueText());
        ASSERT_TRUE(label->getCurrentTextEditor()->keyPressed(
            juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0)
        ));
        EXPECT_NEAR(mid->getValue(), coarse - 0.07, 0.0001);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), mid->getValue(), 0.0001);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        const double finalValue = mid->getValue();
        EXPECT_TRUE(label->getCurrentTextEditor()->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        EXPECT_TRUE(waitForCompactEditorClose(*label));
        EXPECT_DOUBLE_EQ(mid->getValue(), finalValue);
        EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false}));
        EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false}));
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(mid->getValue(), coarse, 0.0001);
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(mid->getValue(), -6.0, 0.0001);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), -6.0F, 0.0001F);
        EXPECT_FALSE(history.canUndo());
    }
}

/// Wheel input delivered through the native peer keeps numeric focus, slider steps, Link, and one undo per notch.
TEST(CompactBands, FocusedNumericEditorWheelUsesSliderStepsAndLinkedHistory)
{
    for (const bool compact : {false, true}) {
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        auto& midParameter = *processor.getParameters().getParameter(midGainParameterId(0));
        auto& sideParameter = *processor.getParameters().getParameter(sideGainParameterId(0));
        setParameterInGesture(midParameter, -6.0F);
        setParameterInGesture(sideParameter, -6.0F);
        const CompactGestureCounter midGestures(midParameter);
        const CompactGestureCounter sideGestures(sideParameter);
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(compact);
        strip.setSize(240, compact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
        strip.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        strip.setAlwaysOnTop(true);
        strip.setTopLeftPosition(80, 100);
        strip.setVisible(true);
        strip.toFront(true);
        auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
        ASSERT_NE(mid, nullptr);
        ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        auto* label = compactValueBox(*mid);
        auto* peer = strip.getPeer();
        ASSERT_NE(label, nullptr);
        ASSERT_NE(peer, nullptr);
        const juce::ScopedValueSetter<juce::ModifierKeys> restoreModifiers(juce::ModifierKeys::currentModifiers, {});
        for (const bool fine : {false, true}) {
            auto* editor = label->getCurrentTextEditor();
            ASSERT_NE(editor, nullptr);
            EXPECT_TRUE(editor->hasKeyboardFocus(true));
            const auto position = strip.getLocalPoint(editor, editor->getLocalBounds().getCentre()).toFloat();
            const auto modifiers = juce::ModifierKeys(fine ? juce::ModifierKeys::shiftModifier : 0);
            juce::ModifierKeys::currentModifiers = modifiers;
            peer->handleMouseEvent(
                juce::MouseInputSource::InputSourceType::mouse,
                position,
                modifiers,
                juce::MouseInputSource::defaultPressure,
                juce::MouseInputSource::defaultOrientation,
                juce::Time::currentTimeMillis()
            );
            auto wheel = compactNotch();
            if (fine) {
                wheel.deltaY = -wheel.deltaY;
            }
            peer->handleMouseWheel(
                juce::MouseInputSource::InputSourceType::mouse, position, juce::Time::currentTimeMillis(), wheel
            );
            EXPECT_NEAR(mid->getValue(), fine ? -5.91 : -5.9, 0.0001);
            EXPECT_NEAR(sideParameter.convertFrom0to1(sideParameter.getValue()), mid->getValue(), 0.0001);
            ASSERT_NE(label->getCurrentTextEditor(), nullptr);
            EXPECT_TRUE(label->getCurrentTextEditor()->hasKeyboardFocus(true));
            EXPECT_EQ(label->getCurrentTextEditor()->getText(), mid->numericValueText());
        }
        mid->hideTextBox(true);
        EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false}));
        EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false}));
        EXPECT_FALSE(history.isEditing());
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(mid->getValue(), -5.9, 0.0001);
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(mid->getValue(), -6.0, 0.0001);
        EXPECT_FALSE(history.canUndo());
    }
}

/// Smooth substeps and clamped wheel events retain the same native editor and uncommitted draft.
TEST(CompactBands, NumericEditorWheelPreservesDraftForSubstepsAndClamps)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& parameter = *processor.getParameters().getParameter(recoverParameterId(0));
    const CompactGestureCounter gestures(parameter);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    auto* recovery = compactControl<ControlSlider>(strip, "Low Recover");
    ASSERT_NE(recovery, nullptr);
    ASSERT_TRUE(recovery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    auto* label = compactValueBox(*recovery);
    ASSERT_NE(label, nullptr);
    auto* editor = label->getCurrentTextEditor();
    ASSERT_NE(editor, nullptr);
    editor->setText("123", false);
    auto wheel = compactNotch();
    wheel.isSmooth = true;
    wheel.deltaY = 0.02F;
    for (int step = 0; step < 2; ++step) {
        editor->mouseWheelMove(compactMouse(*editor, {}, {}, {}), wheel);
        EXPECT_EQ(label->getCurrentTextEditor(), editor);
        EXPECT_EQ(editor->getText(), "123");
        EXPECT_DOUBLE_EQ(recovery->getValue(), 0.0);
        EXPECT_FALSE(processor.getHistory().canUndo());
    }
    editor->mouseWheelMove(compactMouse(*editor, {}, {}, juce::ModifierKeys::shiftModifier), wheel);
    EXPECT_DOUBLE_EQ(recovery->getValue(), 1.0);
    ASSERT_NE(label->getCurrentTextEditor(), nullptr);
    EXPECT_EQ(label->getCurrentTextEditor()->getText(), "1");
    EXPECT_EQ(recovery->numericValueSuffix(), "%");
    recovery->hideTextBox(true);
    EXPECT_TRUE(processor.getHistory().undo());
    ASSERT_TRUE(recovery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    editor = label->getCurrentTextEditor();
    ASSERT_NE(editor, nullptr);
    editor->setText("123", false);
    wheel = compactNotch();
    wheel.deltaY = -wheel.deltaY;
    editor->mouseWheelMove(compactMouse(*editor, {}, {}, {}), wheel);
    EXPECT_EQ(label->getCurrentTextEditor(), editor);
    EXPECT_EQ(editor->getText(), "123");
    EXPECT_TRUE(processor.getHistory().canRedo());
    EXPECT_FALSE(processor.getHistory().canUndo());
    EXPECT_FALSE(processor.getHistory().isEditing());
    recovery->hideTextBox(true);
    EXPECT_EQ(gestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false, true, false}));
}

/// Escape discards a draft, focus loss commits it, and automation closes stale text without becoming a local edit.
TEST(CompactBands, NumericEditorCancelFocusLossAndAutomationPreserveNativeLifecycle)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& history = processor.getHistory();
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setLookAndFeel(&lookAndFeel);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    auto* side = compactControl<ControlSlider>(strip, "Low Side");
    ASSERT_NE(mid, nullptr);
    ASSERT_NE(side, nullptr);
    ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    auto* label = compactValueBox(*mid);
    ASSERT_NE(label, nullptr);
    ASSERT_NE(label->getCurrentTextEditor(), nullptr);
    label->getCurrentTextEditor()->setText("-3.27", false);
    EXPECT_TRUE(label->getCurrentTextEditor()->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    EXPECT_TRUE(waitForCompactEditorClose(*label));
    EXPECT_NEAR(mid->getValue(), 0.0, 0.0001);
    EXPECT_FALSE(history.canUndo());
    ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    ASSERT_NE(label->getCurrentTextEditor(), nullptr);
    label->getCurrentTextEditor()->setText("-3.27", false);
    side->grabKeyboardFocus();
    EXPECT_TRUE(waitForCompactEditorClose(*label));
    EXPECT_NEAR(mid->getValue(), -3.27, 0.0001);
    EXPECT_TRUE(history.undo());
    ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    ASSERT_NE(label->getCurrentTextEditor(), nullptr);
    label->getCurrentTextEditor()->setText("-4.5", false);
    auto& parameter = *processor.getParameters().getParameter(midGainParameterId(0));
    parameter.setValueNotifyingHost(parameter.convertTo0to1(-9.0F));
    EXPECT_TRUE(waitForCompactEditorClose(*label));
    EXPECT_NEAR(mid->getValue(), -9.0, 0.0001);
    EXPECT_FALSE(history.canUndo());
    EXPECT_TRUE(history.canRedo());
}

/// Display disclosures paint local keyboard focus instead of gaining a section-sized managed outline.
TEST(CompactBands, DisclosureFocusOptOutLeavesOrdinaryButtonOutlinesEnabled)
{
    ImageControlLookAndFeel lookAndFeel;
    juce::Component root;
    DisplayDisclosure disclosure("Controls", false);
    juce::TextButton ordinary("Action");
    root.setLookAndFeel(&lookAndFeel);
    root.addAndMakeVisible(disclosure);
    root.addAndMakeVisible(ordinary);
    root.setSize(240, 80);
    disclosure.setBounds(0, 0, 240, 28);
    disclosure.setHasFocusOutline(false);
    ordinary.setBounds(0, 40, 100, 28);
    ordinary.setHasFocusOutline(false);
    root.addToDesktop(0);
    root.setVisible(true);
    root.toFront(true);
    disclosure.grabKeyboardFocus();
    EXPECT_TRUE(disclosure.hasKeyboardFocus(false));
    EXPECT_FALSE(disclosure.hasFocusOutline());
    EXPECT_EQ(lookAndFeel.createFocusOutlineForComponent(disclosure), nullptr);
    ordinary.grabKeyboardFocus();
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
    do {
        dispatchCompactChanges();
    } while (!ordinary.hasFocusOutline() && juce::Time::getMillisecondCounterHiRes() < deadline);
    EXPECT_TRUE(ordinary.hasKeyboardFocus(false));
    EXPECT_TRUE(ordinary.hasFocusOutline());
    EXPECT_NE(lookAndFeel.createFocusOutlineForComponent(ordinary), nullptr);
    root.setLookAndFeel(nullptr);
}
}  // namespace esgrove::image_control::test
