/// Shared GUI geometry and compile-time layout calculations.
/// Groups window, preview, inspector, results-table, plugin-editor, waveform,
/// and theme sizes by topic. Component implementations consume these values
/// without owning unrelated layout configuration.

#pragma once

#include "gui/AudioBatchComponentConstants.h"

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace audiobatch
{
/// Adjacent full-height sections of the lower workspace, in parent coordinates.
struct PreviewColumns {
    // Field order matches the left-to-right positional layout returned by the geometry helpers.
    /// The playback, gain, normalization, plugin, and settings controls on the left.
    juce::Rectangle<int> controls;
    /// The summary and details of the selected file in the middle.
    juce::Rectangle<int> information;
    /// The waveform, which takes the rest of the width.
    juce::Rectangle<int> waveform;
    /// True when the controls and the information use their wide layouts at the original window breakpoint.
    bool wide {false};
};

/// Logical-pixel dimensions and proportions used by the application shell and reusable components.
namespace layout
{
constexpr int TITLE_BAR_HEIGHT {30};
constexpr int IN_WINDOW_MENU_BAR_HEIGHT {26};
constexpr int DEFAULT_WINDOW_WIDTH {1920};
constexpr int DEFAULT_WINDOW_HEIGHT {1080};
constexpr int MINIMUM_WINDOW_WIDTH {800};
constexpr int MINIMUM_WINDOW_HEIGHT {600};
constexpr int MAXIMUM_WINDOW_SIZE {8192};
constexpr int INITIAL_COMPONENT_WIDTH {DEFAULT_WINDOW_WIDTH};
constexpr int INITIAL_COMPONENT_HEIGHT {DEFAULT_WINDOW_HEIGHT};
constexpr int MAIN_CONTENT_PADDING {4};
constexpr int SECTION_SPACING {10};
constexpr int RESULTS_TABLE_ROW_HEIGHT {24};

/// JUCE's negative sizes express proportions of the available vertical space.
/// The table has no upper limit, so a tall window gives it the height the preview keeps out of.
/// The expanded preview starts at the height that fits its controls, a fixed height from then on.
constexpr double RESULTS_TABLE_MINIMUM_HEIGHT {240.0};
constexpr double RESULTS_TABLE_MAXIMUM_PROPORTION {-1.0};
constexpr double RESIZE_BAR_HEIGHT {6.0};
constexpr double PREVIEW_MINIMUM_HEIGHT {230.0};
constexpr double PREVIEW_MAXIMUM_PROPORTION {-0.7};

constexpr int CONTROL_BUTTON_HEIGHT {28};
constexpr int PROCESS_BUTTON_HEIGHT {38};
constexpr int CONTROLS_SECTION_SPACING {8};
constexpr int GAIN_TEXT_BOX_WIDTH {60};
constexpr int NORMALIZE_TOGGLE_HEIGHT {22};
constexpr int NORMALIZE_TARGET_SPACING {8};
constexpr int ZOOM_TEXT_BOX_WIDTH {60};
constexpr int CONTROLS_MINIMUM_WIDTH {209};
constexpr int CONTROLS_MAXIMUM_WIDTH {228};
constexpr int INFORMATION_MINIMUM_WIDTH {234};
constexpr int INFORMATION_MAXIMUM_WIDTH {270};
constexpr double CONTROLS_WIDTH_PROPORTION {0.14};
constexpr double INFORMATION_WIDTH_PROPORTION {0.22};

/// Persisted widths scale the original responsive layout instead of storing a fixed pixel width.
namespace previewWidths
{
constexpr double DEFAULT_CONTROLS_SCALE {0.95};
constexpr double DEFAULT_INFORMATION_SCALE {0.90};
constexpr double MINIMUM_CONTROLS_SCALE {0.855};
constexpr double MINIMUM_INFORMATION_SCALE {0.81};
constexpr double MAXIMUM_SCALE {1.10};
constexpr int MINIMUM_WAVEFORM_WIDTH {140};
constexpr int KEYBOARD_STEP {4};
constexpr int LARGE_KEYBOARD_STEP {16};
constexpr float INDICATOR_WIDTH {2.0f};
constexpr int INDICATOR_INSET {8};
constexpr float INDICATOR_ALPHA {0.45f};
}  // namespace previewWidths

constexpr int SLIDER_ROW_HEIGHT {24};
/// The Peak and Loudness buttons above the target slider and the Normalize button.
constexpr int NORMALIZATION_TARGET_HEIGHT {CONTROL_BUTTON_HEIGHT * 2 + NORMALIZE_TARGET_SPACING};
constexpr int CONTROLS_CONTENT_HEIGHT {
    SLIDER_ROW_HEIGHT * 4 + NORMALIZE_TOGGLE_HEIGHT + NORMALIZE_TARGET_SPACING + NORMALIZATION_TARGET_HEIGHT
        + CONTROL_BUTTON_HEIGHT * 2 + CONTROLS_SECTION_SPACING * 4,
};

/// The wide preview, in which the controls and the information share half the workspace width
/// and the waveform gets the other half.
namespace widePreview
{
/// Narrowest half of the workspace width that turns on the wide layouts.
constexpr int MINIMUM_HALF_WIDTH {790};
/// Share of the shared half given to the controls, the rest going to the information.
constexpr double CONTROLS_PROPORTION {0.48};
/// Widest controls and information, so on very wide windows the waveform grows instead.
constexpr int CONTROLS_MAXIMUM_WIDTH {500};
constexpr int INFORMATION_MAXIMUM_WIDTH {540};
/// Space between the two columns of the controls below the sliders.
constexpr int COLUMN_SPACING {10};
/// Each column below the sliders has three rows as tall as a button,
/// the normalization target taking two of them on the right.
constexpr int ROW_COUNT {3};
}  // namespace widePreview

constexpr int WIDE_CONTROLS_CONTENT_HEIGHT {
    SLIDER_ROW_HEIGHT * 4 + CONTROLS_SECTION_SPACING * 2 + CONTROL_BUTTON_HEIGHT * widePreview::ROW_COUNT
        + CONTROLS_SECTION_SPACING * (widePreview::ROW_COUNT - 1),
};
static_assert(
    NORMALIZATION_TARGET_HEIGHT == CONTROL_BUTTON_HEIGHT * 2 + CONTROLS_SECTION_SPACING,
    "The normalization target must line up with two rows of the left column in the wide controls"
);
constexpr int DIALOG_CONTENT_WIDTH {500};
constexpr int DIALOG_CONTENT_HEIGHT {250};
constexpr int DIALOG_MAXIMUM_WINDOW_SIZE {4096};
constexpr int TOOLTIP_DELAY_MILLISECONDS {700};

/// Small outlined disclosure icon inside the controls, without a separate sidebar.
namespace previewToggle
{
constexpr int SIZE {14};
constexpr int IMAGE_INSET {2};
constexpr float GLYPH_WIDTH {10.0f};
constexpr float GLYPH_HEIGHT {5.0f};
constexpr float STROKE_WIDTH {1.5f};
}  // namespace previewToggle

/// Compact workspace geometry leaves room for the waveform even at the minimum window width.
namespace compactPreview
{
constexpr int HEIGHT {52};
constexpr int PADDING {4};
constexpr int SPACING {6};
constexpr int METRIC_MINIMUM_WIDTH {100};
constexpr int METRIC_CAPTION_HEIGHT {14};
constexpr int METRIC_SPACING {5};
constexpr int METRICS_MINIMUM_WIDTH {METRIC_MINIMUM_WIDTH * 4 + METRIC_SPACING * 3};
constexpr int MINIMUM_WAVEFORM_WIDTH {140};
constexpr double METRICS_WIDTH_PROPORTION {0.35};
constexpr float MINIMUM_VALUE_HORIZONTAL_SCALE {0.5f};
constexpr int ANIMATION_DURATION_MILLISECONDS {180};

}  // namespace compactPreview

/// Width of the controls column scroll bar, reserved only while the controls overflow the column.
constexpr int SCROLL_BAR_THICKNESS {8};

/// Bottom status bar with the root path, the status text, and the audio output device,
/// and the overall progress line along its top edge.
namespace statusBar
{
constexpr int HEIGHT {24};
constexpr int HORIZONTAL_PADDING {8};
constexpr int SECTION_SPACING {16};
/// Share of the width left of the output device given to the root path, leaving the status text the larger part.
constexpr double PATH_PROPORTION {0.4};
constexpr int OUTPUT_STATUS_MAXIMUM_WIDTH {360};
/// Vertical gap above and below the separators between the sections.
constexpr int SEPARATOR_INSET {5};
constexpr int PROGRESS_HEIGHT {3};
constexpr float FONT_HEIGHT {14.0f};
}  // namespace statusBar

/// Fixed summary cards and compact secondary details in the selected-file inspector.
namespace information
{
constexpr float CORNER_RADIUS {6.0f};
constexpr float OUTLINE_ALPHA {0.08f};
constexpr float CAPTION_FONT_HEIGHT {12.0f};
constexpr float VALUE_FONT_HEIGHT {13.0f};
constexpr float METRIC_FONT_HEIGHT {20.0f};
/// Smaller secondary values remain readable beside the source metric.
constexpr float ADJUSTED_METRIC_FONT_HEIGHT {15.0f};
constexpr int ADJUSTED_METRIC_SPACING {4};
constexpr float MINIMUM_VALUE_HORIZONTAL_SCALE {0.7f};
constexpr float HEADING_FONT_HEIGHT {13.0f};
constexpr int FILENAME_HEIGHT {22};
constexpr int STATUS_HEIGHT {18};
constexpr int METRIC_HEIGHT {52};
constexpr int METRIC_PADDING {6};
constexpr int METRIC_CAPTION_HEIGHT {16};
/// Minimum separation between a full metric caption and its unit.
constexpr int METRIC_CAPTION_SPACING {4};
/// Shared property gap, with the second lane yielding extra trailing whitespace.
constexpr int PROPERTY_COLUMN_GAP {4};
constexpr int SAMPLE_RATE_COLUMN_REDUCTION {12};
constexpr int DETAIL_FIELD_WIDTH {60};
constexpr int ROW_HEIGHT {20};
/// Vertical clearance for fully wrapped detail text, shared by measurement and painting.
constexpr int DETAIL_TEXT_PADDING {3};
constexpr int VERTICAL_PADDING {8};
constexpr int HORIZONTAL_PADDING {10};
/// Side of the square album artwork slot beside the file name, in logical pixels.
constexpr int ARTWORK_SIZE {48};
/// Gap between the artwork and the file name.
constexpr int ARTWORK_SPACING {8};
/// The optional pending/error status row is added separately.
constexpr int SUMMARY_HEIGHT {FILENAME_HEIGHT + SECTION_SPACING + METRIC_HEIGHT * 2 + SECTION_SPACING * 2};
}  // namespace information

static_assert(
    MINIMUM_WINDOW_WIDTH - MAIN_CONTENT_PADDING * 2 - compactPreview::PADDING * 2 - CONTROLS_MINIMUM_WIDTH
            - compactPreview::METRICS_MINIMUM_WIDTH - SECTION_SPACING * 2
        >= compactPreview::MINIMUM_WAVEFORM_WIDTH,
    "The compact controls and all four metrics must leave room for the waveform at the minimum window width"
);

/// Plugin-chain rows, action buttons, and the scrolling editor's outer margin.
namespace pluginEditor
{
constexpr int ROW_HEIGHT {54};
constexpr int NAME_HEIGHT {24};
constexpr int FOOTER_HEIGHT {48};
constexpr int FOOTER_LINE_HEIGHT {22};
constexpr int ROW_PADDING {4};
constexpr int ADD_BUTTON_SIZE {28};
constexpr int MARGIN {8};
constexpr int ROW_ICON_BUTTON_WIDTH {24};
/// The arrow and remove buttons are square, as tall as the row inside its padding.
constexpr int ROW_SQUARE_BUTTON_SIZE {ROW_HEIGHT - 2 * ROW_PADDING};
constexpr int ROW_EDIT_BUTTON_WIDTH {48};
/// The Clear Chain button, right-aligned on the line of the add button.
constexpr int CLEAR_CHAIN_BUTTON_WIDTH {96};
constexpr float ROW_CORNER_RADIUS {4.0f};
/// The preset row above the slot rows, with its label, preset list, and update, save, and delete buttons.
constexpr int PRESET_ROW_HEIGHT {28};
constexpr int PRESET_LABEL_WIDTH {52};
constexpr int PRESET_BUTTON_WIDTH {84};
constexpr int UPDATE_PRESET_BUTTON_WIDTH {64};
/// Initial size of the generic parameter editor shown for plugins without a custom editor.
constexpr int GENERIC_EDITOR_WIDTH {450};
constexpr int GENERIC_EDITOR_HEIGHT {600};
/// Initial size of the plugin chain editor window.
constexpr int CHAIN_WINDOW_WIDTH {560};
constexpr int CHAIN_WINDOW_HEIGHT {420};
/// Initial size of the plugin manager window that lists and scans plugins.
constexpr int MANAGER_WINDOW_WIDTH {720};
constexpr int MANAGER_WINDOW_HEIGHT {520};
}  // namespace pluginEditor

/// Device and preview preferences share one scrollable dialog without changing unrelated dialogs.
namespace audioSettings
{
constexpr int PREFERRED_WIDTH {560};
constexpr int PREFERRED_HEIGHT {560};
/// Audio Settings can shrink below its initial size while the content viewport remains scrollable.
constexpr int MINIMUM_WINDOW_HEIGHT {440};
/// Selector width relative to the available control lane, leaving room without spanning the whole group.
constexpr double PREVIEW_SELECTOR_WIDTH_PROPORTION {0.7};
constexpr int PADDING {12};
constexpr int SECTION_SPACING {10};
/// Includes the heading and its clearance, rather than adding another vertical padding inset.
constexpr int GROUP_TOP_INSET {18};
constexpr int ROW_HEIGHT {30};
constexpr int PREFERENCE_ROW_COUNT {7};
constexpr int OVERLOAD_ROW_COUNT {2};
constexpr int OVERLOAD_GROUP_HEIGHT {GROUP_TOP_INSET + ROW_HEIGHT * OVERLOAD_ROW_COUNT + PADDING};
constexpr int RANGE_LABEL_WIDTH {80};
constexpr int RANGE_EDITOR_WIDTH {48};
constexpr int RANGE_EDITOR_PADDING {3};
constexpr int PREFERENCES_HEIGHT {GROUP_TOP_INSET + ROW_HEIGHT * PREFERENCE_ROW_COUNT + PADDING};
}  // namespace audioSettings

/// Waveform overlays retain fixed logical sizes independently of channel and zoom geometry.
namespace waveform
{
/// Inset of the waveform, the loudness axis, and the cue arrows from the edges of the component.
constexpr int CONTENT_INSET {2};
constexpr float CORNER_RADIUS {information::CORNER_RADIUS};
constexpr float CUE_ARROW_HEIGHT {8.0f};
constexpr float CUE_ARROW_OUTLINE_WIDTH {1.5f};
constexpr float CUE_ARROW_WIDTH {10.0f};
constexpr float FOCUS_OUTLINE_WIDTH {1.0f};
constexpr float PLACEHOLDER_FONT_HEIGHT {14.0f};
constexpr float POSITION_MARKER_WIDTH {1.5f};

constexpr float LOUDNESS_FONT_HEIGHT {10.0f};
constexpr float LOUDNESS_LINE_WIDTH {1.5f};
constexpr float LOUDNESS_OUTLINE_WIDTH {1.4f};
/// Equal total gutter for loudness, amplitude and frequency axes.
constexpr int AXIS_GUTTER_WIDTH {22};
constexpr int AXIS_LABEL_GAP {2};
constexpr int AMPLITUDE_LABEL_HEIGHT {10};
constexpr int AMPLITUDE_LABEL_SPACING {2};
constexpr int LOUDNESS_AXIS_GAP {AXIS_LABEL_GAP};
constexpr int LOUDNESS_AXIS_WIDTH {AXIS_GUTTER_WIDTH - AXIS_LABEL_GAP};
constexpr int LOUDNESS_TICK_SPACING {32};

/// Separation between equal preview panes.
constexpr int PANE_GAP {2};
/// Frequency captions stay outside the spectral raster.
constexpr int FREQUENCY_AXIS_WIDTH {AXIS_GUTTER_WIDTH - AXIS_LABEL_GAP};
constexpr int FREQUENCY_AXIS_GAP {AXIS_LABEL_GAP};
constexpr int FREQUENCY_TICK_SPACING {16};
/// Dedicated source-overload strip does not share the frequency axis.
constexpr int SOURCE_WARNING_HEIGHT {4};
/// Fixed spectral presentation opacities, independent of calibrated raster power.
constexpr float SPECTROGRAM_PLACEHOLDER_ALPHA {0.7f};
constexpr float FREQUENCY_LABEL_ALPHA {0.8f};
constexpr float FREQUENCY_GRID_ALPHA {0.1f};
constexpr float PLAYHEAD_ALPHA {0.85f};
/// The pointer's release target stays quieter than the actual playback cursor.
constexpr float SEEK_PREVIEW_ALPHA {0.45f};

/// Height of overload caps at the outer edges of the waveform.
constexpr float OVERLOAD_CAP_HEIGHT {2.5f};
/// Narrowest overload mark, so a single-column overload stays visible when zoomed out.
constexpr float OVERLOAD_MINIMUM_WIDTH {3.0f};
/// Opacity of the optional yellow or orange tint over the overloaded source channel.
constexpr float OVERLOAD_TINT_ALPHA {0.06f};
/// Amplitude reference lines match the subtle loudness grid without adding a second grid over it.
constexpr float AMPLITUDE_GRID_ALPHA {0.12f};

/// Chooses a uniform whole-LU interval for valid integer loudness limits.
/// Even steps keep labels anchored to even values, except when a narrow range needs one-LU detail.
constexpr int loudnessTickInterval(const int minimum, const int maximum, const int height)
{
    constexpr std::array intervals {1, 2, 4, 6, 8, 10, 12, 16, 20};
    const auto targetCount = std::max(2, height / LOUDNESS_TICK_SPACING);
    for (const auto interval : intervals) {
        if (interval * targetCount >= maximum - minimum) {
            return interval;
        }
    }
    return intervals.back();
}
}  // namespace waveform

/// Font and paint geometry shared by the application look-and-feel.
namespace theme
{
constexpr float MONO_FONT_HEIGHT {15.0f};
constexpr float MENU_BAR_FONT_HEIGHT {16.0f};
constexpr float POPUP_MENU_FONT_HEIGHT {15.0f};
constexpr int POPUP_MENU_ITEM_HEIGHT {28};
/// Fullscreen glyph geometry uses its normalized 100 by 100 path rather than logical pixels.
constexpr float FULLSCREEN_GLYPH_INSET {45.0f};
constexpr float FULLSCREEN_GLYPH_STROKE_THICKNESS {30.0f};
constexpr int POPUP_MENU_SEPARATOR_INSET {5};
constexpr int POPUP_MENU_ITEM_MAXIMUM_PADDING {5};
/// Menu width determines padding, bounded by popupMenuItemMaximumPadding.
constexpr int POPUP_MENU_ITEM_PADDING_DIVISOR {20};
constexpr int POPUP_MENU_TICK_INSET_DIVISOR {5};
constexpr float SUB_MENU_ARROW_WIDTH_RATIO {0.6f};
constexpr float SHORTCUT_FONT_HEIGHT_RATIO {0.75f};
constexpr float SHORTCUT_FONT_HORIZONTAL_SCALE {0.95f};
constexpr float TOGGLE_FONT_HEIGHT {15.0f};
constexpr float TOGGLE_FONT_HEIGHT_RATIO {0.75f};
constexpr float TOGGLE_TICK_WIDTH_RATIO {1.1f};
constexpr int TOGGLE_TEXT_SPACING {6};
constexpr float MAXIMUM_BUTTON_FONT_HEIGHT {16.0f};
constexpr float BUTTON_FONT_HEIGHT_RATIO {0.6f};
/// Heights of the results table cell text in Roboto and Roboto Mono.
/// A JUCE font height is the ascent plus the descent, so both give the text an em size of 14 pixels.
constexpr float TABLE_CELL_FONT_HEIGHT {16.4f};
constexpr float TABLE_MONO_CELL_FONT_HEIGHT {18.5f};
constexpr float TABLE_HEADER_FONT_HEIGHT_RATIO {0.5f};
/// Column titles retain JUCE's proportional size with one additional logical pixel.
constexpr float TABLE_HEADER_FONT_HEIGHT_INCREASE {1.0f};
constexpr int TABLE_HEADER_TEXT_INSET {4};
constexpr float TABLE_HEADER_SORT_ARROW_TIP {0.8f};
constexpr float TOOLTIP_CORNER_RADIUS {5.0f};
constexpr float TOOLTIP_FONT_HEIGHT {13.0f};
constexpr int TOOLTIP_MAXIMUM_WIDTH {400};
}  // namespace theme

/// Initial, minimum, and maximum results-table column widths, and the insets of the cell contents.
namespace table
{
/// Horizontal inset of the cell text from both edges of its cell.
constexpr int CELL_TEXT_INSET {4};
/// Largest activity indicator in the status column, and the room it leaves above and below it together.
constexpr int ACTIVITY_INDICATOR_MAXIMUM_SIZE {14};
constexpr int ACTIVITY_INDICATOR_VERTICAL_ROOM {8};
/// Gap between the activity indicator and the activity label.
constexpr int ACTIVITY_INDICATOR_SPACING {8};

constexpr int NAME_COLUMN_MINIMUM_WIDTH {110};
constexpr int PATH_COLUMN_MINIMUM_WIDTH {140};
constexpr int TYPE_COLUMN_MINIMUM_WIDTH {60};
constexpr int BIT_DEPTH_COLUMN_MINIMUM_WIDTH {68};
constexpr int BITRATE_COLUMN_MINIMUM_WIDTH {68};
constexpr int SAMPLE_RATE_COLUMN_MINIMUM_WIDTH {72};
constexpr int LENGTH_COLUMN_MINIMUM_WIDTH {60};
constexpr int METRIC_COLUMN_MINIMUM_WIDTH {72};
constexpr int NAME_COLUMN_DEFAULT_WIDTH {210};
constexpr int FOLDER_COLUMN_DEFAULT_WIDTH {140};
constexpr int PATH_COLUMN_DEFAULT_WIDTH {160};
constexpr int TYPE_COLUMN_DEFAULT_WIDTH {66};
constexpr int BIT_DEPTH_COLUMN_DEFAULT_WIDTH {70};
// Wide enough for each short header next to the sort arrow, such as "True Peak".
constexpr int BITRATE_COLUMN_DEFAULT_WIDTH {76};
constexpr int SAMPLE_RATE_COLUMN_DEFAULT_WIDTH {72};
constexpr int LENGTH_COLUMN_DEFAULT_WIDTH {66};
constexpr int OVERALL_PEAK_COLUMN_DEFAULT_WIDTH {76};
constexpr int PEAK_LEFT_COLUMN_DEFAULT_WIDTH {76};
constexpr int PEAK_RIGHT_COLUMN_DEFAULT_WIDTH {76};
constexpr int TRUE_PEAK_COLUMN_DEFAULT_WIDTH {86};
constexpr int MAX_SHORT_TERM_LUFS_COLUMN_DEFAULT_WIDTH {70};
constexpr int INTEGRATED_LUFS_COLUMN_DEFAULT_WIDTH {70};
constexpr int CUSTOM_GAIN_COLUMN_DEFAULT_WIDTH {76};
constexpr int STATUS_COLUMN_DEFAULT_WIDTH {180};
constexpr int NAME_MAXIMUM_WIDTH {600};
constexpr int PATH_MAXIMUM_WIDTH {1200};
constexpr int METRIC_MAXIMUM_WIDTH {160};
constexpr int STATUS_MAXIMUM_WIDTH {800};

/// Width the initially visible columns may take together.
/// It leaves the name and folder columns room to grow in the default window.
constexpr int INITIALLY_VISIBLE_COLUMN_WIDTH_BUDGET {1240};
constexpr int INITIALLY_VISIBLE_COLUMN_WIDTH {
    NAME_COLUMN_DEFAULT_WIDTH + FOLDER_COLUMN_DEFAULT_WIDTH + TYPE_COLUMN_DEFAULT_WIDTH + BIT_DEPTH_COLUMN_DEFAULT_WIDTH
        + BITRATE_COLUMN_DEFAULT_WIDTH + SAMPLE_RATE_COLUMN_DEFAULT_WIDTH + LENGTH_COLUMN_DEFAULT_WIDTH
        + OVERALL_PEAK_COLUMN_DEFAULT_WIDTH + TRUE_PEAK_COLUMN_DEFAULT_WIDTH + MAX_SHORT_TERM_LUFS_COLUMN_DEFAULT_WIDTH
        + INTEGRATED_LUFS_COLUMN_DEFAULT_WIDTH + STATUS_COLUMN_DEFAULT_WIDTH,
};

static_assert(
    INITIALLY_VISIBLE_COLUMN_WIDTH <= INITIALLY_VISIBLE_COLUMN_WIDTH_BUDGET,
    "Initially visible columns must fit the default column-width budget"
);

/// Returns a column's compile-time initial width, including columns hidden by default.
consteval int initialColumnWidth(const AudioFileTableColumn column)
{
    switch (column) {
        case AudioFileTableColumn::name:
            return NAME_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::folder:
            return FOLDER_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::path:
            return PATH_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::type:
            return TYPE_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::bitDepth:
            return BIT_DEPTH_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::bitrate:
            return BITRATE_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::sampleRate:
            return SAMPLE_RATE_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::length:
            return LENGTH_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::overallPeak:
            return OVERALL_PEAK_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::peakLeft:
            return PEAK_LEFT_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::peakRight:
            return PEAK_RIGHT_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::overallTruePeak:
            return TRUE_PEAK_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::maxShortTermLufs:
            return MAX_SHORT_TERM_LUFS_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::integratedLufs:
            return INTEGRATED_LUFS_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::plr:
        case AudioFileTableColumn::psr:
        case AudioFileTableColumn::crestFactor:
        case AudioFileTableColumn::dcOffset:
        case AudioFileTableColumn::clips:
        case AudioFileTableColumn::correlation:
        case AudioFileTableColumn::customGain:
            return CUSTOM_GAIN_COLUMN_DEFAULT_WIDTH;
        case AudioFileTableColumn::status:
            return STATUS_COLUMN_DEFAULT_WIDTH;
    }
    return 0;
}

/// Returns the compile-time lower drag limit, retaining a common minimum for numeric and status columns.
consteval int minimumColumnWidth(const AudioFileTableColumn column)
{
    switch (column) {
        case AudioFileTableColumn::name:
            return NAME_COLUMN_MINIMUM_WIDTH;
        case AudioFileTableColumn::folder:
        case AudioFileTableColumn::path:
            return PATH_COLUMN_MINIMUM_WIDTH;
        case AudioFileTableColumn::type:
            return TYPE_COLUMN_MINIMUM_WIDTH;
        case AudioFileTableColumn::bitDepth:
            return BIT_DEPTH_COLUMN_MINIMUM_WIDTH;
        case AudioFileTableColumn::bitrate:
            return BITRATE_COLUMN_MINIMUM_WIDTH;
        case AudioFileTableColumn::sampleRate:
            return SAMPLE_RATE_COLUMN_MINIMUM_WIDTH;
        case AudioFileTableColumn::length:
            return LENGTH_COLUMN_MINIMUM_WIDTH;
        case AudioFileTableColumn::overallPeak:
        case AudioFileTableColumn::peakLeft:
        case AudioFileTableColumn::peakRight:
        case AudioFileTableColumn::overallTruePeak:
        case AudioFileTableColumn::maxShortTermLufs:
        case AudioFileTableColumn::integratedLufs:
        case AudioFileTableColumn::plr:
        case AudioFileTableColumn::psr:
        case AudioFileTableColumn::crestFactor:
        case AudioFileTableColumn::dcOffset:
        case AudioFileTableColumn::clips:
        case AudioFileTableColumn::correlation:
        case AudioFileTableColumn::customGain:
        case AudioFileTableColumn::status:
            return METRIC_COLUMN_MINIMUM_WIDTH;
    }
    return 0;
}
}  // namespace table

/// Repairs nonfinite stored scales and bounds finite values without depending on the current window size.
inline double constrainedPreviewWidthScale(const double scale, const bool information)
{
    const auto fallback
        = information ? previewWidths::DEFAULT_INFORMATION_SCALE : previewWidths::DEFAULT_CONTROLS_SCALE;
    const auto minimum = information ? previewWidths::MINIMUM_INFORMATION_SCALE : previewWidths::MINIMUM_CONTROLS_SCALE;
    return std::isfinite(scale) ? juce::jlimit(minimum, previewWidths::MAXIMUM_SCALE, scale) : fallback;
}

/// Computes the original responsive column widths, preserving the established wide-layout breakpoint.
/// This unscaled baseline is also used to interpret the persisted width preferences at every window size.
inline PreviewColumns previewColumnBaseline(juce::Rectangle<int> bounds, const bool allowWide = true)
{
    bounds.setSize(juce::jmax(0, bounds.getWidth()), juce::jmax(0, bounds.getHeight()));
    const auto half = bounds.getWidth() / 2;
    const bool wide = allowWide && half >= widePreview::MINIMUM_HALF_WIDTH;
    auto controlsWidth = juce::jlimit(
        CONTROLS_MINIMUM_WIDTH, CONTROLS_MAXIMUM_WIDTH, static_cast<int>(bounds.getWidth() * CONTROLS_WIDTH_PROPORTION)
    );
    auto informationWidth = juce::jlimit(
        INFORMATION_MINIMUM_WIDTH,
        INFORMATION_MAXIMUM_WIDTH,
        static_cast<int>(bounds.getWidth() * INFORMATION_WIDTH_PROPORTION)
    );
    if (wide) {
        const auto shared = half - SECTION_SPACING;
        controlsWidth = juce::jmin(
            widePreview::CONTROLS_MAXIMUM_WIDTH, static_cast<int>(shared * widePreview::CONTROLS_PROPORTION)
        );
        informationWidth = juce::jmin(widePreview::INFORMATION_MAXIMUM_WIDTH, shared - controlsWidth);
    }
    return {
        .controls = bounds.withWidth(controlsWidth),
        .information = bounds.withWidth(informationWidth),
        .waveform = bounds,
        .wide = wide,
    };
}

/// Allocates independently scaled sidebars and gives the waveform the remaining horizontal space.
/// The existing gaps host dividers without using additional width. Very small layouts reserve the waveform first,
/// then proportionally constrain the sidebars without modifying their stored scales.
/// Disallowing wide mode selects the unchanged compact controls width and ignores both scales.
inline PreviewColumns previewColumns(
    juce::Rectangle<int> bounds,
    const bool allowWide = true,
    const double controlsScale = previewWidths::DEFAULT_CONTROLS_SCALE,
    const double informationScale = previewWidths::DEFAULT_INFORMATION_SCALE
)
{
    bounds.setSize(juce::jmax(0, bounds.getWidth()), juce::jmax(0, bounds.getHeight()));
    const auto baseline = previewColumnBaseline(bounds, allowWide);
    auto controlsWidth = baseline.controls.getWidth();
    auto informationWidth = baseline.information.getWidth();
    const auto gap = allowWide
        ? juce::jmin(SECTION_SPACING, juce::jmax(0, bounds.getWidth() - previewWidths::MINIMUM_WAVEFORM_WIDTH) / 2)
        : SECTION_SPACING;
    if (allowWide) {
        controlsWidth = juce::roundToInt(controlsWidth * constrainedPreviewWidthScale(controlsScale, false));
        informationWidth = juce::roundToInt(informationWidth * constrainedPreviewWidthScale(informationScale, true));
        const auto sidebarBudget
            = juce::jmax(0, bounds.getWidth() - previewWidths::MINIMUM_WAVEFORM_WIDTH - SECTION_SPACING * 2);
        const auto requestedWidth = controlsWidth + informationWidth;
        if (requestedWidth > sidebarBudget) {
            controlsWidth = static_cast<int>(static_cast<double>(controlsWidth) * sidebarBudget / requestedWidth);
            informationWidth = sidebarBudget - controlsWidth;
        }
    }
    const auto controls = bounds.removeFromLeft(juce::jmin(bounds.getWidth(), controlsWidth));
    bounds.removeFromLeft(juce::jmin(bounds.getWidth(), gap));
    const auto information = bounds.removeFromLeft(juce::jmin(bounds.getWidth(), informationWidth));
    bounds.removeFromLeft(juce::jmin(bounds.getWidth(), gap));
    return {.controls = controls, .information = information, .waveform = bounds, .wide = baseline.wide};
}

/// Returns the expanded workspace height that shows controls of the given content height without scrolling.
/// It adds the pinned playback row below the controls, the gap above that row,
/// and the vertical padding of the controls column and of the workspace.
constexpr int previewHeightForControls(const int contentHeight)
{
    return contentHeight + CONTROLS_SECTION_SPACING + PROCESS_BUTTON_HEIGHT + information::VERTICAL_PADDING * 2
        + compactPreview::PADDING * 2;
}

/// Expanded workspace heights that fit the one-column and the wide controls without scrolling.
constexpr int NARROW_PREVIEW_FIT_HEIGHT {previewHeightForControls(CONTROLS_CONTENT_HEIGHT)};
constexpr int WIDE_PREVIEW_FIT_HEIGHT {previewHeightForControls(WIDE_CONTROLS_CONTENT_HEIGHT)};
static_assert(
    WIDE_PREVIEW_FIT_HEIGHT >= PREVIEW_MINIMUM_HEIGHT && NARROW_PREVIEW_FIT_HEIGHT >= PREVIEW_MINIMUM_HEIGHT,
    "The controls-fit preview heights must not be below the preview minimum height"
);

/// Returns the height the expanded workspace starts at for a workspace of the given width,
/// so all of its controls fit without scrolling and without extra space below them.
/// The width decides the layout through the same breakpoint as previewColumns.
inline int expandedPreviewFitHeight(const int workspaceWidth)
{
    const auto workspace = juce::Rectangle<int> {juce::jmax(0, workspaceWidth), 0}.reduced(compactPreview::PADDING, 0);
    return previewColumns(workspace).wide ? WIDE_PREVIEW_FIT_HEIGHT : NARROW_PREVIEW_FIT_HEIGHT;
}

/// Clamps the expanded split without changing its remembered preference when a short window needs scrolling.
inline int constrainedPreviewHeight(const int preferredHeight, const int availableHeight)
{
    const auto minimum = static_cast<int>(PREVIEW_MINIMUM_HEIGHT);
    const auto maximum = juce::jmax(
        minimum,
        juce::jmin(
            juce::roundToInt(-PREVIEW_MAXIMUM_PROPORTION * availableHeight),
            availableHeight - static_cast<int>(RESULTS_TABLE_MINIMUM_HEIGHT + RESIZE_BAR_HEIGHT)
        )
    );
    return juce::jlimit(minimum, maximum, preferredHeight);
}

/// Centres the initial window within the display's usable area, reducing it on smaller screens.
inline juce::Rectangle<int> initialWindowBounds(const juce::Rectangle<int> usableArea)
{
    return usableArea.withSizeKeepingCentre(
        juce::jmin(DEFAULT_WINDOW_WIDTH, usableArea.getWidth()),
        juce::jmin(DEFAULT_WINDOW_HEIGHT, usableArea.getHeight())
    );
}
}  // namespace layout
}  // namespace audiobatch
