/// Declares BandStrip, the controls of one band in the ImageControl editor:
/// the Mid and Side gain sliders attached to their parameters,
/// the independent Width and Recover sliders, the Solo and Link toggles, and the parameter-attached Pan slider.
/// BandCorrelationMeter presents each band's live correlation, smoothed trend, and recent observed range.

#pragma once

#include "Parameters.h"
#include "ui/ControlSlider.h"
#include "ui/HistoryAttachments.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace esgrove::image_control
{
namespace test
{
struct BandCorrelationMeterTestAccess;
}

/// Smallest height of a band strip that fits its margins, its name and range rows, and its five slider rows, in pixels.
constexpr int BAND_STRIP_MIN_HEIGHT {280};

/// Height of the compact editable Mid, Side, Width, and Pan value row with its margins, in pixels.
constexpr int BAND_STRIP_COMPACT_HEIGHT {28};

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
    /// Most recent observation of the minimum, for continuous fading without bucket-boundary steps.
    double minimumSeconds {0.0};
    /// Most recent observation of the maximum, for continuous fading without bucket-boundary steps.
    double maximumSeconds {0.0};
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
    void setSmoothedReadout(bool smoothed);
    void setCorrelation(
        float value,
        double elapsedSeconds = 0.0,
        double persistenceSeconds = DEFAULT_PERSISTENCE_SECONDS
    );

    [[nodiscard]] std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    void paint(juce::Graphics& graphics) override;

private:
    friend struct test::BandCorrelationMeterTestAccess;

    [[nodiscard]] bool updateRangeIntensity(double persistenceSeconds);
    void advanceHistory(float value, double elapsedSeconds, double persistenceSeconds);

    /// Fixed spatial resolution of the soft range field, independent of component width and display refresh rate.
    static constexpr std::size_t RANGE_INTENSITY_SAMPLES {129};
    /// Bucket resolution bounds late expiry of an observed extreme to at most 1/120 second.
    static constexpr double HISTORY_BUCKET_SECONDS {1.0 / 120.0};
    /// One extra bucket retains the partial bucket at the maximum trailing-window boundary.
    static constexpr std::size_t HISTORY_CAPACITY {
        static_cast<std::size_t>(MAX_PERSISTENCE_SECONDS / HISTORY_BUCKET_SECONDS) + 2,
    };

    /// Observations covering maximum Persistence, regardless of the currently selected window.
    std::array<CorrelationHistoryBucket, HISTORY_CAPACITY> history {};
    /// Soft spatial coverage of recent observations, fading continuously to zero within Persistence.
    std::array<float, RANGE_INTENSITY_SAMPLES> rangeIntensity {};
    /// Last finite correlation, or empty for silence and other unavailable measurements.
    std::optional<float> correlation;
    /// Display-only exponential average, initialized directly from the first valid measurement.
    std::optional<float> trend;
    /// Extrema of display-observed readings within the selected trailing window.
    std::optional<juce::Range<float>> range;
    /// Whether visual and accessible numbers follow the trend instead of the live marker.
    bool smoothedReadout {true};
    /// Relative elapsed time since history was last cleared, in seconds.
    double historySeconds {0.0};
    /// Index of the oldest occupied history bucket.
    std::size_t historyBegin {0};
    /// Number of occupied history buckets.
    std::size_t historyCount {0};
};

/// The controls of one band.
///
/// Mid and Side trim the source first. Width independently scales the trimmed Side from mono at 0 to double at 2.
/// Recover adds a quadrature-rotated share of removed Side amplitude to Mid below Width 1, even with source Mid off.
/// With Link on, dragging Mid moves only the Side trim by the same amount.
/// Pan moves the band toward the left or right channel after Width and Recover.
/// Solo mutes the bands that are not soloed, unless no band or every band is soloed.
/// All processing controls attach to host parameters. Link is an editor-only setting.
class BandStrip final : public juce::Component
{
public:
    BandStrip(
        juce::AudioProcessorValueTreeState& parameters,
        juce::ValueTree& editorState,
        std::size_t band,
        ParameterHistory& history
    );

    ~BandStrip() override;

    BandStrip(const BandStrip&) = delete;
    BandStrip(BandStrip&&) = delete;
    BandStrip& operator=(const BandStrip&) = delete;
    BandStrip& operator=(BandStrip&&) = delete;

    void discardPendingEdits();
    void setHeaderDisclosureInset(int inset);
    void setCompact(bool compact);
    void setRangeText(const juce::String& text);

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void configureSlider(ControlSlider& slider, juce::Label& label, const juce::String& text);

    /// Label above the Mid slider.
    juce::Label midLabel;
    /// The band name.
    juce::Label nameLabel;
    /// Label above the Pan slider.
    juce::Label panLabel;
    /// The band's frequency range.
    juce::Label rangeLabel;
    /// Label above the Recover slider.
    juce::Label recoverLabel;

    /// Label above the Side slider.
    juce::Label sideLabel;
    /// Label above the Width slider.
    juce::Label widthLabel;

    /// The band's mid gain in dB.
    ControlSlider midSlider;
    /// The band's pan, from fully left to fully right.
    ControlSlider panSlider;
    /// Percentage of removed Side amplitude added to Mid through the quadrature path while narrowing.
    ControlSlider recoverSlider;
    /// The band's side gain in dB.
    ControlSlider sideSlider;
    /// Independent side multiplier after the source trims.
    ControlSlider widthSlider;

    /// Solos the band: toggles its solo parameter, shown as headphones.
    juce::DrawableButton soloButton {"Solo", juce::DrawableButton::ImageFitted};
    /// Keeps Side moving with Mid while Mid is dragged.
    juce::ToggleButton linkButton {"Link"};

    /// Binds the Mid slider to its parameter.
    std::unique_ptr<HistorySliderAttachment> midAttachment;
    /// Binds the Pan slider to its parameter.
    std::unique_ptr<HistorySliderAttachment> panAttachment;
    /// Binds Recover directly to its percentage parameter.
    std::unique_ptr<HistorySliderAttachment> recoverAttachment;
    /// Binds the Side slider to its parameter.
    std::unique_ptr<HistorySliderAttachment> sideAttachment;
    /// Binds Width directly to its independent parameter.
    std::unique_ptr<HistorySliderAttachment> widthAttachment;
    /// Binds the Solo button to its parameter.
    std::unique_ptr<HistoryButtonAttachment> soloAttachment;

    /// Side gain parameter, owned by the parameter tree, which linked Mid gestures change.
    juce::RangedAudioParameter& sideParameter;

    /// Processor-owned local history, which outlives this strip.
    ParameterHistory& history;
    /// Stable Side identity for capture before a linked write.
    juce::String sideId;
    /// Host-state generation when the Mid gesture started.
    std::uint64_t linkedGeneration {0};

    /// Mid gain in dB when the current Mid drag started, for Link.
    double linkedMidStart {0.0};
    /// Side gain in dB when the current Mid drag started, for Link.
    double linkedSideStart {0.0};
    /// Whether the user is dragging Mid, so only their own drags move Side through Link, not automation.
    bool draggingMid {false};
    /// Whether Mid owns the extra frame that keeps linked Side edits in the same local command.
    bool linkedHistoryEditing {false};
    /// Whether the current Mid gesture opened a matching Side gesture, captured before Link can change.
    bool linkedSideGesture {false};
    /// Whether only editable Mid, Side, Width, and Pan values with inline identities are visible.
    bool compact {false};
    /// Space in the final band's full heading reserved for the editor's controls disclosure.
    int headerDisclosureInset {0};
};
}  // namespace esgrove::image_control
