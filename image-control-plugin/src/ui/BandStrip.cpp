/// Implements BandStrip: the slider setup and layout,
/// its independent Width and Recover attachments, and Link's balanced Side-trim host gestures.
/// Also implements live correlation readouts, bounded time-based history, and layered meter indicators.

#include "ui/BandStrip.h"

#include "ParameterHistory.h"
#include "Parameters.h"
#include "ui/HistoryAttachments.h"
#include "ui/ImageControlLookAndFeel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace esgrove::image_control
{
namespace
{
/// Accessibility value of a correlation meter, with no editing action or misleading numeric silence value.
class CorrelationAccessibleValue final : public juce::AccessibilityTextValueInterface
{
public:
    /// Keeps a reference to the component, which owns and outlives its accessibility handler.
    explicit CorrelationAccessibleValue(BandCorrelationMeter& owner) : meter(owner) {}

    /// Leaves the component lifetime with its owner.
    ~CorrelationAccessibleValue() override = default;

    CorrelationAccessibleValue(const CorrelationAccessibleValue&) = delete;
    CorrelationAccessibleValue(CorrelationAccessibleValue&&) = delete;
    CorrelationAccessibleValue& operator=(const CorrelationAccessibleValue&) = delete;
    CorrelationAccessibleValue& operator=(CorrelationAccessibleValue&&) = delete;

    /// Reads the current measurement or its unavailable status.
    [[nodiscard]] juce::String getCurrentValueAsString() const override
    {
        return meter.valueText();
    }
    /// Exposes measurements as read-only.
    [[nodiscard]] bool isReadOnly() const override
    {
        return true;
    }
    /// Ignores editing because the value comes from the audio processor.
    void setValueAsString(const juce::String& /*value*/) override {}

private:
    /// The component that owns this value interface through its handler.
    BandCorrelationMeter& meter;
};

/// Height of the name and range rows, in pixels.
constexpr int HEADER_ROW_HEIGHT {20};
/// Height of the labels above the sliders, in pixels.
constexpr int SLIDER_LABEL_HEIGHT {18};
/// Height of the sliders, in pixels.
constexpr int SLIDER_HEIGHT {26};
/// Width of the value boxes of the sliders, in pixels.
constexpr int TEXT_BOX_WIDTH {78};
/// Height of the value boxes of the sliders, in pixels.
constexpr int TEXT_BOX_HEIGHT {22};
/// Width of the Link toggle, in pixels.
constexpr int LINK_WIDTH {60};
/// Gap between the Solo button and the Link toggle, in pixels.
constexpr int SOLO_GAP {6};
/// Insets only Solo artwork to 16 logical units inside its unchanged 20-by-20 interaction bounds.
constexpr int SOLO_ICON_INDENT {2};
/// Gap separating the inline name, range, and heading buttons, in pixels.
constexpr int HEADER_TEXT_GAP {8};
/// How much brighter the headphone icon draws while the pointer is over the Solo button.
constexpr float SOLO_HOVER_BRIGHTNESS {0.3F};
/// Largest vertical gap between the range label and the first slider row, in pixels.
/// The gap only takes height the slider rows do not need, so it is gone at the minimum strip height.
constexpr int HEADER_SLIDER_GAP {16};
/// Inner margin of the strip, in pixels.
constexpr int MARGIN {10};
/// Number of slider rows: Mid, Side, Width, Recover, and Pan.
constexpr int SLIDER_ROWS {5};
/// Font height of the band name.
constexpr float NAME_FONT_HEIGHT {16.0F};
/// Font height of the band range and the slider labels.
constexpr float LABEL_FONT_HEIGHT {14.0F};
/// Width change per mouse wheel step.
constexpr double WIDTH_WHEEL_STEP {0.01};
/// Recovery change per mouse wheel step, in percent.
constexpr double RECOVER_WHEEL_STEP_PERCENT {1.0};
/// Change of the Pan slider per mouse wheel step, in percent.
constexpr double PAN_WHEEL_STEP {1.0};
/// Side of the square box the headphone icon of the Solo button is drawn in, in icon units.
constexpr float HEADPHONE_BOX {24.0F};
/// Half-width of the correlation range's soft spatial edge in signed correlation units.
constexpr float CORRELATION_RANGE_FEATHER {0.06F};
/// Thickness of the headband of the headphone icon, in icon units.
constexpr float HEADBAND_THICKNESS {2.0F};
/// Corner radius of the ear cups of the headphone icon, in icon units.
constexpr float EAR_CUP_CORNER {1.5F};
/// Height of the compact editable value row, in pixels.
constexpr int COMPACT_VALUE_HEIGHT {24};
/// Narrow inset leaves room for four compact values at the minimum 180-pixel column width.
constexpr int COMPACT_MARGIN {2};
/// Gap between compact numeric fields, in pixels.
constexpr int COMPACT_VALUE_GAP {2};
/// Relative field widths at a 180-pixel column: gains keep signed tenths, Width keeps hundredths, and Pan four glyphs.
constexpr std::array COMPACT_FIELD_WEIGHTS {49, 49, 40, 32};
/// Total field allocation units, excluding gaps.
constexpr int COMPACT_TOTAL_FIELD_WEIGHT {170};

static_assert(BAND_STRIP_COMPACT_HEIGHT == COMPACT_MARGIN * 2 + COMPACT_VALUE_HEIGHT);

static_assert(
    BAND_STRIP_MIN_HEIGHT == MARGIN * 2 + HEADER_ROW_HEIGHT * 2 + SLIDER_ROWS * (SLIDER_LABEL_HEIGHT + SLIDER_HEIGHT),
    "The minimum strip height must fit the margins, the header rows, and the slider rows"
);

/// Measures the label's actual drawing font and insets without allowing fitted-text compression.
int headingLabelWidth(juce::Label& label)
{
    auto& lookAndFeel = label.getLookAndFeel();
    return static_cast<int>(
               std::ceil(juce::GlyphArrangement::getStringWidth(lookAndFeel.getLabelFont(label), label.getText()))
           )
        + lookAndFeel.getLabelBorderSize(label).getLeftAndRight();
}

/// Returns a headphone icon in a HEADPHONE_BOX square: a headband arc over two ear cups.
juce::Path headphonePath()
{
    constexpr float centreX {HEADPHONE_BOX * 0.5F};
    constexpr float bandCentreY {13.0F};
    constexpr float bandRadius {8.0F};
    constexpr float cupTop {12.5F};
    constexpr float cupWidth {5.0F};
    constexpr float cupHeight {8.5F};
    juce::Path headband;
    headband.addCentredArc(
        centreX,
        bandCentreY,
        bandRadius,
        bandRadius,
        0.0F,
        -juce::MathConstants<float>::halfPi,
        juce::MathConstants<float>::halfPi,
        true
    );
    juce::Path icon;
    juce::PathStrokeType(HEADBAND_THICKNESS, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
        .createStrokedPath(icon, headband);
    icon.addRoundedRectangle(centreX - bandRadius - 1.0F, cupTop, cupWidth, cupHeight, EAR_CUP_CORNER);
    icon.addRoundedRectangle(centreX + bandRadius + 1.0F - cupWidth, cupTop, cupWidth, cupHeight, EAR_CUP_CORNER);
    return icon;
}

/// Returns the headphone icon filled with a colour.
/// Every state of the Solo button uses the same path, so the icon does not move between them.
std::unique_ptr<juce::Drawable> headphoneIcon(const juce::Colour colour)
{
    auto icon = std::make_unique<juce::DrawablePath>();
    icon->setPath(headphonePath());
    icon->setFill(colour);
    return icon;
}

/// Sets a parameter to a plain value, inside the gesture already open, or in a gesture of its own.
void setSideValue(juce::RangedAudioParameter& parameter, const float value, const bool inGesture)
{
    if (inGesture) {
        parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
    } else {
        setParameterInGesture(parameter, value);
    }
}
}  // namespace

/// Names the meter with its band so assistive technology can distinguish the four measurements.
BandCorrelationMeter::BandCorrelationMeter(const juce::String& band)
{
    setTitle(band + " correlation");
    setDescription(
        "Stereo correlation: -1 anti-phase, 0 uncorrelated, +1 in-phase. "
        "The number shows the trend by default, the yellow highlight shows the trend, the coloured line is live, "
        "and the fading trail shows the recent range. Silence is unavailable"
    );
    setTooltip(
        "Processed band correlation: -1 is anti-phase, 0 is uncorrelated, +1 is in-phase. "
        "The coloured line is live, the yellow highlight shows the trend, and the soft trail fades with age. "
        "Numbers show the trend by default. Turn off Smooth correlation numbers in Settings for live numbers. "
        "Persistence controls trend smoothing and range duration. Silence is unavailable"
    );
}

/// Returns the selected signed reading, leaving the visual readout blank when unavailable.
juce::String BandCorrelationMeter::readoutText() const
{
    return trend.has_value() ? valueText() : juce::String {};
}

/// Returns the selected trend or live value for visual and accessible readouts, or an unavailable status.
juce::String BandCorrelationMeter::valueText() const
{
    const auto reading = smoothedReadout ? trend : correlation;
    if (!reading.has_value()) {
        return "Unavailable";
    }
    return (*reading >= 0.0F ? "+" : "") + juce::String(*reading, 2);
}

/// Changes only the numerical presentation, preserving trend and range history across mode switches.
void BandCorrelationMeter::setSmoothedReadout(const bool smoothed)
{
    if (smoothedReadout == smoothed) {
        return;
    }
    const auto previousValueText = valueText();
    smoothedReadout = smoothed;
    repaint();
    if (previousValueText != valueText()) {
        if (const auto* handler = getAccessibilityHandler()) {
            handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
        }
    }
}

/// Returns the display-smoothed trend shared by the highlight and numerical readout.
std::optional<float> BandCorrelationMeter::trendValue() const
{
    return trend;
}

/// Returns the recent display-observed extrema, not guaranteed audio-thread extrema.
std::optional<juce::Range<float>> BandCorrelationMeter::recentRange() const
{
    return range;
}

/// Clears all presentation history through the same unavailable path as an invalid measurement.
void BandCorrelationMeter::clearHistory()
{
    setCorrelation(std::numeric_limits<float>::quiet_NaN());
}

/// Advances message-thread history in real seconds, including when the live reading is unchanged.
/// Buckets retain extrema at arbitrary refresh rates, with at most one bucket of late expiry.
void BandCorrelationMeter::setCorrelation(
    const float value,
    const double elapsedSeconds,
    const double persistenceSeconds
)
{
    const auto previousValueText = valueText();
    const auto previousTrend = trend;
    const auto previousRange = range;
    const auto next = std::isfinite(value) ? std::optional<float>(std::clamp(value, -1.0F, 1.0F)) : std::nullopt;
    const bool liveChanged = correlation.has_value() != next.has_value()
        || (next.has_value() && !juce::approximatelyEqual(*correlation, *next));
    const double persistence = std::isfinite(persistenceSeconds)
        ? std::clamp(persistenceSeconds, MIN_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS)
        : DEFAULT_PERSISTENCE_SECONDS;
    if (!next.has_value() || !std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0
        || elapsedSeconds > MAX_PERSISTENCE_SECONDS)
    {
        historyBegin = 0;
        historyCount = 0;
        historySeconds = 0.0;
        trend.reset();
        range.reset();
    }
    correlation = next;
    if (next.has_value()) {
        const double elapsed
            = std::isfinite(elapsedSeconds) && elapsedSeconds >= 0.0 && elapsedSeconds <= MAX_PERSISTENCE_SECONDS
            ? elapsedSeconds
            : 0.0;
        advanceHistory(*next, elapsed, persistence);
    }
    const bool trendChanged = previousTrend.has_value() != trend.has_value()
        || (previousTrend.has_value() && trend.has_value() && !juce::approximatelyEqual(*previousTrend, *trend));
    const bool rangeChanged = previousRange.has_value() != range.has_value()
        || (previousRange.has_value() && range.has_value()
            && (!juce::approximatelyEqual(previousRange->getStart(), range->getStart())
                || !juce::approximatelyEqual(previousRange->getEnd(), range->getEnd())));
    const bool intensityChanged = updateRangeIntensity(persistence);
    if (liveChanged || trendChanged || rangeChanged || intensityChanged) {
        repaint();
    }
    if (previousValueText != valueText()) {
        if (const auto* handler = getAccessibilityHandler()) {
            handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
        }
    }
}

/// Accumulates a valid observation and selects its trailing window from bounded maximum-duration history.
void BandCorrelationMeter::advanceHistory(
    const float value,
    const double elapsedSeconds,
    const double persistenceSeconds
)
{
    historySeconds += elapsedSeconds;
    if (trend.has_value()) {
        const double weight = -std::expm1(-elapsedSeconds / persistenceSeconds);
        trend = static_cast<float>(static_cast<double>(*trend) + weight * static_cast<double>(value - *trend));
    } else {
        trend = value;
    }
    // Keep maximum-duration history so increasing Persistence can reveal recent observations immediately.
    const double oldestRetained = historySeconds - MAX_PERSISTENCE_SECONDS;
    while (historyCount > 0 && history.at(historyBegin).startSeconds + HISTORY_BUCKET_SECONDS <= oldestRetained) {
        historyBegin = (historyBegin + 1) % HISTORY_CAPACITY;
        --historyCount;
    }
    const double bucketStart = std::floor(historySeconds / HISTORY_BUCKET_SECONDS) * HISTORY_BUCKET_SECONDS;
    const std::size_t last = (historyBegin + historyCount + HISTORY_CAPACITY - 1) % HISTORY_CAPACITY;
    if (historyCount > 0 && bucketStart <= history.at(last).startSeconds) {
        auto& bucket = history.at(last);
        if (value <= bucket.minimum) {
            bucket.minimum = value;
            bucket.minimumSeconds = historySeconds;
        }
        if (value >= bucket.maximum) {
            bucket.maximum = value;
            bucket.maximumSeconds = historySeconds;
        }
    } else {
        if (historyCount == HISTORY_CAPACITY) {
            historyBegin = (historyBegin + 1) % HISTORY_CAPACITY;
            --historyCount;
        }
        history.at((historyBegin + historyCount) % HISTORY_CAPACITY) = {
            .startSeconds = bucketStart,
            .minimum = value,
            .maximum = value,
            .minimumSeconds = historySeconds,
            .maximumSeconds = historySeconds,
        };
        ++historyCount;
    }
    float minimum = value;
    float maximum = value;
    const double oldestShown = historySeconds - persistenceSeconds;
    for (std::size_t index = 0; index < historyCount; ++index) {
        const auto& bucket = history.at((historyBegin + index) % HISTORY_CAPACITY);
        if (bucket.startSeconds + HISTORY_BUCKET_SECONDS > oldestShown) {
            minimum = std::min(minimum, bucket.minimum);
            maximum = std::max(maximum, bucket.maximum);
        }
    }
    range = juce::Range<float>(minimum, maximum);
}

/// Rebuilds a soft coverage field from the same retained observations as the exact range.
/// Each old extreme fades with a smoothstep envelope that reaches zero at Persistence, with no extra release tail.
/// Connecting each extreme to the live reading keeps the range filled while its oldest portions fade independently.
bool BandCorrelationMeter::updateRangeIntensity(const double persistenceSeconds)
{
    const auto previous = rangeIntensity;
    rangeIntensity.fill(0.0F);
    if (!correlation.has_value()) {
        return std::ranges::any_of(previous, [](const float intensity) { return intensity > 0.0F; });
    }
    float minimumCovered = *correlation;
    float maximumCovered = *correlation;
    double minimumCoveredSeconds = historySeconds;
    double maximumCoveredSeconds = historySeconds;
    const auto addObservation = [&](const float value, const double observedSeconds) {
        // Newer outer observations already cover an older inner interval, including its identical soft edge.
        if (value >= minimumCovered && value <= maximumCovered
            && observedSeconds < std::min(minimumCoveredSeconds, maximumCoveredSeconds))
        {
            return;
        }
        const double remaining = std::clamp(1.0 - (historySeconds - observedSeconds) / persistenceSeconds, 0.0, 1.0);
        const auto opacity = static_cast<float>(remaining * remaining * (3.0 - 2.0 * remaining));
        if (opacity <= 0.0F) {
            return;
        }
        const float minimum = std::min(value, *correlation);
        const float maximum = std::max(value, *correlation);
        constexpr float spatialStep {2.0F / static_cast<float>(RANGE_INTENSITY_SAMPLES - 1)};
        for (std::size_t sample = 0; sample < rangeIntensity.size(); ++sample) {
            const float position = -1.0F + static_cast<float>(sample) * spatialStep;
            const float distance = std::max({minimum - position, position - maximum, 0.0F});
            const float coverage = std::clamp(1.0F - distance / CORRELATION_RANGE_FEATHER, 0.0F, 1.0F);
            const float feather = coverage * coverage * (3.0F - 2.0F * coverage);
            rangeIntensity.at(sample) = std::max(rangeIntensity.at(sample), opacity * feather);
        }
        if (value < minimumCovered) {
            minimumCovered = value;
            minimumCoveredSeconds = observedSeconds;
        }
        if (value > maximumCovered) {
            maximumCovered = value;
            maximumCoveredSeconds = observedSeconds;
        }
    };
    addObservation(*correlation, historySeconds);
    for (std::size_t index = historyCount; index > 0; --index) {
        const auto& bucket = history.at((historyBegin + index - 1) % HISTORY_CAPACITY);
        addObservation(bucket.minimum, bucket.minimumSeconds);
        addObservation(bucket.maximum, bucket.maximumSeconds);
    }
    return !std::ranges::equal(previous, rangeIntensity, [](const float first, const float second) {
        return juce::approximatelyEqual(first, second);
    });
}

/// Layers a soft fading range, an integrated yellow trend glow, and an immediate live marker above the scale.
void BandCorrelationMeter::paint(juce::Graphics& graphics)
{
    constexpr int meterMargin {10};
    constexpr int meterLabelHeight {17};
    constexpr float trackHeight {5.0F};
    constexpr float markerWidth {3.0F};
    constexpr float meterFontHeight {11.0F};
    constexpr float markerExtension {2.0F};
    auto area = getLocalBounds().reduced(meterMargin, 2);
    graphics.setFont(textFont(meterFontHeight));
    graphics.setColour(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    graphics.drawText("Correlation", area.removeFromTop(meterLabelHeight), juce::Justification::centredLeft);
    const juce::Rectangle<float> track {
        static_cast<float>(area.getX()),
        static_cast<float>(area.getY()),
        static_cast<float>(area.getWidth()),
        trackHeight
    };
    graphics.setColour(juce::Colour(palette::GREY_MEDIUM));
    graphics.fillRoundedRectangle(track, trackHeight * 0.5F);
    const auto positionFor
        = [&track](const float value) { return track.getX() + (value + 1.0F) * 0.5F * track.getWidth(); };
    if (correlation.has_value() && track.getWidth() > 0.0F) {
        constexpr float rangeLightShare {0.55F};
        const auto rangeColour
            = juce::Colour(palette::BLUE).interpolatedWith(juce::Colour(palette::GREY_LIGHT), rangeLightShare);
        // Lift faint, older coverage without changing its duration or introducing a hard opacity floor.
        const auto visibleAlpha = [](const float intensity) { return intensity * 2.0F / (1.0F + intensity); };
        juce::ColourGradient gradient(
            rangeColour.withAlpha(visibleAlpha(rangeIntensity.front())),
            track.getX(),
            track.getCentreY(),
            rangeColour.withAlpha(visibleAlpha(rangeIntensity.back())),
            track.getRight(),
            track.getCentreY(),
            false
        );
        constexpr double colourStep {1.0 / static_cast<double>(RANGE_INTENSITY_SAMPLES - 1)};
        for (std::size_t sample = 1; sample + 1 < rangeIntensity.size(); ++sample) {
            gradient.addColour(
                static_cast<double>(sample) * colourStep, rangeColour.withAlpha(visibleAlpha(rangeIntensity.at(sample)))
            );
        }
        graphics.setGradientFill(gradient);
        constexpr std::array layerExtensions {4.0F, 2.0F, 0.0F};
        constexpr std::array layerAlphas {0.14F, 0.34F, 0.6F};
        for (std::size_t layer = 0; layer < layerExtensions.size(); ++layer) {
            graphics.setOpacity(layerAlphas.at(layer));
            graphics.fillRoundedRectangle(track.expanded(0.0F, layerExtensions.at(layer)), trackHeight * 0.5F);
        }
    }
    graphics.setColour(juce::Colour(palette::GREY_LIGHT));
    graphics.fillRect(track.getCentreX(), track.getY() - markerExtension, 1.0F, trackHeight + markerExtension * 2.0F);
    if (trend.has_value()) {
        constexpr float trendGlowHalfWidth {12.0F};
        constexpr float trendCoreWidth {2.0F};
        constexpr float trendGlowAlpha {0.7F};
        constexpr float trendHaloAlpha {0.22F};
        constexpr float trendShoulderAlpha {0.2F};
        constexpr double trendLeftShoulder {0.25};
        constexpr double trendRightShoulder {1.0 - trendLeftShoulder};
        const float position = positionFor(*trend);
        const auto yellow = juce::Colour(palette::YELLOW);
        juce::ColourGradient glow(
            yellow.withAlpha(0.0F),
            position - trendGlowHalfWidth,
            track.getCentreY(),
            yellow.withAlpha(0.0F),
            position + trendGlowHalfWidth,
            track.getCentreY(),
            false
        );
        glow.addColour(trendLeftShoulder, yellow.withAlpha(trendShoulderAlpha));
        glow.addColour(0.5, yellow.withAlpha(trendGlowAlpha));
        glow.addColour(trendRightShoulder, yellow.withAlpha(trendShoulderAlpha));
        const juce::Graphics::ScopedSaveState saved(graphics);
        graphics.reduceClipRegion(track.expanded(0.0F, markerExtension * 2.0F).getSmallestIntegerContainer());
        graphics.setGradientFill(glow);
        const auto highlight = track.withLeft(position - trendGlowHalfWidth).withWidth(trendGlowHalfWidth * 2.0F);
        graphics.setOpacity(trendHaloAlpha);
        graphics.fillRoundedRectangle(highlight.expanded(0.0F, markerExtension * 2.0F), trackHeight * 0.5F);
        graphics.setOpacity(1.0F);
        graphics.fillRoundedRectangle(highlight, trackHeight * 0.5F);
        graphics.setColour(yellow);
        graphics.fillRoundedRectangle(
            std::clamp(position - trendCoreWidth * 0.5F, track.getX(), track.getRight() - trendCoreWidth),
            track.getY() - markerExtension,
            trendCoreWidth,
            trackHeight + markerExtension * 2.0F,
            trendCoreWidth * 0.5F
        );
    }
    if (correlation.has_value()) {
        const float position = positionFor(*correlation);
        graphics.setColour(juce::Colour(*correlation < 0.0F ? palette::ORANGE : palette::BLUE));
        graphics.fillRect(
            std::clamp(position - markerWidth * 0.5F, track.getX(), track.getRight() - markerWidth),
            track.getY() - markerExtension,
            markerWidth,
            trackHeight + markerExtension * 2.0F
        );
    }
    area.removeFromTop(static_cast<int>(trackHeight) + 2);
    graphics.setColour(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    graphics.drawText("-1", area, juce::Justification::centredLeft);
    graphics.drawText("0", area, juce::Justification::centred);
    graphics.drawText("+1", area, juce::Justification::centredRight);
    const auto readout = getLocalBounds().reduced(meterMargin, 2).removeFromTop(meterLabelHeight);
    graphics.setFont(monoFont(meterFontHeight));
    graphics.drawText(readoutText(), readout, juce::Justification::centredRight);
}

/// Exposes a named, read-only text value without slider actions.
std::unique_ptr<juce::AccessibilityHandler> BandCorrelationMeter::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(
        *this,
        juce::AccessibilityRole::progressBar,
        juce::AccessibilityActions {},
        juce::AccessibilityHandler::Interfaces {std::make_unique<CorrelationAccessibleValue>(*this)}
    );
}

/// Creates the controls of a band, attaches Mid, Side, Width, Recover, Pan, and Solo to their parameters,
/// and binds Link to the editor settings.
BandStrip::BandStrip(
    juce::AudioProcessorValueTreeState& parameters,
    juce::ValueTree& editorState,
    const std::size_t band,
    ParameterHistory& localHistory
) :
    sideParameter(*parameters.getParameter(sideGainParameterId(band))),
    history(localHistory),
    sideId(sideGainParameterId(band))
{
    setTitle(bandName(band) + " band controls");
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    nameLabel.setText(bandName(band), juce::dontSendNotification);
    nameLabel.setFont(semiboldTextFont(NAME_FONT_HEIGHT));
    nameLabel.setBorderSize({});
    nameLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_SUPER_LIGHT));
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    rangeLabel.setFont(textFont(LABEL_FONT_HEIGHT));
    rangeLabel.setBorderSize({});
    rangeLabel.setJustificationType(juce::Justification::centredLeft);
    rangeLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_MIDDLE_LIGHT));
    addAndMakeVisible(nameLabel);
    addAndMakeVisible(rangeLabel);

    configureSlider(midSlider, midLabel, "Mid");
    configureSlider(sideSlider, sideLabel, "Side");
    midSlider.setDoubleClickReturnValue(true, 0.0);
    sideSlider.setDoubleClickReturnValue(true, 0.0);
    midSlider.setTooltip("Level of the band's mid, what both channels share. Double-click to reset");
    sideSlider.setTooltip(
        "Level of the band's side, what differs between the channels. Off makes the band mono. Double-click to reset"
    );
    midAttachment = std::make_unique<HistorySliderAttachment>(parameters, midGainParameterId(band), midSlider, history);
    sideAttachment
        = std::make_unique<HistorySliderAttachment>(parameters, sideGainParameterId(band), sideSlider, history);

    configureSlider(widthSlider, widthLabel, "Width");
    widthSlider.setWheelStep(WIDTH_WHEEL_STEP);
    widthSlider.setTooltip(
        "Independent side multiplier after Mid and Side trims: 0 is mono, 1 is unchanged, and 2 doubles Side. "
        "Double-click to reset"
    );
    widthSlider.setDoubleClickReturnValue(true, 1.0);
    widthAttachment
        = std::make_unique<HistorySliderAttachment>(parameters, widthParameterId(band), widthSlider, history);

    configureSlider(recoverSlider, recoverLabel, "Recover");
    recoverSlider.setWheelStep(RECOVER_WHEEL_STEP_PERCENT);
    recoverSlider.setTooltip(
        "Mix phase-rotated removed Side detail into Mid. Only works with Width below 1. "
        "Side off disables the recovery source. Mid off can still retain recovered Side. Double-click to turn off"
    );
    recoverSlider.setDoubleClickReturnValue(true, 0.0);
    recoverAttachment
        = std::make_unique<HistorySliderAttachment>(parameters, recoverParameterId(band), recoverSlider, history);

    configureSlider(panSlider, panLabel, "Pan");
    panSlider.setDoubleClickReturnValue(true, 0.0);
    panSlider.setWheelStep(PAN_WHEEL_STEP);
    panSlider.setTooltip("Moves the band toward the left or right channel after its width. Double-click to centre");
    panAttachment = std::make_unique<HistorySliderAttachment>(parameters, panParameterId(band), panSlider, history);

    linkButton.getToggleStateValue().referTo(editorState.getPropertyAsValue(linkPropertyName(band), nullptr));
    linkButton.setTooltip(
        "Editing Mid also moves the Side trim by the same amount, including drag, wheel, keys, and numeric entry. "
        "Width and Recover stay independent"
    );
    addAndMakeVisible(linkButton);

    const auto offIcon = headphoneIcon(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    const auto offHoverIcon = headphoneIcon(juce::Colour(palette::GREY_SUPER_LIGHT));
    const auto onIcon = headphoneIcon(juce::Colour(palette::YELLOW));
    const auto onHoverIcon = headphoneIcon(juce::Colour(palette::YELLOW).brighter(SOLO_HOVER_BRIGHTNESS));
    soloButton.setImages(
        offIcon.get(), offHoverIcon.get(), onIcon.get(), nullptr, onIcon.get(), onHoverIcon.get(), onHoverIcon.get()
    );
    soloButton.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    soloButton.setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
    soloButton.setEdgeIndent(SOLO_ICON_INDENT);
    soloButton.setClickingTogglesState(true);
    soloButton.setTooltip(
        "Solo: listen to this band only. Solo several bands to hear them together. Press S over a band to toggle it"
    );
    soloAttachment = std::make_unique<HistoryButtonAttachment>(parameters, soloParameterId(band), soloButton, history);
    addAndMakeVisible(soloButton);

    midSlider.onDragStart = [this] {
        linkedGeneration = history.generation();
        linkedHistoryEditing = history.isEditing();
        // The slider listener ends before onDragEnd, so this nested frame keeps linked writes in one command.
        // A restore from beginChangeGesture cancels the listener's frame and must not open a replacement here.
        if (linkedHistoryEditing) {
            history.beginEdit(midSlider.getTitle());
        }
        draggingMid = true;
        linkedMidStart = midSlider.getValue();
        linkedSideStart = sideSlider.getValue();
        linkedSideGesture = linkButton.getToggleState();
        if (linkedSideGesture) {
            sideParameter.beginChangeGesture();
        }
    };
    midSlider.onDragEnd = [this] {
        if (linkedSideGesture) {
            sideParameter.endChangeGesture();
            linkedSideGesture = false;
        }
        draggingMid = false;
        if (linkedHistoryEditing && linkedGeneration == history.generation()) {
            history.endEdit();
        }
        linkedHistoryEditing = false;
    };
    midSlider.onValueChange = [this] {
        if (!linkedHistoryEditing || linkedGeneration != history.generation()) {
            return;
        }
        if (draggingMid && linkedSideGesture && linkedSideStart > static_cast<double>(BAND_GAIN_MIN_DB)) {
            const double side = std::clamp(
                linkedSideStart + midSlider.getValue() - linkedMidStart,
                static_cast<double>(BAND_GAIN_MIN_DB),
                static_cast<double>(BAND_GAIN_MAX_DB)
            );
            history.touch(sideId);
            setSideValue(sideParameter, static_cast<float>(side), true);
        }
    };

    for (auto* slider : {&midSlider, &sideSlider, &panSlider, &widthSlider, &recoverSlider}) {
        slider->setTitle(bandName(band) + " " + slider->getName());
    }
    soloButton.setTitle(bandName(band) + " Solo");
    linkButton.setTitle(bandName(band) + " Link");
    soloButton.setWantsKeyboardFocus(true);
    linkButton.setWantsKeyboardFocus(true);
    int focusOrder = 1;
    for (auto* control :
         std::array<juce::Component*, 7> {
             &soloButton,
             &linkButton,
             &midSlider,
             &sideSlider,
             &widthSlider,
             &recoverSlider,
             &panSlider,
         })
    {
        control->setExplicitFocusOrder(focusOrder++);
    }
}

