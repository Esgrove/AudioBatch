/// Source overload presentation for waveform edges and the spectrogram-only warning strip.
/// Projects independent source flags into equal-size solid caps, painting orange true peaks over yellow samples.

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/OverloadHistory.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/components/ThumbnailComponent.h"
#include "gui/GuiLayout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace audiobatch
{
/// Attaches source overloads without changing playback, gain, or the viewport.
/// The immutable history describes decoded source audio, independently of preview processing.
void ThumbnailComponent::setOverloadHistory(std::shared_ptr<const OverloadHistory> history)
{
    if (overloadHistory != history) {
        overloadHistory = std::move(history);
        repaint();
    }
}

/// Changes only overload presentation, preserving histories, caches, viewport and playback.
void ThumbnailComponent::setOverloadIndicatorSettings(const bool sample, const bool truePeak, const bool lines)
{
    if (showSampleOverloads == sample && showTruePeakOverloads == truePeak && showOverloadLines == lines) {
        return;
    }
    showSampleOverloads = sample;
    showTruePeakOverloads = truePeak;
    showOverloadLines = lines;
    repaint();
}

/// Returns visible overload runs with channel-local tint bounds and waveform-outer-edge cap policy.
/// Each kind retains its own runs. All sample marks precede true peaks so orange wins overlapping caps.
/// Returns nothing while the history and the waveform disagree on the channel count.
std::vector<OverloadMark> ThumbnailComponent::getOverloadMarks() const
{
    const auto bounds = waveformBounds();
    if ((!showSampleOverloads && !showTruePeakOverloads) || !overloadHistory || !overloadHistory->hasOverloads()
        || bounds.isEmpty() || visibleRange.getLength() <= 0.0
        || overloadHistory->channelCount != thumbnail->getNumChannels())
    {
        return {};
    }
    auto marks = showSampleOverloads ? getOverloadMarks(true) : std::vector<OverloadMark>();
    if (showTruePeakOverloads) {
        const auto truePeakMarks = getOverloadMarks(false);
        marks.insert(marks.end(), truePeakMarks.begin(), truePeakMarks.end());
    }
    return marks;
}

/// Projects one enabled kind independently, preserving overlaps with the other kind for paint-order precedence.
std::vector<OverloadMark> ThumbnailComponent::getOverloadMarks(const bool sampleOver) const
{
    const auto bounds = waveformBounds();
    const auto channels = overloadHistory->channelCount;
    const auto kind = sampleOver ? OverloadHistory::sampleOver : OverloadHistory::truePeakOver;
    std::vector<OverloadMark> marks;
    for (int channel = 0; channel < channels; ++channel) {
        auto columns = projectOverloadColumns(*overloadHistory, channel, visibleRange, bounds.getWidth());
        for (auto& flags : columns) {
            flags &= kind;
        }
        // The same integer lane split as AudioThumbnail::drawChannels.
        const int laneTopPixel = bounds.getY() + channel * bounds.getHeight() / channels;
        const int laneBottomPixel = bounds.getY() + (channel + 1) * bounds.getHeight() / channels;
        std::size_t column = 0;
        while (column < columns.size()) {
            const auto columnFlags = columns[column];
            if (columnFlags == 0) {
                ++column;
                continue;
            }
            auto end = column + 1;
            while (end < columns.size() && columns[end] != 0) {
                ++end;
            }
            auto start = static_cast<float>(bounds.getX()) + static_cast<float>(column);
            auto finish = static_cast<float>(bounds.getX()) + static_cast<float>(end);
            if (finish - start < layout::waveform::overloadMinimumWidth) {
                const auto centre = (start + finish) * 0.5f;
                start = centre - layout::waveform::overloadMinimumWidth * 0.5f;
                finish = centre + layout::waveform::overloadMinimumWidth * 0.5f;
            }
            marks.push_back({
                .horizontal = {start, finish},
                .laneTop = static_cast<float>(laneTopPixel),
                .laneBottom = static_cast<float>(laneBottomPixel),
                .sampleOver = sampleOver,
                .topEdge = channel == 0,
                .bottomEdge = channel == channels - 1,
                .showLine = showOverloadLines,
            });
            column = end;
        }
    }
    return marks;
}

/// Draws equal solid yellow sample caps and orange true peak caps only at the waveform's outer edges.
/// Both kinds may tint their own channel, independently of caps and without tinting another channel.
void ThumbnailComponent::drawOverloads(juce::Graphics& graphics) const
{
    const auto marks = getOverloadMarks();
    if (marks.empty()) {
        return;
    }
    const juce::Graphics::ScopedSaveState savedState(graphics);
    graphics.reduceClipRegion(waveformBounds());
    for (const auto& mark : marks) {
        const auto colour = mark.sampleOver ? CustomLookAndFeel::yellow() : CustomLookAndFeel::orange();
        const auto width = mark.horizontal.getLength();
        const auto laneHeight = mark.laneBottom - mark.laneTop;
        const auto height = std::min(layout::waveform::overloadCapHeight, laneHeight / 4.0f);
        if (mark.showLine) {
            graphics.setColour(colour.withAlpha(layout::waveform::overloadTintAlpha));
            graphics.fillRect(juce::Rectangle<float>(mark.horizontal.getStart(), mark.laneTop, width, laneHeight));
        }
        graphics.setColour(colour);
        const auto drawCap = [&](const float top) {
            const juce::Rectangle<float> cap(mark.horizontal.getStart(), top, width, height);
            graphics.fillRect(cap);
        };
        if (mark.topEdge) {
            drawCap(mark.laneTop);
        }
        if (mark.bottomEdge) {
            drawCap(mark.laneBottom - height);
        }
    }
}

/// Marks filtered source overloads in a thin strip, never in frequency rows or mixed-channel lanes.
/// The line preference affects waveform lane tints only, not these compact warning caps.
void ThumbnailComponent::drawSourceWarning(juce::Graphics& graphics, const juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty() || !overloadHistory || !overloadHistory->hasOverloads()
        || (!showSampleOverloads && !showTruePeakOverloads))
    {
        return;
    }
    std::vector<std::uint8_t> mixed(static_cast<std::size_t>(bounds.getWidth()), 0);
    for (int channel = 0; channel < overloadHistory->channelCount; ++channel) {
        const auto columns = projectOverloadColumns(*overloadHistory, channel, visibleRange, bounds.getWidth());
        for (std::size_t index = 0; index < columns.size(); ++index) {
            mixed[index] |= visibleOverloadFlags(columns[index], showSampleOverloads, showTruePeakOverloads);
        }
    }
    const juce::Graphics::ScopedSaveState savedState(graphics);
    graphics.reduceClipRegion(bounds);
    for (std::size_t column = 0; column < mixed.size(); ++column) {
        if (mixed[column] == 0) {
            continue;
        }
        const auto rectangle
            = juce::Rectangle<int>(bounds.getX() + static_cast<int>(column), bounds.getY(), 1, bounds.getHeight());
        const auto truePeakOver = (mixed[column] & OverloadHistory::truePeakOver) != 0;
        graphics.setColour(truePeakOver ? CustomLookAndFeel::orange() : CustomLookAndFeel::yellow());
        graphics.fillRect(rectangle);
    }
}
}  // namespace audiobatch
