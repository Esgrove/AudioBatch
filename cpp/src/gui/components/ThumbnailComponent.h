/// Waveform and source spectrogram preview for the currently loaded audio file.
/// Declares ThumbnailComponent, a single JUCE host with full, side-by-side and stacked plots that share one viewport,
/// draws the native thumbnail and a retained bounded source-power raster,
/// shows the playback position of the shared transport source and scrubs it,
/// overlays optional cached short-term loudness with an adjustable LUFS-S axis,
/// marks source sample overloads in yellow and true peak overloads in orange,
/// supports zoom and gain mouse-wheel gestures, Serato hot cue jumps with the number keys, file drag-and-drop,
/// and caching of the rendered waveform data.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/SpectrogramData.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/components/PreviewPlotGeometry.h"
#include "serato/SeratoTagTypes.h"
#include "serato/SeratoWaveformColouring.h"
#include "utils/FileUtils.h"

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace audiobatch
{
class AudioPreviewPlayer;

/// Applies a display-only smoothstep to quantized source power without changing stored intensities.
/// Quiet values become darker and strong values brighter while both palette endpoints remain fixed.
[[nodiscard]] constexpr float spectrogramDisplayIntensity(const std::uint8_t intensity) noexcept
{
    constexpr float SMOOTHSTEP_QUADRATIC_COEFFICIENT {3.0f};
    const auto normalized = static_cast<float>(intensity) / 255.0f;
    return normalized * normalized * (SMOOTHSTEP_QUADRATIC_COEFFICIENT - 2.0f * normalized);
}

/// Placement of the two arrows that mark one Serato cue on the waveform, in component coordinates.
/// One arrow hangs from the top edge of the waveform and one rises from its bottom edge, both pointing inwards.
struct CueArrows {
    // Preserve the geometry aggregate contract used by rendering and cue-position tests.
    /// Opaque fill colour of the cue.
    juce::Colour colour;
    /// Horizontal position of the cue, where the tips of both arrows lie.
    float horizontalPosition {0.0f};
    /// Half the width of each arrow base along the waveform edge.
    float halfWidth {0.0f};
    /// Distance from the waveform edge to the arrow tip.
    float height {0.0f};
    /// Top edge of the waveform, where the base of the upper arrow lies.
    float top {0.0f};
    /// Bottom edge of the waveform, where the base of the lower arrow lies.
    float bottom {0.0f};
};

/// The coloured waveform last painted, with the geometry and gain it was rendered for.
/// The waveform paints it again without rendering while none of them, the waveform, or the overview changed.
struct ColouredWaveformCache {
    // Preserve designated construction order alongside the cached render's geometry and gain.
    /// The rendered waveform, which also records the pixel scale and renderer it was rendered for.
    serato::ColouredThumbnailImage rendered;
    /// Waveform bounds in component coordinates.
    juce::Rectangle<int> bounds;
    /// Visible time range in seconds.
    juce::Range<double> visibleRange;
    /// Linear display gain of the waveform.
    float displayGain {1.0f};
};

/// One horizontal run of an overload mark in a channel lane, in component coordinates.
struct OverloadMark {
    // Geometry follows the horizontal run and its lane bounds, matching aggregate rendering requests.
    /// Horizontal extent of the run, at least the minimum mark width.
    juce::Range<float> horizontal;
    /// Top of the channel lane the run marks.
    float laneTop {0.0f};
    /// Bottom of the channel lane the run marks.
    float laneBottom {0.0f};
    /// True for a yellow sample cap, false for an equally sized solid orange true peak cap.
    bool sampleOver {false};
    /// Caps touch only the waveform's outer top edge, never the stereo divider.
    bool topEdge {false};
    /// Caps touch only the waveform's outer bottom edge, never the stereo divider.
    bool bottomEdge {false};
    /// Optional lane tint is independent of the overload kind and cap visibility.
    bool showLine {false};
};

/// Removes disabled raw overload kinds while preserving both independently visible source signals.
[[nodiscard]] constexpr std::uint8_t visibleOverloadFlags(
    const std::uint8_t flags,
    const bool sample,
    const bool truePeak
) noexcept
{
    const auto mask = (sample ? static_cast<unsigned int>(OverloadHistory::sampleOver) : 0u)
        | (truePeak ? static_cast<unsigned int>(OverloadHistory::truePeakOver) : 0u);
    return static_cast<std::uint8_t>(static_cast<unsigned int>(flags) & mask);
}

/// Waveform preview component with a playback cursor, scrubbing, zooming, and drag-and-drop.
class ThumbnailComponent :
    public juce::Component,
    public juce::ChangeListener,
    public juce::FileDragAndDropTarget,
    public juce::ChangeBroadcaster,
    public juce::TooltipClient
{
public:
    ThumbnailComponent(juce::AudioFormatManager& formatManager, AudioPreviewPlayer& source);

    ~ThumbnailComponent() override;

    ThumbnailComponent(const ThumbnailComponent&) = delete;
    ThumbnailComponent& operator=(const ThumbnailComponent&) = delete;
    ThumbnailComponent(ThumbnailComponent&&) = delete;
    ThumbnailComponent& operator=(ThumbnailComponent&&) = delete;

    juce::URL getLastDroppedFile() const noexcept;
    juce::StringArray getLastDroppedFiles() const;
    juce::MemoryBlock saveToCacheData() const;

    [[nodiscard]] PreviewPlotGeometry getPlotGeometry() const;
    [[nodiscard]] PreviewViewMode getViewMode() const noexcept;
    [[nodiscard]] std::optional<double> getPendingSeekPosition() const noexcept;
    [[nodiscard]] bool hasSpectrogramRaster() const noexcept;
    void setViewMode(PreviewViewMode mode);
    void setViewModeChangedCallback(std::function<void(PreviewViewMode)> callback);
    void setSpectrogramData(std::shared_ptr<const SpectrogramData> data);

    [[nodiscard]] std::vector<CueArrows> getCueArrows() const;
    [[nodiscard]] std::vector<CueArrows> getCueArrows(juce::Rectangle<int> bounds) const;
    [[nodiscard]] std::vector<OverloadMark> getOverloadMarks() const;

    [[nodiscard]] const std::optional<utils::FileIdentity>& getSourceIdentity() const noexcept;

    [[nodiscard]] bool adoptWaveformData(const juce::MemoryBlock& waveformData, const utils::FileIdentity& identity);
    [[nodiscard]] bool isSeratoWaveformColouringEnabled() const noexcept;
    bool loadFromCacheData(
        const juce::MemoryBlock& waveformData,
        std::optional<utils::FileIdentity> identity = std::nullopt
    );

    void setDisplayGain(float linearGain);
    void setLoudnessGraphSettings(bool enabled, double minimum, double maximum);
    void setLoudnessHistory(std::shared_ptr<const ShortTermLoudnessHistory> history);
    void setMouseWheelGainCallback(std::function<void(float)> callback);
    void setMouseWheelZoomCallback(std::function<void(double)> callback);
    void setOverloadHistory(std::shared_ptr<const OverloadHistory> history);
    void setOverloadIndicatorSettings(bool sample, bool truePeak, bool lines);
    void setPendingSource(juce::int64 lengthInSamples, double durationSeconds, const utils::FileIdentity& identity);
    void setRange(juce::Range<double> newRange);

    // Serato overlays and source replacement.
    void setSeratoMarkers(std::shared_ptr<const serato::Markers> markers);
    void setSeratoOverview(std::shared_ptr<const serato::Overview> overview);
    void setSeratoWaveformColouringEnabled(bool enabled);
    void setThumbnailFullyLoadedCallback(std::function<void()> callback);
    void setURL(
        const juce::URL& url,
        juce::int64 lengthInSamples = 0,
        std::optional<utils::FileIdentity> identity = std::nullopt
    );
    void setZoom(double zoomLevel);

    // juce::Component
    bool keyPressed(const juce::KeyPress& key) override;

    void focusGained(FocusChangeType cause) override;
    void focusLost(FocusChangeType cause) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void paint(juce::Graphics& graphics) override;
    void resized() override;

    // juce::ChangeListener
    void changeListenerCallback(ChangeBroadcaster* source) override;

    // juce::FileDragAndDropTarget
    bool isInterestedInFileDrag(const juce::StringArray& /*files*/) override;

    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;

    // juce::TooltipClient
    juce::String getTooltip() override;

private:
    [[nodiscard]] juce::Rectangle<int> waveformBounds() const;
    [[nodiscard]] std::vector<OverloadMark> getOverloadMarks(bool sampleOver) const;

    bool drawColouredWaveform(juce::Graphics& graphics, juce::Rectangle<int> bounds);
    [[nodiscard]] double sourceDuration() const;
    [[nodiscard]] float loudnessToY(double value, juce::Rectangle<int> bounds) const;

    void drawCueMarkers(juce::Graphics& graphics, juce::Rectangle<int> bounds) const;
    void drawSpectrogram(juce::Graphics& graphics, juce::Rectangle<int> bounds) const;
    void drawFrequencyAxis(juce::Graphics& graphics, const PreviewPlotGeometry& geometry) const;
    void drawWaveformAxis(juce::Graphics& graphics, const PreviewPlotGeometry& geometry) const;
    void drawSourceWarning(juce::Graphics& graphics, juce::Rectangle<int> bounds) const;
    void drawPlayheads(juce::Graphics& graphics) const;
    void drawPositionMarker(juce::Graphics& graphics, double time, juce::Colour colour) const;
    void showViewModeMenu();
    void drawLoudnessAxis(juce::Graphics& graphics, juce::Rectangle<int> bounds) const;
    void drawLoudnessCurve(juce::Graphics& graphics, juce::Rectangle<int> bounds) const;
    void drawLoudnessGraph(juce::Graphics& graphics) const;
    void drawOverloads(juce::Graphics& graphics) const;

    // Source and playback interaction.
    void jumpToHotCue(int slot);
    void replaceThumbnail(int samplesPerPoint);
    void updateCursorPosition();
    void updatePlaybackPosition();

    /// Preview player, which outlives this component.
    AudioPreviewPlayer& transportSource;
    /// Borrowed formats, outliving the thumbnail and this component.
    juce::AudioFormatManager& thumbnailFormats;
    /// Must outlive the thumbnails referring into it.
    juce::AudioThumbnailCache thumbnailCache {128};

    /// Retained source-only raster, rebuilt only when the immutable artifact changes.
    juce::Image spectrogramRaster;
    /// Pane chosen at mouse-down, retained when scrubbing crosses a split.
    juce::Rectangle<int> scrubBounds;
    /// Visible time range latched at mouse-down, so zooming or scrolling during a drag cannot move the target.
    juce::Range<double> scrubRange;
    /// Visible file time range in seconds.
    juce::Range<double> visibleRange;
    /// Visualization preference, retained across source changes and compact layouts.
    PreviewViewMode viewMode {PreviewViewMode::waveform};
    /// Requests owner-side preference publication rather than persisting from the preview.
    std::function<void(PreviewViewMode)> viewModeChangedCallback;
    /// Null before a request completes, including while pending.
    std::shared_ptr<const SpectrogramData> spectrogramData;

    /// Display-only release target, independent of the transport's continuously advancing position.
    std::optional<double> pendingSeekTime;

    /// Last painted actual and pending marker bounds, for bounded refresh invalidation.
    std::vector<juce::Rectangle<int>> playheadBounds;
    /// Playback position captured once per display refresh for every pane.
    double playheadTime {0.0};
    /// A popup gesture never becomes a transport gesture on mouse-up.
    bool popupGesture {false};
    /// True only for a non-popup scrub started inside a plot.
    bool scrubbing {false};
    /// Distinguishes an unavailable result from an artifact still computing.
    bool spectrogramCompleted {false};

    /// Source metadata remains available before native peaks arrive.
    double pendingDuration {0.0};
    /// Source sample rate inferred from explicit metadata or validated native peaks.
    double pendingSampleRate {0.0};
    /// Exact source length from pending metadata or validated native peaks.
    juce::int64 sourceLengthInSamples {0};
    /// Native source channels, zero until peaks establish the actual count.
    int sourceChannels {0};

    /// Files last dropped onto the waveform.
    juce::StringArray lastDroppedFiles;
    /// First file last dropped onto the waveform.
    juce::URL lastFileDropped;

    /// Called once after reading the complete waveform.
    std::function<void()> thumbnailFullyLoadedCallback;
    /// Mouse-wheel zoom factor.
    std::function<void(double)> mouseWheelZoomCallback;
    /// Mouse-wheel gain change in decibels.
    std::function<void(float)> mouseWheelGainCallback;

    /// Last rendered coloured waveform.
    std::optional<ColouredWaveformCache> colouredWaveform;
    /// Source identity captured before decoding, never resampled during publication.
    std::optional<utils::FileIdentity> sourceIdentity;

    /// Analysis histories and Serato tags, null until available.
    std::shared_ptr<const ShortTermLoudnessHistory> loudnessHistory;
    std::shared_ptr<const OverloadHistory> overloadHistory;
    std::shared_ptr<const serato::Markers> seratoMarkers;
    std::shared_ptr<const serato::Overview> seratoOverview;

    /// Recreated for each file, after the cache and before its destruction.
    std::unique_ptr<juce::AudioThumbnail> thumbnail;

    /// Whether the current file's completion callback ran.
    bool hasNotifiedFullyLoaded {false};
    bool loudnessGraphEnabled {false};
    bool seratoWaveformColouringEnabled {true};
    /// Optional lane tints for either overload kind, retained across source changes.
    bool showOverloadLines {true};
    /// Display-only sample cap visibility, independent of retained source histories.
    bool showSampleOverloads {true};
    /// Display-only true peak cap visibility, independent of retained source histories.
    bool showTruePeakOverloads {true};
    /// Loudness axis limits in LUFS.
    double loudnessMaximum {AudioPreviewPreferences::defaultLoudnessGraphMaximum};
    double loudnessMinimum {AudioPreviewPreferences::defaultLoudnessGraphMinimum};
    /// Linear display gain reflecting the custom gain.
    float displayGain {1.0f};

    /// Kept last so display callbacks detach before any observed state is destroyed.
    juce::VBlankAttachment positionAttachment;
};
}  // namespace audiobatch