/// Closes active gestures and detaches the sliders before they are destroyed.
BandStrip::~BandStrip()
{
    for (auto* slider : {&midSlider, &sideSlider, &panSlider, &widthSlider, &recoverSlider}) {
        slider->hideTextBox(true);
        slider->endDrag();
        slider->onDragStart = nullptr;
        slider->onDragEnd = nullptr;
        slider->onValueChange = nullptr;
    }
    midAttachment.reset();
    sideAttachment.reset();
    panAttachment.reset();
    widthAttachment.reset();
    recoverAttachment.reset();
    soloAttachment.reset();
}

/// Checks uncompressed heading glyphs against the current width, including buttons, gaps, and disclosure space.
/// The message-thread editor combines all bands' fit results before selecting their shared heading layout.
bool BandStrip::canFitInlineRange()
{
    const int requiredWidth = MARGIN * 2 + headerDisclosureInset + LINK_WIDTH + SOLO_GAP + HEADER_ROW_HEIGHT
        + headingLabelWidth(nameLabel) + headingLabelWidth(rangeLabel) + HEADER_TEXT_GAP * 2;
    return rangeLabel.getText().isNotEmpty() && getWidth() >= requiredWidth;
}

/// Cancels unfinished numeric text after a host restore, without overwriting restored parameter values.
void BandStrip::discardPendingEdits()
{
    for (auto* slider : {&midSlider, &sideSlider, &widthSlider, &recoverSlider, &panSlider}) {
        slider->hideTextBox(true);
        slider->endDrag();
    }
}

