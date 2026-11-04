/// Selected-file inspector beside the waveform preview.
/// Keeps the filename, optionally split into artist and title, and four analysis metrics above scrolling file details.
/// Technical and musical values share a two-row,
/// five-column grid, followed by genre and wrapped album and comment tags.
/// Analysis errors remain available in the details, without lifecycle or cache-provenance badges.
/// The wide layout puts the metrics in one row while keeping the same stacked details.

#pragma once

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "serato/SeratoTagTypes.h"
#include "utils/BackgroundRunner.h"
#include "utils/FileUtils.h"

#include <JuceHeader.h>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace audiobatch
{
/// Property grid dimensions shared by presentation and component storage.
constexpr std::size_t AUDIO_INFO_COLUMN_COUNT {5};
constexpr std::size_t AUDIO_INFO_ROW_COUNT {2};
/// Scrollable source statistics and optional tag lines.
constexpr std::size_t AUDIO_INFO_DETAIL_LINE_COUNT {7};
constexpr std::size_t AUDIO_INFO_CELL_COUNT {AUDIO_INFO_COLUMN_COUNT * AUDIO_INFO_ROW_COUNT};

/// One value in a technical or musical line, with a label retained for its tooltip.
struct AudioInfoValue {
    /// Name of the value, such as "Format" or "Duration".
    juce::String label;
    /// Display text, including any unit.
    juce::String value;
};

/// Pure selected-file details, independent of component state and available width.
struct AudioInfoDetails {
    /// File type, channels, duration, and size, available only for ready records.
    std::vector<AudioInfoValue> format;
    /// Bit depth, sample rate, and bitrate, available only for ready records.
    std::vector<AudioInfoValue> audio;
    /// Tempo, original key with its Camelot equivalent, and year, independent of analysis readiness.
    std::vector<AudioInfoValue> track;
    /// Whole-file source ratios, never gain-adjusted estimates.
    juce::String dynamics;
    /// Signed source channel means.
    juce::String dcOffsets;
    /// Source clipped sample counts by channel.
    juce::String clips;
    /// Whole-file centered source correlation.
    juce::String correlation;
    /// Single-line genre tag.
    juce::String genre;
    /// Fully wrapped album tag.
    juce::String album;
    /// Fully wrapped comment tag.
    juce::String comment;
};

// The optional Serato data is borrowed for this call only and may be null before metadata arrives.
AudioInfoDetails formatInfoDetails(const AudioAnalysisRecord& record, const serato::SeratoData* data = nullptr);
std::array<AudioInfoValue, AUDIO_INFO_CELL_COUNT> formatInfoGrid(const AudioInfoDetails& details);

/// Read-only text rendered and measured with the same layout, including long-token wrapping.
/// Inherits label text and accessibility so the actual visible content remains discoverable.
class AudioInfoText final : public juce::Label
{
public:
    AudioInfoText(const juce::String& name, bool shouldWrap);

    [[nodiscard]] int getPreferredHeight(int width) const;
    void setValues(const std::vector<AudioInfoValue>& values);

protected:
    // juce::Component
    void paint(juce::Graphics& graphics) override;

private:
    [[nodiscard]] juce::TextLayout createTextLayout(int width) const;

    /// True for unlimited album, comment, and error wrapping.
    bool wrapping {false};
};

/// A labelled analysis value with an explicit unit and a restrained card background.
class AudioInfoMetric final : public juce::Component
{
public:
    AudioInfoMetric(
        const juce::String& name,
        const juce::String& caption,
        const juce::String& unit,
        const juce::String& compactCaption = {}
    );

    void setCompact(bool compact);
    void setValue(
        const juce::String& value,
        const juce::String& adjustedValue = {},
        bool clippedLoudness = false,
        bool limiterEstimate = false
    );

    // juce::Component
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void layoutValues(juce::Rectangle<int> bounds);

    /// Smaller custom-gain prediction beside the source value.
    juce::Label adjustedValueLabel;
    /// Name of the value, such as "Peak".
    juce::Label captionLabel;
    /// Unit of the value, such as "dBFS".
    juce::Label unitLabel;
    /// The formatted value.
    juce::Label valueLabel;
    /// Distinct unit-only caption used when the full metric name does not fit.
    juce::String compactUnit;
    /// Measurement unit retained in expanded captions and accessible descriptions.
    juce::String measurementUnit;
    /// True in the compact row, which stacks the caption over the unit beside the value.
    bool compactMode {false};
};

/// Read-only inspector with a fixed summary and a vertically scrolling details region.
class AudioInfoPanel final : public juce::Component, private juce::Timer
{
public:
    AudioInfoPanel();

    ~AudioInfoPanel() override;

    AudioInfoPanel(const AudioInfoPanel&) = delete;
    AudioInfoPanel(AudioInfoPanel&&) = delete;
    AudioInfoPanel& operator=(const AudioInfoPanel&) = delete;
    AudioInfoPanel& operator=(AudioInfoPanel&&) = delete;

    [[nodiscard]] bool hasArtwork() const noexcept;
    [[nodiscard]] int getPreferredHeight() const;

    void clear();
    void setArtwork(const juce::Image& image);
    void setCompact(bool compact);
    void setWide(bool wide);
    void showRecord(
        const AudioAnalysisRecord& record,
        const juce::String& activeStatusLabel,
        const serato::SeratoData* data = nullptr,
        bool showGainAdjustedMetrics = true
    );

    // juce::Component
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    [[nodiscard]] int getDetailsHeight(int width) const;
    [[nodiscard]] int getErrorHeight(int width) const;
    [[nodiscard]] int getHeaderHeight() const;

    void timerCallback() override;

    void updateLoudnessPreview(const AudioAnalysisRecord& record, bool enabled);
    void showLoudnessPreview();
    void setDetails(const AudioInfoDetails& details);

    /// Details parent outlives the viewport and its text components.
    juce::Component detailsContent;
    /// Embedded album artwork beside the file name.
    juce::ImageComponent artworkImage {"Album artwork"};

    /// Error of a failed analysis, empty otherwise.
    AudioInfoText errorLabel {"Error", true};
    /// File name, or artist when split into artist and title.
    juce::Label filenameLabel {"File"};
    /// Title when the file name was split.
    juce::Label titleLabel {"Title"};

    /// Path used to retain scroll position across updates.
    juce::String displayedPath;
    /// Borrows the details content without taking ownership.
    juce::Viewport detailsViewport;

    /// Cards retain display order: peak, true peak, short-term and integrated loudness.
    std::array<AudioInfoMetric, 4> metrics {
        AudioInfoMetric {"Peak", "Peak", "dBFS"},
        AudioInfoMetric {"True peak", "True peak", "dBTP"},
        AudioInfoMetric {"Max short-term", "Short-term", "LUFS", "LUFS-S"},
        AudioInfoMetric {"Integrated loudness", "Integrated", "LUFS", "LUFS-I"},
    };
    /// Two shared five-column rows, retaining empty slots when a property is unavailable.
    std::array<juce::Label, AUDIO_INFO_CELL_COUNT> detailCells;
    /// Populated rows, independent of whether the details viewport is currently visible.
    std::array<bool, AUDIO_INFO_ROW_COUNT> detailRowsVisible {false, false};
    /// Captions retain genre, album, and comment display order.
    std::array<juce::Label, AUDIO_INFO_DETAIL_LINE_COUNT> tagCaptions {
        juce::Label {{}, "Source"},
        juce::Label {{}, "DC"},
        juce::Label {{}, "Clips"},
        juce::Label {{}, "Corr"},
        juce::Label {{}, "Genre"},
        juce::Label {{}, "Album"},
        juce::Label {{}, "Comment"},
    };
    /// Genre truncates on one line while album and comment wrap without a line limit.
    std::array<AudioInfoText, AUDIO_INFO_DETAIL_LINE_COUNT> tagValues {
        AudioInfoText {"Source dynamics", true},
        AudioInfoText {"Source DC offset", true},
        AudioInfoText {"Source clip counts", true},
        AudioInfoText {"Source correlation", false},
        AudioInfoText {"Genre", false},
        AudioInfoText {"Album", true},
        AudioInfoText {"Comment", true},
    };

    /// Latest selected request, retained on failure to avoid retrying on every unrelated UI refresh.
    std::optional<utils::FileIdentity> loudnessIdentity;
    std::optional<Decibels> loudnessGain;
    std::optional<GainAdjustedLoudness> adjustedLoudness;
    /// Source text is never replaced by the transient model result.
    std::array<juce::String, 2> sourceLoudness;
    /// Unmodified source loudness used only for the integrated presentation fit.
    double sourceIntegratedLufs {AudioAnalysisRecord::negativeInfinityLoudness};
    /// One cancellable worker, joined in the destructor before any request state is destroyed.
    BackgroundRunner loudnessRunner {1, -1};

    /// True for the compact metric-only layout.
    bool compactMode {false};
    /// Whether the file name was split.
    bool hasTitle {false};
    /// True for the single-row wide metrics layout.
    bool wideMode {false};
};
}  // namespace audiobatch
