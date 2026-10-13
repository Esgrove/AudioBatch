/// Compact metadata panel shown beside the waveform preview in the GUI.
/// Declares AudioInfoPanel, a two-column list of field names and values
/// that shows the format, levels, and status of the selected file.

#pragma once

#include "AudioAnalysisTypes.h"
#include "SeratoTagTypes.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

/// One row of the panel: the field name and its value.
using AudioInfoRow = std::pair<juce::String, juce::String>;

/// Compact two-column metadata panel displayed beside the waveform preview.
class AudioInfoPanel : public juce::Component
{
public:
    void clear();

    void showRecord(
        const AudioAnalysisRecord& record,
        const juce::String& activeStatusLabel,
        const audiobatch::serato::SeratoData* data = nullptr
    );

    [[nodiscard]] int getPreferredHeight() const;

    void setRows(std::vector<AudioInfoRow> newRows);

    void setPreferredHeightChangedCallback(std::function<void()> callback);

    void paint(juce::Graphics& graphics) override;

    void resized() override;

    /// Height in pixels of one row.
    static constexpr int rowHeight = 20;
    /// Padding in pixels above and below the rows.
    static constexpr int verticalPadding = 8;
    /// Padding in pixels left and right of the rows.
    static constexpr int horizontalPadding = 10;

private:
    void ensureLabelCount(std::size_t requiredCount);

    std::vector<AudioInfoRow> rows;
    std::vector<std::unique_ptr<juce::Label>> fieldLabels;
    std::vector<std::unique_ptr<juce::Label>> valueLabels;
    std::function<void()> onPreferredHeightChanged;
};