/// Finishes edits before changing geometry, retaining controls, attachments, tooltips, and editor-only Link state.
/// Focus on controls hidden by compaction moves to Mid. Focus on the four retained values stays on its slider.
void BandStrip::setCompact(const bool shouldBeCompact)
{
    if (compact == shouldBeCompact) {
        return;
    }
    juce::Component::SafePointer<juce::Component> focusedControl;
    for (auto* control :
         std::array<juce::Component*, 7> {
             &midSlider,
             &sideSlider,
             &widthSlider,
             &recoverSlider,
             &panSlider,
             &soloButton,
             &linkButton,
         })
    {
        if (control->hasKeyboardFocus(true)) {
            focusedControl = control;
            break;
        }
    }
    for (auto* slider : {&midSlider, &sideSlider, &widthSlider, &recoverSlider, &panSlider}) {
        slider->hideTextBox(false);
        slider->endDrag();
    }
    compact = shouldBeCompact;
    midSlider.setCompactValue(compact ? "M" : "");
    sideSlider.setCompactValue(compact ? "S" : "");
    widthSlider.setCompactValue(compact ? "W" : "");
    panSlider.setCompactValue(compact ? "P" : "");
    for (auto* control :
         std::array<juce::Component*, 10> {
             &nameLabel,
             &rangeLabel,
             &midLabel,
             &sideLabel,
             &widthLabel,
             &recoverLabel,
             &panLabel,
             &recoverSlider,
             &soloButton,
             &linkButton,
         })
    {
        control->setVisible(!compact);
    }
    resized();
    if (focusedControl != nullptr && isShowing()) {
        if (compact && focusedControl.getComponent() != &midSlider && focusedControl.getComponent() != &sideSlider
            && focusedControl.getComponent() != &widthSlider && focusedControl.getComponent() != &panSlider)
        {
            midSlider.grabKeyboardFocus();
        } else {
            focusedControl->grabKeyboardFocus();
        }
    }
}

