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

    void setValue(const juce::String& value);
    void setCompact(bool compact);
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

    void clear();
    void setCompact(bool compact);
    void setWide(bool wide);
    void showRecord(
        const AudioAnalysisRecord& record,
        const juce::String& activeStatusLabel,
        const serato::SeratoData* data = nullptr
    );

    [[nodiscard]] int getPreferredHeight() const;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    [[nodiscard]] int getErrorHeight(int width) const;

    [[nodiscard]] int getDetailLineCount() const;

    void ensureLabelCount(std::size_t requiredCount);
    void setRows(std::vector<AudioInfoRow> newRows);

    /// The file name, or the part before " - " when the name holds an artist and a title.
    juce::Label filenameLabel {"File"};
    /// The part of the file name after " - ", shown only for such names.
    juce::Label titleLabel {"Title"};
    /// The status of a file that is not analyzed yet or failed, hidden otherwise.
    juce::Label statusLabel {"Status"};
    /// The cards of the peak, true peak, maximum short-term loudness, and integrated loudness.
    std::array<AudioInfoMetric, 4> metrics {
        AudioInfoMetric {"Peak", "Peak", "dBFS"},
        AudioInfoMetric {"True peak", "True peak", "dBTP"},
        AudioInfoMetric {"Max short-term", "Short-term", "LUFS"},
        AudioInfoMetric {"Integrated loudness", "Integrated", "LUFS"},
    };
    /// The content of the details viewport, with the error and the detail rows.
    juce::Component detailsContent;
    /// The error message of a failed analysis, empty otherwise.
    juce::Label errorLabel {"Error"};
    /// Path of the shown file, so an update of the same file keeps the scroll position of the details.
    juce::String displayedPath;
    /// Scrolls the details below the summary.
    juce::Viewport detailsViewport;
    /// The detail rows that the labels show.
    std::vector<AudioInfoRow> rows;
    /// The caption labels of the detail rows, created as rows are needed and reused afterwards.
    std::vector<std::unique_ptr<juce::Label>> fieldLabels;
    /// The value labels of the detail rows, created as rows are needed and reused afterwards.
    std::vector<std::unique_ptr<juce::Label>> valueLabels;
    /// True in the compact row, which shows only the metric cards.
    bool compactMode {false};
    /// True in the wide layout, which shows the metrics in one row and the short details two to a line.
    bool wideMode {false};
    /// True when the file name was split into a name and a title.
    bool hasTitle {false};
    /// True when the status label is shown.
    bool hasStatus {false};
};
}  // namespace audiobatch
