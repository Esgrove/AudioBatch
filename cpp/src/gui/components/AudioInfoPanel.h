/// Selected-file inspector beside the waveform preview.
/// Keeps the filename, optionally split into artist and title, and four analysis metrics above scrolling file details.
/// Pending and failed records also show their status.
/// The wide layout puts the metrics in one row, and the details with short values two to a line,
/// above the tags that can be long, such as the album and the comment, which keep the full width.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "serato/SeratoTagTypes.h"

#include <JuceHeader.h>

#include <array>
#include <memory>
#include <vector>

namespace audiobatch
{
/// One read-only detail with a field caption and value.
struct AudioInfoRow {
    /// Caption of the detail, such as "Format".
    juce::String field;
    /// The value shown next to the caption.
    juce::String value;
    /// True for a value that always stays short, such as the format or the tempo,
    /// which the wide layout shows beside another one on the same line.
    bool isShort {false};
};

/// A labelled analysis value with an explicit unit and a restrained card background.
class AudioInfoMetric final : public juce::Component
{
public:
    AudioInfoMetric(const juce::String& name, const juce::String& caption, const juce::String& unit);

    void setCompact(bool compact);
    void setValue(const juce::String& value);

    // juce::Component
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    /// Name of the value, such as "Peak".
    juce::Label captionLabel;
    /// Unit of the value, such as "dBFS".
    juce::Label unitLabel;
    /// The formatted value.
    juce::Label valueLabel;
    /// True in the compact row, which stacks the caption over the unit beside the value.
    bool compactMode {false};
};

/// Read-only inspector with a fixed summary and a vertically scrolling details region.
class AudioInfoPanel : public juce::Component
{
public:
    AudioInfoPanel();

    static std::vector<AudioInfoRow> formatRows(const AudioAnalysisRecord& record);

    [[nodiscard]] bool hasArtwork() const noexcept;
    [[nodiscard]] int getPreferredHeight() const;

    void clear();
    void setArtwork(const juce::Image& image);
    void setCompact(bool compact);
    void setWide(bool wide);
    void showRecord(
        const AudioAnalysisRecord& record,
        const juce::String& activeStatusLabel,
        const serato::SeratoData* data = nullptr
    );

    // juce::Component
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    [[nodiscard]] int getDetailLineCount() const;
    [[nodiscard]] int getErrorHeight(int width) const;
    [[nodiscard]] int getHeaderHeight() const;

    void ensureLabelCount(std::size_t requiredCount);
    void setRows(std::vector<AudioInfoRow> newRows);

    /// Details parent outlives the viewport and the dynamically owned labels.
    juce::Component detailsContent;
    /// Embedded album artwork beside the file name.
    juce::ImageComponent artworkImage {"Album artwork"};

    /// Error of a failed analysis, empty otherwise.
    juce::Label errorLabel {"Error"};
    /// File name, or artist when split into artist and title.
    juce::Label filenameLabel {"File"};
    /// Status while pending or failed.
    juce::Label statusLabel {"Status"};
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
        AudioInfoMetric {"Max short-term", "Short-term", "LUFS"},
        AudioInfoMetric {"Integrated loudness", "Integrated", "LUFS"},
    };
    /// Detail rows displayed by the labels.
    std::vector<AudioInfoRow> rows;

    /// Caption labels created on demand.
    std::vector<std::unique_ptr<juce::Label>> fieldLabels;
    /// Value labels created on demand.
    std::vector<std::unique_ptr<juce::Label>> valueLabels;

    /// True for the compact metric-only layout.
    bool compactMode {false};
    /// Whether the status label is shown.
    bool hasStatus {false};
    /// Whether the file name was split.
    bool hasTitle {false};
    /// True for the wide metrics and short-detail layout.
    bool wideMode {false};
};
}  // namespace audiobatch
