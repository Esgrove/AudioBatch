/// Selected-file inspector beside the waveform preview.
/// Keeps the filename, optionally split into artist and title, and four analysis metrics above scrolling file details.
/// Pending and failed records also show their status.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "serato/SeratoTagTypes.h"

#include <JuceHeader.h>

#include <array>
#include <memory>
#include <vector>

namespace audiobatch
{
/// One full-width read-only detail with a field caption and value.
struct AudioInfoRow {
    juce::String field;
    juce::String value;
};

/// A labelled analysis value with an explicit unit and a restrained card background.
class AudioInfoMetric final : public juce::Component
{
public:
    AudioInfoMetric(const juce::String& name, const juce::String& caption, const juce::String& unit);

    void setValue(const juce::String& value);
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    juce::Label captionLabel;
    juce::Label unitLabel;
    juce::Label valueLabel;
};

/// Read-only inspector with a fixed summary and a vertically scrolling details region.
class AudioInfoPanel : public juce::Component
{
public:
    AudioInfoPanel();

    void clear();

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

    void ensureLabelCount(std::size_t requiredCount);
    void setRows(std::vector<AudioInfoRow> newRows);

    juce::Label filenameLabel {"File"};
    juce::Label titleLabel {"Title"};
    juce::Label statusLabel {"Status"};
    std::array<AudioInfoMetric, 4> metrics {
        AudioInfoMetric {"Sample peak", "Sample peak", "dBFS"},
        AudioInfoMetric {"True peak", "True peak", "dBTP"},
        AudioInfoMetric {"Integrated loudness", "Integrated", "LUFS"},
        AudioInfoMetric {"Max short-term", "Short-term", "LUFS"},
    };
    juce::Component detailsContent;
    juce::Viewport detailsViewport;
    juce::Label errorLabel {"Error"};
    std::vector<AudioInfoRow> rows;
    std::vector<std::unique_ptr<juce::Label>> fieldLabels;
    std::vector<std::unique_ptr<juce::Label>> valueLabels;
    juce::String displayedPath;
};
}  // namespace audiobatch
