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
#include <functional>
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
/// How much brighter the headphone icon draws while the pointer is over the Solo button.
constexpr float SOLO_HOVER_BRIGHTNESS {0.3F};
/// Largest vertical gap between the range label and the first slider row, in pixels.
/// The gap only takes height the slider rows do not need, so it is gone at the minimum strip height.
constexpr int HEADER_SLIDER_GAP {16};
/// Inner margin of the strip, in pixels.
constexpr int MARGIN {10};
/// Corner radius of the strip background, in pixels.
constexpr float CORNER_RADIUS {6.0F};
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
/// Thickness of the headband of the headphone icon, in icon units.
constexpr float HEADBAND_THICKNESS {2.0F};
/// Corner radius of the ear cups of the headphone icon, in icon units.
constexpr float EAR_CUP_CORNER {1.5F};

static_assert(
    BAND_STRIP_MIN_HEIGHT == MARGIN * 2 + HEADER_ROW_HEIGHT * 2 + SLIDER_ROWS * (SLIDER_LABEL_HEIGHT + SLIDER_HEIGHT),
    "The minimum strip height must fit the margins, the header rows, and the slider rows"
);

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
        "The number shows the trend by default, the notch shows the trend, the coloured line is live, "
        "and the box shows the recent range. Silence is unavailable"
    );
    setTooltip(
        "Processed band correlation: -1 is anti-phase, 0 is uncorrelated, +1 is in-phase. "
        "The coloured line is live, the yellow notch shows the trend, and the shaded box is the recent range. "
        "Numbers show the trend by default. Turn off Smooth correlation numbers in Settings for live numbers. "
        "Persistence controls trend smoothing and range duration. Silence is unavailable"
    );
}

