/// Declares BandStrip, the controls of one band in the ImageControl editor:
/// the Mid and Side gain sliders attached to their parameters,
/// the Width macro that sets Side relative to Mid, the Solo and Link toggles, and the parameter-attached Pan slider.
/// BandCorrelationMeter presents each band's live correlation, smoothed trend, and recent observed range.

#pragma once

#include "Parameters.h"
#include "ui/ControlSlider.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>

namespace esgrove::image_control
{
/// Highest directly editable Width, where side is twice as loud relative to mid as in the input.
/// Actual gain ratios above this remain visible in the numerical readout.
constexpr double MAX_STEREO_WIDTH {2.0};
/// Smallest height of a band strip that fits its margins, its name and range rows, and its four slider rows, in pixels.
constexpr int BAND_STRIP_MIN_HEIGHT {236};

/// Height of the independent band correlation row, in pixels.
constexpr int BAND_CORRELATION_HEIGHT {48};

/// Observed extrema within one display-history bucket.
struct CorrelationHistoryBucket final {
    /// Start of the bucket on the meter's relative clock, in seconds.
    double startSeconds {0.0};
    /// Lowest live reading observed in this bucket.
    float minimum {0.0F};
    /// Highest live reading observed in this bucket.
    float maximum {0.0F};
};

/// A read-only horizontal correlation meter with live, smoothed, and recent-range indicators.
/// Message-thread history is bounded, never serialized, and clears when measurements are unavailable.
class BandCorrelationMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit BandCorrelationMeter(const juce::String& band);

    [[nodiscard]] juce::String readoutText() const;
    [[nodiscard]] juce::String valueText() const;
    [[nodiscard]] std::optional<float> trendValue() const;
    [[nodiscard]] std::optional<juce::Range<float>> recentRange() const;
    void clearHistory();
    void setCorrelation(
        float value,
        double elapsedSeconds = 0.0,
        double persistenceSeconds = DEFAULT_PERSISTENCE_SECONDS
    );

    [[nodiscard]] std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    void paint(juce::Graphics& graphics) override;

private:
    void advanceHistory(float value, double elapsedSeconds, double persistenceSeconds);

    /// Bucket resolution bounds late expiry of an observed extreme to at most 1/120 second.
    static constexpr double HISTORY_BUCKET_SECONDS {1.0 / 120.0};
    /// One extra bucket retains the partial bucket at the maximum trailing-window boundary.
    static constexpr std::size_t HISTORY_CAPACITY {
        static_cast<std::size_t>(MAX_PERSISTENCE_SECONDS / HISTORY_BUCKET_SECONDS) + 2,
    };

    /// Observations covering maximum Persistence, regardless of the currently selected window.
    std::array<CorrelationHistoryBucket, HISTORY_CAPACITY> history {};
    /// Last finite correlation, or empty for silence and other unavailable measurements.
    std::optional<float> correlation;
    /// Display-only exponential average, initialized directly from the first valid measurement.
    std::optional<float> trend;
    /// Extrema of display-observed readings within the selected trailing window.
    std::optional<juce::Range<float>> range;
    /// Relative elapsed time since history was last cleared, in seconds.
    double historySeconds {0.0};
    /// Index of the oldest occupied history bucket.
    std::size_t historyBegin {0};
    /// Number of occupied history buckets.
    std::size_t historyCount {0};
};

/// The controls of one band.
///
/// Width is the side gain relative to the mid gain as a linear ratio:
/// 0 is mono, 1 leaves the stereo image as it is, and 2 doubles the side relative to the mid.
/// Moving Width sets Side to Mid plus 20 · log10(width) dB, and width 0 turns Side off.
/// With Link on, dragging Mid moves Side by the same amount, keeping the width.
/// Pan moves the band toward the left or right channel after its mid and side gains.
/// Solo is a parameter that mutes the bands that are not soloed, unless no band or every band is soloed.
/// Width and Link are editor controls, not host parameters, so automation never has conflicting values.
class BandStrip final : public juce::Component
{
public:
    BandStrip(juce::AudioProcessorValueTreeState& parameters, juce::ValueTree& editorState, std::size_t band);

    ~BandStrip() override;

    BandStrip(const BandStrip&) = delete;
    BandStrip(BandStrip&&) = delete;
    BandStrip& operator=(const BandStrip&) = delete;
    BandStrip& operator=(BandStrip&&) = delete;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void setRangeText(const juce::String& text);

private:
    void applyWidth();
    void configureSlider(ControlSlider& slider, juce::Label& label, const juce::String& text);
    void updateWidthFromGains();

    /// The band name.
    juce::Label nameLabel;
    /// The band's frequency range.
    juce::Label rangeLabel;
    /// Label above the Mid slider.
    juce::Label midLabel;
    /// Label above the Side slider.
    juce::Label sideLabel;
    /// Label above the Pan slider.
    juce::Label panLabel;
    /// Label above the Width slider.
    juce::Label widthLabel;

    /// The band's mid gain in dB.
    ControlSlider midSlider;
    /// The band's side gain in dB.
    ControlSlider sideSlider;
    /// The band's pan, from fully left to fully right.
    ControlSlider panSlider;
    /// The Width macro, which writes the side gain.
    ControlSlider widthSlider;
    /// Solos the band: toggles its solo parameter, shown as headphones.
    juce::DrawableButton soloButton {"Solo", juce::DrawableButton::ImageFitted};
    /// Keeps Side moving with Mid while Mid is dragged.
    juce::ToggleButton linkButton {"Link"};

    /// Binds the Mid slider to its parameter.
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> midAttachment;
    /// Binds the Pan slider to its parameter.
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttachment;
    /// Binds the Side slider to its parameter.
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sideAttachment;
    /// Binds the Solo button to its parameter.
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> soloAttachment;

    /// Side gain parameter, owned by the parameter tree, which the Width and Link gestures change.
    juce::RangedAudioParameter& sideParameter;

    /// Mid gain in dB when the current Mid drag started, for Link.
    double linkedMidStart {0.0};
    /// Side gain in dB when the current Mid drag started, for Link.
    double linkedSideStart {0.0};
    /// Whether the user is dragging Mid, so only their own drags move Side through Link, not automation.
    bool draggingMid {false};
    /// Whether the current Mid gesture opened a matching Side gesture, captured before Link can change.
    bool linkedSideGesture {false};
    /// Whether a Width gesture is open, so its changes join that gesture instead of opening their own.
    bool draggingWidth {false};
};
}  // namespace esgrove::image_control
