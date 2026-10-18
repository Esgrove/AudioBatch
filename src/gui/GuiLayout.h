/// Shared GUI geometry and compile-time layout calculations.
/// Groups window, preview, inspector, results-table, plugin-editor, waveform,
/// and theme sizes by topic. Component implementations consume these values
/// without owning unrelated layout configuration.

#pragma once

#include "gui/AudioBatchComponentConstants.h"

#include <JuceHeader.h>

namespace audiobatch
{
/// Adjacent full-height sections of the lower workspace, in parent coordinates.
struct PreviewColumns {
    juce::Rectangle<int> controls;
    juce::Rectangle<int> information;
    juce::Rectangle<int> waveform;
};

/// Logical-pixel dimensions and proportions used by the application shell and reusable components.
namespace layout
{
constexpr int titleBarHeight = 30;
constexpr int inWindowMenuBarHeight = 26;
constexpr int defaultWindowWidth = 1920;
constexpr int defaultWindowHeight = 1080;
constexpr int minimumWindowWidth = 800;
constexpr int minimumWindowHeight = 600;
constexpr int maximumWindowSize = 8192;
constexpr int initialComponentWidth = defaultWindowWidth;
constexpr int initialComponentHeight = defaultWindowHeight;
constexpr int mainContentPadding = 4;
constexpr int topBarHeight = 32;
constexpr int statusLabelWidth = 220;
constexpr int sectionSpacing = 10;
constexpr int overallProgressBarHeight = 3;
constexpr int resultsTableRowHeight = 24;

/// JUCE's negative sizes express proportions of the available vertical space.
constexpr double resultsTableMinimumHeight = 240.0;
constexpr double resultsTableMaximumProportion = -0.75;
constexpr double resultsTablePreferredProportion = -0.5;
constexpr double resizeBarHeight = 6.0;
constexpr double previewMinimumHeight = 220.0;
constexpr double previewMaximumProportion = -0.7;
constexpr double previewPreferredProportion = -0.26;

constexpr int controlButtonHeight = 28;
constexpr int processButtonHeight = 34;
constexpr int controlsSectionSpacing = 8;
constexpr int gainTextBoxWidth = 60;
constexpr int normalizeToggleHeight = 22;
constexpr int normalizeTargetSpacing = 8;
constexpr int zoomTextBoxWidth = 60;
constexpr int controlsMinimumWidth = 209;
constexpr int controlsMaximumWidth = 228;
constexpr int informationMinimumWidth = 234;
constexpr int informationMaximumWidth = 270;
constexpr double controlsWidthProportion = 0.14;
constexpr double informationWidthProportion = 0.22;
constexpr int sliderRowHeight = 24;
constexpr int controlsContentHeight = sliderRowHeight * 4 + normalizeToggleHeight + normalizeTargetSpacing
    + controlButtonHeight * 3 + controlsSectionSpacing * 4;
constexpr int dialogContentWidth = 500;
constexpr int dialogContentHeight = 250;
constexpr int dialogMaximumWindowSize = 4096;
constexpr int tooltipDelayMilliseconds = 700;

/// Compact workspace geometry leaves room for the waveform even at the minimum window width.
namespace compactPreview
{
constexpr int height = 64;
constexpr int padding = 6;
constexpr int spacing = 6;
constexpr int toggleWidth = 24;
constexpr int playbackWidth = 56;
constexpr int processWidth = 68;
constexpr int buttonHeight = 28;
constexpr int metricWidth = 100;
constexpr int metricSpacing = 4;
constexpr int metricsWidth = metricWidth * 4 + metricSpacing * 3;
constexpr int minimumWaveformWidth = 120;
constexpr float valueFontHeight = 16.0f;
constexpr int animationDurationMilliseconds = 180;
constexpr int animationFramesPerSecond = 60;

static_assert(
    minimumWindowWidth - mainContentPadding * 2 - padding * 2 - toggleWidth - playbackWidth - processWidth
            - metricsWidth - spacing * 4
        >= minimumWaveformWidth,
    "The compact controls and all four metrics must leave room for the waveform at the minimum window width"
);
}  // namespace compactPreview

/// Reserve scrollbar width even when hidden, avoiding width/height relayout feedback.
constexpr int scrollBarThickness = 8;

/// Fixed summary cards and compact secondary details in the selected-file inspector.
namespace information
{
constexpr float cornerRadius = 6.0f;
constexpr float outlineAlpha = 0.08f;
constexpr float captionFontHeight = 12.0f;
constexpr float valueFontHeight = 13.0f;
constexpr float metricFontHeight = 20.0f;
constexpr float headingFontHeight = 13.0f;
constexpr int filenameHeight = 22;
constexpr int statusHeight = 18;
constexpr int metricHeight = 52;
constexpr int metricPadding = 6;
constexpr int metricCaptionHeight = 16;
constexpr int metricUnitWidth = 30;
constexpr int detailFieldWidth = 60;
constexpr int rowHeight = 20;
constexpr int verticalPadding = 8;
constexpr int horizontalPadding = 10;
/// The optional pending/error status row is added separately.
constexpr int summaryHeight = filenameHeight + sectionSpacing + metricHeight * 2 + sectionSpacing * 2;
}  // namespace information

/// Plugin-chain rows, action buttons, and the scrolling editor's outer margin.
namespace pluginEditor
{
constexpr int rowHeight = 36;
constexpr int rowPadding = 4;
constexpr int addButtonSize = 28;
constexpr int margin = 8;
constexpr int rowIconButtonWidth = 24;
constexpr int rowEditButtonWidth = 48;
constexpr float rowCornerRadius = 4.0f;
}  // namespace pluginEditor

/// Waveform overlays retain fixed logical sizes independently of channel and zoom geometry.
namespace waveform
{
constexpr float cornerRadius = information::cornerRadius;
constexpr float positionMarkerWidth = 1.5f;
constexpr float placeholderFontHeight = 14.0f;
constexpr float cueArrowHeight = 8.0f;
constexpr float cueArrowWidth = 10.0f;
constexpr float cueArrowOutlineWidth = 1.5f;
constexpr float focusOutlineWidth = 1.0f;
}  // namespace waveform

/// Font and paint geometry shared by the application look-and-feel.
namespace theme
{
constexpr float monoFontHeight = 15.0f;
/// Fullscreen glyph geometry uses its normalized 100 by 100 path rather than logical pixels.
constexpr float fullscreenGlyphInset = 45.0f;
constexpr float fullscreenGlyphStrokeThickness = 30.0f;
constexpr int popupMenuSeparatorInset = 5;
constexpr int popupMenuItemMaximumPadding = 5;
/// Menu width determines padding, bounded by popupMenuItemMaximumPadding.
constexpr int popupMenuItemPaddingDivisor = 20;
constexpr int popupMenuTickInsetDivisor = 5;
constexpr float subMenuArrowWidthRatio = 0.6f;
constexpr float shortcutFontHeightRatio = 0.75f;
constexpr float shortcutFontHorizontalScale = 0.95f;
constexpr float toggleFontHeight = 15.0f;
constexpr float toggleFontHeightRatio = 0.75f;
constexpr float toggleTickWidthRatio = 1.1f;
constexpr int toggleTextSpacing = 6;
constexpr float maximumButtonFontHeight = 16.0f;
constexpr float buttonFontHeightRatio = 0.6f;
constexpr float tableHeaderFontHeightRatio = 0.5f;
/// Column titles retain JUCE's proportional size with one additional logical pixel.
constexpr float tableHeaderFontHeightIncrease = 1.0f;
constexpr int tableHeaderTextInset = 4;
constexpr float tableHeaderSortArrowTip = 0.8f;
constexpr float tooltipCornerRadius = 5.0f;
constexpr float tooltipFontHeight = 13.0f;
constexpr int tooltipMaximumWidth = 400;
}  // namespace theme

/// Initial, minimum, and maximum results-table column widths.
namespace table
{
constexpr int nameColumnMinimumWidth = 110;
constexpr int pathColumnMinimumWidth = 140;
constexpr int typeColumnMinimumWidth = 60;
constexpr int bitrateColumnMinimumWidth = 68;
constexpr int sampleRateColumnMinimumWidth = 72;
constexpr int metricColumnMinimumWidth = 72;
constexpr int nameColumnDefaultWidth = 140;
constexpr int pathColumnDefaultWidth = 160;
constexpr int typeColumnDefaultWidth = 66;
constexpr int bitrateColumnDefaultWidth = 70;
constexpr int sampleRateColumnDefaultWidth = 76;
constexpr int overallPeakColumnDefaultWidth = 74;
constexpr int peakLeftColumnDefaultWidth = 74;
constexpr int peakRightColumnDefaultWidth = 74;
constexpr int truePeakColumnDefaultWidth = 78;
constexpr int maxShortTermLufsColumnDefaultWidth = 76;
constexpr int integratedLufsColumnDefaultWidth = 76;
constexpr int customGainColumnDefaultWidth = 72;
constexpr int statusColumnDefaultWidth = 240;
constexpr int nameMaximumWidth = 600;
constexpr int pathMaximumWidth = 1200;
constexpr int metricMaximumWidth = 160;
constexpr int statusMaximumWidth = 800;

/// Preserve the original compact-column budget independently of the larger main-window default.
constexpr int initiallyVisibleColumnWidthBudget = 1096;
constexpr int initiallyVisibleColumnWidth = nameColumnDefaultWidth + pathColumnDefaultWidth + typeColumnDefaultWidth
    + bitrateColumnDefaultWidth + sampleRateColumnDefaultWidth + overallPeakColumnDefaultWidth
    + truePeakColumnDefaultWidth + maxShortTermLufsColumnDefaultWidth + integratedLufsColumnDefaultWidth
    + statusColumnDefaultWidth;

static_assert(
    initiallyVisibleColumnWidth <= initiallyVisibleColumnWidthBudget,
    "Initially visible columns must fit the default column-width budget"
);

/// Returns a column's compile-time initial width, including columns hidden by default.
consteval int initialColumnWidth(const AudioFileTableColumn columnId)
{
    switch (columnId) {
        case columnName:
            return nameColumnDefaultWidth;
        case columnPath:
            return pathColumnDefaultWidth;
        case columnType:
            return typeColumnDefaultWidth;
        case columnBitrate:
            return bitrateColumnDefaultWidth;
        case columnSampleRate:
            return sampleRateColumnDefaultWidth;
        case columnOverallPeak:
            return overallPeakColumnDefaultWidth;
        case columnPeakLeft:
            return peakLeftColumnDefaultWidth;
        case columnPeakRight:
            return peakRightColumnDefaultWidth;
        case columnOverallTruePeak:
            return truePeakColumnDefaultWidth;
        case columnMaxShortTermLufs:
            return maxShortTermLufsColumnDefaultWidth;
        case columnIntegratedLufs:
            return integratedLufsColumnDefaultWidth;
        case columnCustomGain:
            return customGainColumnDefaultWidth;
        case columnStatus:
            return statusColumnDefaultWidth;
    }
    return 0;
}

/// Returns the compile-time lower drag limit, retaining a common minimum for numeric and status columns.
consteval int minimumColumnWidth(const AudioFileTableColumn columnId)
{
    switch (columnId) {
        case columnName:
            return nameColumnMinimumWidth;
        case columnPath:
            return pathColumnMinimumWidth;
        case columnType:
            return typeColumnMinimumWidth;
        case columnBitrate:
            return bitrateColumnMinimumWidth;
        case columnSampleRate:
            return sampleRateColumnMinimumWidth;
        case columnOverallPeak:
        case columnPeakLeft:
        case columnPeakRight:
        case columnOverallTruePeak:
        case columnMaxShortTermLufs:
        case columnIntegratedLufs:
        case columnCustomGain:
        case columnStatus:
            return metricColumnMinimumWidth;
    }
    return 0;
}
}  // namespace table

/// Allocates stable sidebar widths and gives the waveform the remaining horizontal space.
/// Widths are bounded even during transient zero-sized construction layouts.
inline PreviewColumns previewColumns(juce::Rectangle<int> bounds)
{
    const auto controlsWidth = juce::jlimit(
        controlsMinimumWidth, controlsMaximumWidth, static_cast<int>(bounds.getWidth() * controlsWidthProportion)
    );
    const auto informationWidth = juce::jlimit(
        informationMinimumWidth,
        informationMaximumWidth,
        static_cast<int>(bounds.getWidth() * informationWidthProportion)
    );
    const auto controls = bounds.removeFromLeft(juce::jmin(bounds.getWidth(), controlsWidth));
    bounds.removeFromLeft(juce::jmin(bounds.getWidth(), sectionSpacing));
    const auto information = bounds.removeFromLeft(juce::jmin(bounds.getWidth(), informationWidth));
    bounds.removeFromLeft(juce::jmin(bounds.getWidth(), sectionSpacing));
    return {.controls = controls, .information = information, .waveform = bounds};
}

/// Centres the initial window within the display's usable area, reducing it on smaller screens.
inline juce::Rectangle<int> initialWindowBounds(const juce::Rectangle<int> usableArea)
{
    return usableArea.withSizeKeepingCentre(
        juce::jmin(defaultWindowWidth, usableArea.getWidth()), juce::jmin(defaultWindowHeight, usableArea.getHeight())
    );
}
}  // namespace layout
}  // namespace audiobatch