/// Returns the selected signed reading or a single Unicode em dash when no valid measurement is available.
juce::String BandCorrelationMeter::readoutText() const
{
    return trend.has_value() ? valueText() : juce::String(u8"\u2014");
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

/// Returns the display-smoothed trend shared by the notch and numerical readout.
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
    if (liveChanged || trendChanged || rangeChanged) {
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
        bucket.minimum = std::min(bucket.minimum, value);
        bucket.maximum = std::max(bucket.maximum, value);
    } else {
        if (historyCount == HISTORY_CAPACITY) {
            historyBegin = (historyBegin + 1) % HISTORY_CAPACITY;
            --historyCount;
        }
        history.at((historyBegin + historyCount) % HISTORY_CAPACITY)
            = {.startSeconds = bucketStart, .minimum = value, .maximum = value};
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

/// Layers the observed range, zero tick, trend notch, and live marker above the correlation scale.
void BandCorrelationMeter::paint(juce::Graphics& graphics)
{
    constexpr int METER_MARGIN {10};
    constexpr int METER_LABEL_HEIGHT {17};
    constexpr float TRACK_HEIGHT {5.0F};
    constexpr float MARKER_WIDTH {3.0F};
    constexpr float METER_FONT_HEIGHT {11.0F};
    constexpr float MARKER_EXTENSION {2.0F};
    auto area = getLocalBounds().reduced(METER_MARGIN, 2);
    graphics.setFont(textFont(METER_FONT_HEIGHT));
    graphics.setColour(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    graphics.drawText("Correlation", area.removeFromTop(METER_LABEL_HEIGHT), juce::Justification::centredLeft);
    const juce::Rectangle<float> track {
        static_cast<float>(area.getX()),
        static_cast<float>(area.getY()),
        static_cast<float>(area.getWidth()),
        TRACK_HEIGHT
    };
    graphics.setColour(juce::Colour(palette::GREY_MEDIUM));
    graphics.fillRoundedRectangle(track, TRACK_HEIGHT * 0.5F);
    const auto positionFor
        = [&track](const float value) { return track.getX() + (value + 1.0F) * 0.5F * track.getWidth(); };
    if (range.has_value()) {
        constexpr float MIN_RANGE_WIDTH {2.0F};
        constexpr float RANGE_ALPHA {0.35F};
        const float width = std::clamp(
            positionFor(range->getEnd()) - positionFor(range->getStart()), MIN_RANGE_WIDTH, track.getWidth()
        );
        const float left = std::clamp(positionFor(range->getStart()), track.getX(), track.getRight() - width);
        const juce::Rectangle<float> box {
            left, track.getY() - MARKER_EXTENSION, width, TRACK_HEIGHT + MARKER_EXTENSION * 2.0F
        };
        graphics.setColour(juce::Colour(palette::GREY_SUPER_LIGHT).withAlpha(RANGE_ALPHA));
        graphics.fillRect(box);
        graphics.drawRect(box, 1.0F);
    }
    graphics.setColour(juce::Colour(palette::GREY_LIGHT));
    graphics.fillRect(
        track.getCentreX(), track.getY() - MARKER_EXTENSION, 1.0F, TRACK_HEIGHT + MARKER_EXTENSION * 2.0F
    );
    if (trend.has_value()) {
        constexpr float TREND_HALF_WIDTH {3.0F};
        constexpr float TREND_HEIGHT {3.0F};
        const float position
            = std::clamp(positionFor(*trend), track.getX() + TREND_HALF_WIDTH, track.getRight() - TREND_HALF_WIDTH);
        juce::Path notch;
        notch.addTriangle(
            position - TREND_HALF_WIDTH,
            track.getY() - MARKER_EXTENSION,
            position + TREND_HALF_WIDTH,
            track.getY() - MARKER_EXTENSION,
            position,
            track.getY() - MARKER_EXTENSION + TREND_HEIGHT
        );
        graphics.setColour(juce::Colour(palette::YELLOW));
        graphics.fillPath(notch);
    }
    if (correlation.has_value()) {
        const float position = positionFor(*correlation);
        graphics.setColour(juce::Colour(*correlation < 0.0F ? palette::ORANGE : palette::BLUE));
        graphics.fillRect(
            std::clamp(position - MARKER_WIDTH * 0.5F, track.getX(), track.getRight() - MARKER_WIDTH),
            track.getY() - MARKER_EXTENSION,
            MARKER_WIDTH,
            TRACK_HEIGHT + MARKER_EXTENSION * 2.0F
        );
    }
    area.removeFromTop(static_cast<int>(TRACK_HEIGHT) + 2);
    graphics.setColour(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    graphics.drawText("-1", area, juce::Justification::centredLeft);
    graphics.drawText("0", area, juce::Justification::centred);
    graphics.drawText("+1", area, juce::Justification::centredRight);
    const auto readout = getLocalBounds().reduced(METER_MARGIN, 2).removeFromTop(METER_LABEL_HEIGHT);
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
    std::optional<std::reference_wrapper<ParameterHistory>> localHistory
) :
    sideParameter(*parameters.getParameter(sideGainParameterId(band))),
    history(localHistory),
    sideId(sideGainParameterId(band))
{
    nameLabel.setText(bandName(band), juce::dontSendNotification);
    nameLabel.setFont(textFont(NAME_FONT_HEIGHT).boldened());
    nameLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_SUPER_LIGHT));
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    rangeLabel.setFont(textFont(LABEL_FONT_HEIGHT));
    rangeLabel.setJustificationType(juce::Justification::centredLeft);
    rangeLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_MIDDLE_LIGHT));
    addAndMakeVisible(nameLabel);
    addAndMakeVisible(rangeLabel);

    configureSlider(midSlider, midLabel, "Mid");
    configureSlider(sideSlider, sideLabel, "Side");
    midSlider.setDoubleClickReturnValue(true, 0.0);
    sideSlider.setDoubleClickReturnValue(true, 0.0);
    midSlider.setTooltip("Level of the band's mid, what both channels share. Double-click to reset");
    sideSlider.setTooltip("Level of the band's side, what differs between the channels. Off makes the band mono");
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
        "Dragging Mid moves only the Side trim by the same amount. Width and Recover stay independent"
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
    soloButton.setEdgeIndent(0);
    soloButton.setClickingTogglesState(true);
    soloButton.setTooltip(
        "Solo: listen to this band only. Solo several bands to hear them together. Press S over a band to toggle it"
    );
    soloAttachment = std::make_unique<HistoryButtonAttachment>(parameters, soloParameterId(band), soloButton, history);
    addAndMakeVisible(soloButton);

    midSlider.onDragStart = [this] {
        if (history.has_value()) {
            linkedGeneration = history->get().generation();
            linkedHistoryEditing = history->get().isEditing();
            // The slider listener ends before onDragEnd, so this nested frame keeps linked writes in one command.
            // A restore from beginChangeGesture cancels the listener's frame and must not open a replacement here.
            if (linkedHistoryEditing) {
                history->get().beginEdit(midSlider.getTitle());
            }
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
        if (history.has_value() && linkedHistoryEditing && linkedGeneration == history->get().generation()) {
            history->get().endEdit();
        }
        linkedHistoryEditing = false;
    };
    midSlider.onValueChange = [this] {
        if (history.has_value() && (!linkedHistoryEditing || linkedGeneration != history->get().generation())) {
            return;
        }
        if (draggingMid && linkedSideGesture && linkedSideStart > static_cast<double>(BAND_GAIN_MIN_DB)) {
            const double side = std::clamp(
                linkedSideStart + midSlider.getValue() - linkedMidStart,
                static_cast<double>(BAND_GAIN_MIN_DB),
                static_cast<double>(BAND_GAIN_MAX_DB)
            );
            if (history.has_value()) {
                history->get().touch(sideId);
            }
            setSideValue(sideParameter, static_cast<float>(side), true);
        }
    };

    for (auto* slider : {&midSlider, &sideSlider, &panSlider, &widthSlider, &recoverSlider}) {
        slider->setTitle(bandName(band) + " " + slider->getName());
    }
    soloButton.setTitle(bandName(band) + " Solo");
    linkButton.setTitle(bandName(band) + " Link");
}

/// Closes active gestures and detaches the sliders before they are destroyed.
BandStrip::~BandStrip()
{
    for (auto* slider : {&midSlider, &sideSlider, &panSlider, &widthSlider, &recoverSlider}) {
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

/// Paints the rounded background of the strip.
void BandStrip::paint(juce::Graphics& graphics)
{
    graphics.setColour(juce::Colour(palette::GREY_SEMI_DARK));
    graphics.fillRoundedRectangle(getLocalBounds().toFloat(), CORNER_RADIUS);
}

/// Lays out the name, Solo, and Link in the header row, the range below them,
/// a gap that uses the spare height up to 16 pixels, and the Mid, Side, Width, Recover, and Pan sliders at full width,
/// each with its label above it. The slider rows share the remaining height evenly.
void BandStrip::resized()
{
    auto area = getLocalBounds().reduced(MARGIN);
    auto header = area.removeFromTop(HEADER_ROW_HEIGHT);
    linkButton.setBounds(header.removeFromRight(LINK_WIDTH));
    header.removeFromRight(SOLO_GAP);
    soloButton.setBounds(header.removeFromRight(HEADER_ROW_HEIGHT));
    nameLabel.setBounds(header);
    rangeLabel.setBounds(area.removeFromTop(HEADER_ROW_HEIGHT));
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

/// Shows the frequency range of the band under its name.
void BandStrip::setRangeText(const juce::String& text)
{
    rangeLabel.setText(text, juce::dontSendNotification);
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
