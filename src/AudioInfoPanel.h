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
    /// Height in pixels of one row.
    static constexpr int rowHeight = 20;
    /// Padding in pixels above and below the rows.
    static constexpr int verticalPadding = 8;
    /// Padding in pixels left and right of the rows.
    static constexpr int horizontalPadding = 10;

    /// Removes all rows so the panel shows nothing and collapses to its minimum height.
    void clear();

    /// Shows the details of an analysis record.
    /// A record still being analyzed shows its name and activeStatusLabel, or "Pending" when that is empty,
    /// and a failed record shows its name and error.
    /// Optional selected-file metadata adds BPM and key rows only when those values are available.
    void showRecord(
        const AudioAnalysisRecord& record,
        const juce::String& activeStatusLabel,
        const audiobatch::serato::SeratoData* data = nullptr
    );

    /// Returns the height needed to show every row plus padding,
    /// used to size the panel inside its scrolling viewport.
    [[nodiscard]] int getPreferredHeight() const;

    /// Replaces the displayed rows, reusing existing labels and hiding any leftover ones.
    /// Notifies the preferred-height callback so the parent can re-run its layout.
    void setRows(std::vector<AudioInfoRow> newRows);

    /// Registers a callback invoked whenever the preferred height changes,
    /// so the owner can resize the surrounding viewport.
    void setPreferredHeightChangedCallback(std::function<void()> callback);

    /// Draws the rounded panel background with a subtle outline.
    void paint(juce::Graphics& graphics) override;

    /// Positions the field and value labels in two columns, one fixed-height row per entry.
    void resized() override;

private:
    /// Grows the label pools until at least the required number of field and value label pairs exist.
    /// Labels are only ever added, never destroyed, so row updates stay cheap.
    void ensureLabelCount(std::size_t requiredCount);

    std::vector<AudioInfoRow> rows;
    std::vector<std::unique_ptr<juce::Label>> fieldLabels;
    std::vector<std::unique_ptr<juce::Label>> valueLabels;
    std::function<void()> onPreferredHeightChanged;
};
