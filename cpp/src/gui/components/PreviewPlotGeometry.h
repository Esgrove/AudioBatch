/// Semantic pane geometry shared by preview painting, interactions and tests.
/// Time projection always takes an explicit plot, so adjacent panes share one viewport without sharing pixels.

#pragma once

#include "gui/AudioBatchComponentState.h"
#include "gui/GuiLayout.h"

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace audiobatch
{
/// Fixed frequency captions, independent of source sample rate.
inline constexpr std::array
    PREVIEW_FREQUENCIES {0.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16'000.0, 18'000.0, 20'000.0};

/// Pane-local plots and separate axis gutters in component coordinates.
struct PreviewPlotGeometry {
    /// Source waveform plot, empty in spectrogram-only mode.
    juce::Rectangle<int> waveform;
    /// Source spectrogram plot, empty in waveform-only mode.
    juce::Rectangle<int> spectrogram;
    /// The plot that hosts source loudness, empty while disabled.
    juce::Rectangle<int> loudness;
    /// Dedicated LUFS label gutter.
    juce::Rectangle<int> loudnessAxis;
    /// Dedicated frequency label gutter, opposite LUFS when enabled.
    juce::Rectangle<int> frequencyAxis;
    /// Magnitude dBFS labels for each linear-amplitude waveform channel.
    juce::Rectangle<int> amplitudeAxis;
    /// Source warning strip outside the frequency plot in spectrogram-only mode.
    juce::Rectangle<int> sourceWarning;
};

/// One magnitude dBFS caption at a signed linear position within its native waveform channel lane.
struct PreviewAmplitudeLabel {
    /// Caption bounds in the axis gutter, clamped to stay within the channel lane.
    juce::Rectangle<int> bounds;
    /// Signed linear amplitude the caption names, from -1 at the lane bottom to 1 at its top.
    float amplitude {0.0f};
    /// Magnitude reference in dBFS, restricted to -1 and -6.
    float decibels {-1.0f};
    /// Unclamped vertical position of the amplitude within the waveform, shared by grid lines.
    float vertical {0.0f};
    /// Zero-based channel lane of the caption.
    int channel {0};
};

/// Fits post-display-gain dBFS magnitude references to JUCE's integer, linear-amplitude channel lanes.
/// Both polarities share a level. The -1 dBFS reference takes priority over -6 dBFS in compact lanes.
inline std::vector<PreviewAmplitudeLabel> previewAmplitudeLabels(
    const PreviewPlotGeometry& geometry,
    const int channels
)
{
    std::vector<PreviewAmplitudeLabel> labels;
    if (channels <= 0 || geometry.waveform.isEmpty() || geometry.amplitudeAxis.isEmpty()) {
        return labels;
    }
    constexpr std::array DECIBEL_LEVELS {-1.0f, -6.0f};
    constexpr float AMPLITUDE_DECIBEL_FACTOR {20.0f};
    constexpr float DECIBEL_LOG_BASE {10.0f};
    constexpr std::array POLARITIES {1.0f, -1.0f};
    // Conversion is invariant across channels and repaints, while label occupancy remains pane-local.
    static const auto MAGNITUDES = [=] {
        std::array<float, DECIBEL_LEVELS.size()> magnitudes {};
        for (std::size_t index = 0; index < magnitudes.size(); ++index) {
            magnitudes[index] = std::pow(DECIBEL_LOG_BASE, DECIBEL_LEVELS[index] / AMPLITUDE_DECIBEL_FACTOR);
        }
        return magnitudes;
    }();
    for (int channel = 0; channel < channels; ++channel) {
        const auto top = geometry.waveform.getY() + channel * geometry.waveform.getHeight() / channels;
        const auto bottom = geometry.waveform.getY() + (channel + 1) * geometry.waveform.getHeight() / channels;
        const auto height = bottom - top;
        if (height < layout::waveform::AMPLITUDE_LABEL_HEIGHT) {
            continue;
        }
        const auto addLabel = [&](const float amplitude, const float decibels) {
            const auto vertical = static_cast<float>(top) + (1.0f - amplitude) * static_cast<float>(height) * 0.5f;
            const auto labelTop = std::clamp(
                juce::roundToInt(vertical) - layout::waveform::AMPLITUDE_LABEL_HEIGHT / 2,
                top,
                bottom - layout::waveform::AMPLITUDE_LABEL_HEIGHT
            );
            const auto bounds
                = geometry.amplitudeAxis.withY(labelTop).withHeight(layout::waveform::AMPLITUDE_LABEL_HEIGHT);
            if (std::ranges::any_of(
                    labels,
                    [&](const auto& label) {
                        const auto spacing = label.channel == channel ? layout::waveform::AMPLITUDE_LABEL_SPACING : 0;
                        return label.bounds.expanded(0, spacing).intersects(bounds);
                    }
                ))
            {
                return;
            }
            labels.push_back({
                .bounds = bounds,
                .amplitude = amplitude,
                .decibels = decibels,
                .vertical = vertical,
                .channel = channel,
            });
        };
        for (std::size_t index = 0; index < DECIBEL_LEVELS.size(); ++index) {
            for (const auto polarity : POLARITIES) {
                addLabel(polarity * MAGNITUDES[index], DECIBEL_LEVELS[index]);
            }
        }
    }
    return labels;
}

/// Returns bounded plots without changing the requested mode when the host becomes compact.
inline PreviewPlotGeometry previewPlotGeometry(
    juce::Rectangle<int> bounds,
    const PreviewViewMode mode,
    const bool loudnessEnabled
)
{
    PreviewPlotGeometry result;
    if (bounds.getWidth() <= layout::waveform::contentInset * 2
        || bounds.getHeight() <= layout::waveform::contentInset * 2)
    {
        return result;
    }
    bounds = bounds.reduced(
        std::min(layout::waveform::contentInset, std::max(0, bounds.getWidth() / 2)),
        std::min(layout::waveform::contentInset, std::max(0, bounds.getHeight() / 2))
    );
    if (bounds.isEmpty()) {
        return result;
    }
    switch (mode) {
        case PreviewViewMode::waveform:
            result.waveform = bounds;
            break;
        case PreviewViewMode::spectrogram:
            result.spectrogram = bounds;
            result.sourceWarning = result.spectrogram.removeFromTop(
                std::min(layout::waveform::sourceWarningHeight, result.spectrogram.getHeight())
            );
            break;
        case PreviewViewMode::sideBySide: {
            const auto width = std::max(0, bounds.getWidth() - layout::waveform::paneGap) / 2;
            result.waveform = bounds.removeFromLeft(width);
            bounds.removeFromLeft(std::min(layout::waveform::paneGap, bounds.getWidth()));
            result.spectrogram = bounds;
            break;
        }
        case PreviewViewMode::stacked: {
            const auto height = std::max(0, bounds.getHeight() - layout::waveform::paneGap) / 2;
            result.waveform = bounds.removeFromTop(height);
            bounds.removeFromTop(std::min(layout::waveform::paneGap, bounds.getHeight()));
            result.spectrogram = bounds;
            break;
        }
    }
    // Every pane reserves the same gutters, including the unused LUFS gutter in a paired spectral pane.
    // This keeps both plot edges aligned in stacked mode and equal widths in side-by-side mode.
    const auto reserveAxes = [loudnessEnabled](juce::Rectangle<int>& plot) {
        std::array<juce::Rectangle<int>, 2> axes;
        if (plot.isEmpty()) {
            return axes;
        }
        axes[0] = plot.removeFromLeft(std::min(layout::waveform::AXIS_GUTTER_WIDTH, plot.getWidth()));
        axes[0].removeFromRight(std::min(layout::waveform::AXIS_LABEL_GAP, axes[0].getWidth()));
        if (loudnessEnabled) {
            axes[1] = plot.removeFromRight(std::min(layout::waveform::AXIS_GUTTER_WIDTH, plot.getWidth()));
            axes[1].removeFromLeft(std::min(layout::waveform::AXIS_LABEL_GAP, axes[1].getWidth()));
        }
        return axes;
    };
    const auto waveformAxes = reserveAxes(result.waveform);
    const auto spectralAxes = reserveAxes(result.spectrogram);
    result.amplitudeAxis = waveformAxes[0];
    result.frequencyAxis = spectralAxes[0];
    if (loudnessEnabled) {
        result.loudnessAxis = mode == PreviewViewMode::spectrogram ? spectralAxes[1] : waveformAxes[1];
        result.loudness = mode == PreviewViewMode::spectrogram ? result.spectrogram : result.waveform;
    }
    if (!result.sourceWarning.isEmpty()) {
        result.sourceWarning.setLeft(std::min(result.spectrogram.getX(), result.sourceWarning.getRight()));
        result.sourceWarning.setRight(std::max(result.sourceWarning.getX(), result.spectrogram.getRight()));
    }
    return result;
}

/// Projects seconds into the supplied pane without clamping cues or history outside the viewport.
inline float previewTimeToX(const double seconds, const juce::Range<double> range, const juce::Rectangle<int> plot)
{
    if (plot.isEmpty() || range.getLength() <= 0.0) {
        return static_cast<float>(plot.getX());
    }
    return static_cast<float>(plot.getX())
        + static_cast<float>((seconds - range.getStart()) / range.getLength()) * static_cast<float>(plot.getWidth());
}

/// Projects a pointer within its latched plot, clamping crossing drags to the pane's time endpoints.
inline double previewXToTime(const float horizontal, const juce::Range<double> range, const juce::Rectangle<int> plot)
{
    if (plot.isEmpty() || range.getLength() <= 0.0) {
        return range.getStart();
    }
    const auto fraction
        = std::clamp((horizontal - static_cast<float>(plot.getX())) / static_cast<float>(plot.getWidth()), 0.0f, 1.0f);
    return range.getStart() + static_cast<double>(fraction) * range.getLength();
}
}  // namespace audiobatch