/// Reserves editor-owned disclosure space without overlapping Solo or Link in the full heading.
void BandStrip::setHeaderDisclosureInset(const int inset)
{
    headerDisclosureInset = std::max(0, inset);
    resized();
}

/// Applies the message-thread editor's shared wrapping requirement without touching numeric drafts or gestures.
void BandStrip::setRangeOnSecondRow(const bool secondRow)
{
    if (rangeOnSecondRow == secondRow) {
        return;
    }
    rangeOnSecondRow = secondRow;
    if (!compact) {
        resized();
    }
}

/// Paints the rounded background of the strip.
void BandStrip::paint(juce::Graphics& graphics)
{
    graphics.setColour(juce::Colour(palette::GREY_SEMI_DARK));
    graphics.fillRoundedRectangle(getLocalBounds().toFloat(), corner_radius::PANEL);
}

/// Uses one compact row of four values, with inline full names only when their worst-case glyphs fit together.
/// Expanded headings share a row only when both full labels fit and the editor permits the shared inline layout.
/// Otherwise the range keeps its own row. A gap takes at most 16 spare pixels before the five fixed-font slider rows,
/// which share the remaining height evenly without interrupting their editors or gestures.
void BandStrip::resized()
{
    if (compact) {
        auto values = getLocalBounds().reduced(COMPACT_MARGIN).removeFromTop(COMPACT_VALUE_HEIGHT);
        const int fieldSpace = std::max(0, values.getWidth() - COMPACT_VALUE_GAP * 3);
        const std::array controls {&midSlider, &sideSlider, &widthSlider, &panSlider};
        std::array<int, controls.size()> fullNameWidths {};
        int fullNameSpace = 0;
        for (std::size_t index = 0; index < controls.size(); ++index) {
            fullNameWidths.at(index) = controls.at(index)->compactValueRequiredWidth(true);
            fullNameSpace += fullNameWidths.at(index);
        }
        // Full names share one band-wide mode and keep worst-case values clear without automation-driven flipping.
        const bool fullNames = fieldSpace >= fullNameSpace;
        for (std::size_t index = 0; index < controls.size(); ++index) {
            const int allocatedWidth = fullNames
                ? fullNameWidths.at(index) + (fieldSpace - fullNameSpace) * fullNameWidths.at(index) / fullNameSpace
                : fieldSpace * COMPACT_FIELD_WEIGHTS.at(index) / COMPACT_TOTAL_FIELD_WEIGHT;
            const int fieldWidth = index + 1 == controls.size() ? values.getWidth() : allocatedWidth;
            auto& slider = *controls.at(index);
            slider.setCompactValueFullNames(fullNames);
            slider.setBounds(values.removeFromLeft(fieldWidth));
            values.removeFromLeft(COMPACT_VALUE_GAP);
        }
        return;
    }
    auto area = getLocalBounds().reduced(MARGIN);
    auto header = area.removeFromTop(HEADER_ROW_HEIGHT).withTrimmedRight(headerDisclosureInset);
    linkButton.setBounds(header.removeFromRight(LINK_WIDTH));
    header.removeFromRight(SOLO_GAP);
    soloButton.setBounds(header.removeFromRight(HEADER_ROW_HEIGHT));
    const int nameWidth = headingLabelWidth(nameLabel);
    const bool inlineRange = !rangeOnSecondRow && canFitInlineRange();
    if (inlineRange) {
        nameLabel.setBounds(header.removeFromLeft(nameWidth));
        header.removeFromLeft(HEADER_TEXT_GAP);
        rangeLabel.setBounds(header.withTrimmedRight(HEADER_TEXT_GAP));
    } else {
        nameLabel.setBounds(header);
        rangeLabel.setBounds(area.removeFromTop(HEADER_ROW_HEIGHT));
    }
    const int spareHeight = area.getHeight() - SLIDER_ROWS * (SLIDER_LABEL_HEIGHT + SLIDER_HEIGHT);
    area.removeFromTop(std::clamp(spareHeight, 0, HEADER_SLIDER_GAP));

    const int rowHeight = area.getHeight() / SLIDER_ROWS;
    const std::array rows {
        std::pair {&midLabel, &midSlider},
        std::pair {&sideLabel, &sideSlider},
        std::pair {&widthLabel, &widthSlider},
        std::pair {&recoverLabel, &recoverSlider},
        std::pair {&panLabel, &panSlider},
    };
    static_assert(rows.size() == SLIDER_ROWS);
    for (const auto& [label, slider] : rows) {
        auto row = area.removeFromTop(rowHeight);
        label->setBounds(row.removeFromTop(SLIDER_LABEL_HEIGHT));
        slider->setBounds(row.removeFromTop(SLIDER_HEIGHT));
    }
}

/// Updates the message-thread range and its measured heading layout only when the crossover text changes.
/// Compact controls, active numeric drafts, and host gestures retain their presentation and ownership.
void BandStrip::setRangeText(const juce::String& text)
{
    if (rangeLabel.getText() == text) {
        return;
    }
    rangeLabel.setText(text, juce::dontSendNotification);
    if (!compact) {
        resized();
    }
}

/// Sets up a full-width horizontal slider with its value box on the right and its label above it.
void BandStrip::configureSlider(ControlSlider& slider, juce::Label& label, const juce::String& text)
{
    slider.setName(text);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, TEXT_BOX_WIDTH, TEXT_BOX_HEIGHT);
    addAndMakeVisible(slider);
    label.setText(text, juce::dontSendNotification);
    label.setFont(textFont(LABEL_FONT_HEIGHT));
    label.setJustificationType(juce::Justification::bottomLeft);
    label.setBorderSize({});
    addAndMakeVisible(label);
}

}  // namespace esgrove::image_control
