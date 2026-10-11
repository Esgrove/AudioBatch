/// Implementation of the main GUI component.
/// Covers UI construction and layout, table interaction and sorting, playback preview,
/// folder scanning and drag-and-drop, and wiring of the analysis, normalization, and plugin coordinators.
/// Also defines the file-local AudioInfoPanel and SecondarySortTableHeader helper components
/// and the record sort comparators used by the results table.

#include "AudioBatchComponent.h"

#include "AppInfo.h"
#include "AudioAnalysisService.h"
#include "CustomLookAndFeel.h"
#include "FileUtils.h"
#include "Log.h"
#include "PluginProcessingService.h"
#include "StringFormat.h"
#include <string_view>

#include <algorithm>
#include <cstdint>
#include <map>

using AudioInfoRow = std::pair<juce::String, juce::String>;

/// Compact two-column metadata panel displayed beside the waveform preview.
class AudioInfoPanel : public juce::Component
{
public:
    static constexpr int rowHeight = 20;
    static constexpr int verticalPadding = 8;
    static constexpr int horizontalPadding = 10;

    /// Opacity of the panel background fill.
    static constexpr float backgroundAlpha = 0.85f;
    /// Corner radius in pixels of the rounded panel background and outline.
    static constexpr float cornerRadius = 6.0f;
    /// Opacity of the white outline drawn around the panel.
    static constexpr float outlineAlpha = 0.08f;
    /// Opacity of the white value text in the right-hand column.
    static constexpr float valueTextAlpha = 0.95f;

    /// Removes all rows so the panel shows nothing and collapses to its minimum height.
    void clear()
    {
        setRows({});
    }

    /// Returns the height needed to show every row plus padding,
    /// used to size the panel inside its scrolling viewport.
    [[nodiscard]] int getPreferredHeight() const
    {
        const auto rowCount = static_cast<int>(rows.size());
        return verticalPadding * 2 + juce::jmax(0, rowCount) * rowHeight;
    }

    /// Replaces the displayed rows, reusing existing labels and hiding any leftover ones.
    /// Notifies the preferred-height callback so the parent can re-run its layout.
    void setRows(std::vector<AudioInfoRow> newRows)
    {
        rows = std::move(newRows);
        ensureLabelCount(rows.size());

        for (std::size_t index = 0; index < rows.size(); ++index) {
            fieldLabels[index]->setText(rows[index].first, juce::dontSendNotification);
            valueLabels[index]->setText(rows[index].second, juce::dontSendNotification);
            fieldLabels[index]->setVisible(true);
            valueLabels[index]->setVisible(true);
        }

        for (std::size_t index = rows.size(); index < fieldLabels.size(); ++index) {
            fieldLabels[index]->setVisible(false);
            valueLabels[index]->setVisible(false);
        }

        if (onPreferredHeightChanged) {
            onPreferredHeightChanged();
        }

        resized();
        repaint();
    }

    /// Registers a callback invoked whenever the preferred height changes,
    /// so the owner can resize the surrounding viewport.
    void setPreferredHeightChangedCallback(std::function<void()> callback)
    {
        onPreferredHeightChanged = std::move(callback);
    }

    /// Draws the rounded panel background with a subtle outline.
    void paint(juce::Graphics& graphics) override
    {
        const auto bounds = getLocalBounds().toFloat();
        graphics.setColour(juce::CustomLookAndFeel::greySemiDark().withAlpha(backgroundAlpha));
        graphics.fillRoundedRectangle(bounds, cornerRadius);

        graphics.setColour(juce::Colours::white.withAlpha(outlineAlpha));
        graphics.drawRoundedRectangle(bounds.reduced(0.5f), cornerRadius, 1.0f);
    }

    /// Positions the field and value labels in two columns, one fixed-height row per entry.
    void resized() override
    {
        auto bounds = getLocalBounds().reduced(horizontalPadding, verticalPadding);

        if (rows.empty()) {
            return;
        }

        const auto rowCount = static_cast<int>(rows.size());
        const auto fieldWidth = juce::roundToInt(static_cast<float>(bounds.getWidth()) * 0.55f);
        const auto fieldArea = bounds.removeFromLeft(fieldWidth);

        for (int row = 0; row < rowCount; ++row) {
            const auto fieldRow = juce::Rectangle(
                fieldArea.getX(), fieldArea.getY() + row * rowHeight, fieldArea.getWidth(), rowHeight
            );
            const auto valueRow
                = juce::Rectangle(bounds.getX(), bounds.getY() + row * rowHeight, bounds.getWidth(), rowHeight);

            fieldLabels[static_cast<std::size_t>(row)]->setBounds(fieldRow);
            valueLabels[static_cast<std::size_t>(row)]->setBounds(valueRow);
        }
    }

private:
    /// Grows the label pools until at least the required number of field and value label pairs exist.
    /// Labels are only ever added, never destroyed, so row updates stay cheap.
    void ensureLabelCount(const std::size_t requiredCount)
    {
        while (fieldLabels.size() < requiredCount) {
            auto fieldLabel = std::make_unique<juce::Label>();
            fieldLabel->setJustificationType(juce::Justification::centredLeft);
            fieldLabel->setColour(juce::Label::textColourId, juce::CustomLookAndFeel::greyMiddleLight());
            addAndMakeVisible(*fieldLabel);
            fieldLabels.push_back(std::move(fieldLabel));

            auto valueLabel = std::make_unique<juce::Label>();
            valueLabel->setJustificationType(juce::Justification::centredRight);
            valueLabel->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(valueTextAlpha));
            addAndMakeVisible(*valueLabel);
            valueLabels.push_back(std::move(valueLabel));
        }
    }

    std::vector<AudioInfoRow> rows;
    std::vector<std::unique_ptr<juce::Label>> fieldLabels;
    std::vector<std::unique_ptr<juce::Label>> valueLabels;
    std::function<void()> onPreferredHeightChanged;
};

/// Main-view constants and helper functions local to this translation unit.
namespace audiobatch::gui
{
constexpr auto supportedAudioFilePatterns = "*.wav;*.aif;*.aiff;*.flac;*.mp3";
constexpr int activityIndicatorTimerHz = 24;
constexpr int nameColumnMinimumWidth = AudioFileTableModel::minimumColumnWidth(AudioFileTableModel::columnName);
constexpr int pathColumnMinimumWidth = AudioFileTableModel::minimumColumnWidth(AudioFileTableModel::columnPath);

constexpr int nameColumnDefaultWidth = AudioFileTableModel::initialColumnWidth(AudioFileTableModel::columnName);
constexpr int pathColumnDefaultWidth = AudioFileTableModel::initialColumnWidth(AudioFileTableModel::columnPath);

/// Suffix that some decoders append to their format name, for example "WAV file".
/// It is stripped when the format name is used as the file type label.
constexpr auto formatNameFileSuffix = " file";

/// Pixel sizes, spacing, and timing used to lay out and configure the main view.
namespace layout
{
/// Initial width in pixels of the main component before the window is resized by the user.
constexpr int initialComponentWidth = 1024;
/// Initial height in pixels of the main component.
constexpr int initialComponentHeight = 800;

/// Height in pixels of the top bar holding the current folder and status labels.
constexpr int topBarHeight = 34;
/// Width in pixels reserved for the status label at the right end of the top bar.
constexpr int statusLabelWidth = 220;
/// Spacing in pixels between the main sections and between the stacked preview controls.
constexpr int sectionSpacing = 6;

/// Height in pixels of each row in the results table.
constexpr int resultsTableRowHeight = 24;

/// Minimum height in pixels of the results table in the vertical split.
constexpr double resultsTableMinimumHeight = 240.0;
/// Maximum share of the vertical split for the results table, negative to mark it as a proportion.
constexpr double resultsTableMaximumProportion = -0.85;
/// Preferred share of the vertical split for the results table, negative to mark it as a proportion.
constexpr double resultsTablePreferredProportion = -0.69;
/// Fixed height in pixels of the draggable bar between the results table and the preview area.
constexpr double resizeBarHeight = 6.0;
/// Minimum height in pixels of the waveform preview area in the vertical split.
constexpr double previewMinimumHeight = 180.0;
/// Maximum share of the vertical split for the preview area, negative to mark it as a proportion.
constexpr double previewMaximumProportion = -0.70;
/// Preferred share of the vertical split for the preview area, negative to mark it as a proportion.
constexpr double previewPreferredProportion = -0.31;

/// Height in pixels of the Process, Play, Audio Settings, and Plugins buttons.
constexpr int controlButtonHeight = 28;
/// Width in pixels of the square button that clears the per-file gain.
constexpr int gainClearButtonWidth = 28;
/// Height in pixels of the normalize-before-plugins toggle row.
constexpr int normalizeToggleHeight = 22;
/// Width in pixels of the zoom slider value text box.
constexpr int zoomTextBoxWidth = 50;

/// Width in pixels of the content component in child dialog windows.
constexpr int dialogContentWidth = 500;
/// Height in pixels of the content component in child dialog windows.
constexpr int dialogContentHeight = 150;
}  // namespace layout

/// Steps of the normalization target slider and the keys that persist the normalization settings.
/// The target ranges come from AudioNormalizationOptions, which the CLI validates against too.
namespace normalize_target
{
/// Step of the target sample peak, in dB.
constexpr double peakStepDecibels = 0.1;
/// Step of the target loudness, in LU.
constexpr double loudnessStepDecibels = 0.5;
/// Combo box item id of the peak mode, which must be non-zero.
constexpr int peakModeItemId = 1;
/// Combo box item id of the loudness mode.
constexpr int loudnessModeItemId = 2;
constexpr auto modeSettingKey = "normalizeMode";
constexpr auto targetPeakSettingKey = "normalizeTargetPeakDbfs";
constexpr auto targetLufsSettingKey = "normalizeTargetLufs";
}  // namespace normalize_target

/// Range of the waveform zoom slider in percent of the unzoomed view and its conversion to a scale factor.
namespace zoom_range
{
/// Smallest zoom level in percent, which is also the initial value and shows the whole file.
constexpr int minimumPercent = 100;
/// Largest zoom level in percent.
constexpr int maximumPercent = 1500;
/// Converts a zoom percentage into the scale factor the thumbnail expects.
constexpr double percentToScaleFactor = 0.01;
}  // namespace zoom_range

/// Range and step of the per-file gain slider in decibels.
namespace gain_range
{
/// Largest boost or cut in dB the gain slider allows in either direction.
constexpr double maximumDecibels = 12.0;
/// Step size in dB for the slider and for mouse wheel adjustments over the waveform.
constexpr double stepDecibels = 0.1;
}  // namespace gain_range

/// Delay in milliseconds before a tooltip appears when hovering over a control.
constexpr int tooltipDelayMilliseconds = 700;
/// Time in milliseconds to wait for the audio preview thread to stop during shutdown.
constexpr int previewThreadStopTimeoutMilliseconds = 2000;
/// Number of milliseconds in one second, used to report elapsed analysis time in seconds.
constexpr double millisecondsPerSecond = 1000.0;

enum FileMenuItemId : std::uint8_t {
    revealFileMenuItemId = 1,
    openParentDirectoryMenuItemId,
    normalizeMenuItemId,
    normalizeSupportMenuItemId,
    processMenuItemId,
    clearCustomGainMenuItemId,
    moveToTrashMenuItemId,
    removeFromListMenuItemId,
    reanalyzeMenuItemId,
};

/// Formats a bounded list of normalization failures for an alert dialog,
/// truncating with a "...and N more" line so the dialog stays readable.
static juce::String buildNormalizationFailureMessage(const juce::StringArray& failures)
{
    if (failures.isEmpty()) {
        return {};
    }

    juce::String message;
    const auto linesToShow = juce::jmin(6, failures.size());

    for (int index = 0; index < linesToShow; ++index) {
        message << utils::format("- {}", failures[index]) << juce::newLine;
    }

    if (failures.size() > linesToShow) {
        message << utils::format("- ...and {} more", failures.size() - linesToShow);
    }

    return message.trimEnd();
}

/// Returns the platform-appropriate label for the reveal-in-file-manager menu item.
static juce::String getRevealFileMenuLabel()
{
#if JUCE_MAC
    return "Reveal in Finder";
#elif JUCE_WINDOWS
    return "Reveal in Explorer";
#else
    return "Reveal in File Manager";
#endif
}

/// Derives a short display label for a record's file type from its extension,
/// falling back to the decoder's format name and finally to "Unknown".
static juce::String getRecordTypeLabel(const AudioAnalysisRecord& record)
{
    if (const auto extension = record.file.getFileExtension().trimCharactersAtStart("."); extension.isNotEmpty()) {
        return extension.toUpperCase();
    }

    auto formatName = record.formatName.trim();

    if (formatName.endsWithIgnoreCase(formatNameFileSuffix)) {
        formatName
            = formatName.dropLastCharacters(static_cast<int>(std::string_view {formatNameFileSuffix}.size())).trimEnd();
    }

    if (formatName.isNotEmpty()) {
        return formatName;
    }

    return "Unknown";
}

/// Returns the uppercase file extension for display, or "Unknown" when the file has none.
static juce::String formatLabelForFile(const juce::File& file)
{
    auto extension = file.getFileExtension().trimCharactersAtStart(".").toUpperCase();

    if (extension.isEmpty()) {
        extension = "Unknown";
    }

    return extension;
}

/// Table header that maps ctrl or cmd clicks to a secondary sort request
/// and renders the secondary sort indicator inside the column name text.
class SecondarySortTableHeader final : public juce::TableHeaderComponent
{
public:
    using SecondarySortCallback = std::function<void(int columnId)>;

    /// Stores the callback invoked when a column is clicked with ctrl or cmd held down.
    explicit SecondarySortTableHeader(SecondarySortCallback secondarySortRequestedCallback) :
        secondarySortRequested(std::move(secondarySortRequestedCallback))
    { }

    /// Routes ctrl or cmd clicks to the secondary sort callback,
    /// while plain clicks fall through to the normal primary sort behaviour.
    void columnClicked(const int columnId, const juce::ModifierKeys& modifiers) override
    {
        if ((modifiers.isCtrlDown() || modifiers.isCommandDown()) && secondarySortRequested != nullptr) {
            secondarySortRequested(columnId);
            return;
        }

        juce::TableHeaderComponent::columnClicked(columnId, modifiers);
    }

    /// Appends a textual secondary sort marker to the given column's name
    /// and restores the base name of every other column.
    /// Passing a column id of zero clears the indicator entirely.
    void setSecondarySortIndicator(const int columnId, const bool isForwards)
    {
        if (baseColumnNames.empty()) {
            cacheBaseColumnNames();
        }

        secondarySortColumnId = columnId;
        secondarySortForwards = isForwards;

        for (const auto& [trackedColumnId, baseName] : baseColumnNames) {
            auto displayName = baseName;

            if (trackedColumnId == secondarySortColumnId) {
                displayName = utils::format("{} {}", displayName, secondarySortForwards ? "[2^]" : "[2v]");
            }

            setColumnName(trackedColumnId, displayName);
        }

        repaint();
    }

private:
    /// Captures the original column names once,
    /// so indicator suffixes can be re-applied without accumulating in the names.
    void cacheBaseColumnNames()
    {
        const auto columnCount = getNumColumns(false);

        for (int index = 0; index < columnCount; ++index) {
            const auto columnId = getColumnIdOfIndex(index, false);
            baseColumnNames.emplace(columnId, getColumnName(columnId));
        }
    }

    SecondarySortCallback secondarySortRequested;
    std::map<int, juce::String> baseColumnNames;
    int secondarySortColumnId = 0;
    bool secondarySortForwards = true;
};

/// Returns the table's header downcast to SecondarySortTableHeader.
/// The table must have been created with that header type, which the constructor guarantees.
static SecondarySortTableHeader& getSecondarySortHeader(juce::TableListBox const& table)
{
    return dynamic_cast<SecondarySortTableHeader&>(table.getHeader());
}

/// Three-way natural-order string comparison shared by the column comparators.
static int compareNaturalStrings(const juce::String& left, const juce::String& right)
{
    return left.compareNatural(right);
}

/// Three-way comparison of peak values by magnitude, ignoring polarity.
template<typename Value>
int comparePeaks(Value left, Value right)
{
    const auto leftMagnitude = std::abs(left);
    const auto rightMagnitude = std::abs(right);

    if (juce::approximatelyEqual(leftMagnitude, rightMagnitude)) {
        return 0;
    }

    return leftMagnitude < rightMagnitude ? -1 : 1;
}

/// Three-way comparison by average bitrate, treating nearly equal bitrates as equal.
static int compareBitrates(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    const auto leftBitrate = AudioAnalysisService::getAverageBitrateKbps(lhs);
    const auto rightBitrate = AudioAnalysisService::getAverageBitrateKbps(rhs);

    if (juce::approximatelyEqual(leftBitrate, rightBitrate)) {
        return 0;
    }

    return leftBitrate < rightBitrate ? -1 : 1;
}

/// Three-way comparison by sample rate.
static int compareSampleRates(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    if (lhs.sampleRate == rhs.sampleRate) {
        return 0;
    }

    return lhs.sampleRate < rhs.sampleRate ? -1 : 1;
}

/// Three-way loudness comparison that sorts negative-infinity readings below every measurable value.
static int compareLoudness(const double left, const double right)
{
    const auto leftIsNegativeInfinity = left <= AudioAnalysisRecord::negativeInfinityLoudness;
    const auto rightIsNegativeInfinity = right <= AudioAnalysisRecord::negativeInfinityLoudness;

    if (leftIsNegativeInfinity && rightIsNegativeInfinity) {
        return 0;
    }

    if (leftIsNegativeInfinity != rightIsNegativeInfinity) {
        return leftIsNegativeInfinity ? -1 : 1;
    }

    if (juce::approximatelyEqual(left, right)) {
        return 0;
    }

    return left < right ? -1 : 1;
}

/// Dispatches to the three-way comparator matching the given table column,
/// defaulting to overall peak for unknown column ids.
static int compareRecordsByColumn(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs, const int columnId)
{
    switch (columnId) {
        case AudioFileTableModel::columnName:
            return compareNaturalStrings(lhs.fileName, rhs.fileName);

        case AudioFileTableModel::columnPath:
            return compareNaturalStrings(lhs.fullPath, rhs.fullPath);

        case AudioFileTableModel::columnType:
            return compareNaturalStrings(getRecordTypeLabel(lhs), getRecordTypeLabel(rhs));

        case AudioFileTableModel::columnBitrate:
            return compareBitrates(lhs, rhs);

        case AudioFileTableModel::columnSampleRate:
            return compareSampleRates(lhs, rhs);

        case AudioFileTableModel::columnPeakLeft:
            return comparePeaks(lhs.peakLeft, rhs.peakLeft);

        case AudioFileTableModel::columnPeakRight:
            return comparePeaks(lhs.peakRight, rhs.peakRight);

        case AudioFileTableModel::columnOverallTruePeak:
            return comparePeaks(lhs.overallTruePeak, rhs.overallTruePeak);

        case AudioFileTableModel::columnMaxShortTermLufs:
            return compareLoudness(lhs.maxShortTermLufs, rhs.maxShortTermLufs);

        case AudioFileTableModel::columnIntegratedLufs:
            return compareLoudness(lhs.integratedLufs, rhs.integratedLufs);

        case AudioFileTableModel::columnCustomGain: {
            if (lhs.hasCustomGain != rhs.hasCustomGain) {
                return lhs.hasCustomGain ? 1 : -1;
            }
            if (!lhs.hasCustomGain) {
                return 0;
            }
            if (juce::approximatelyEqual(lhs.customGainDb, rhs.customGainDb)) {
                return 0;
            }
            return lhs.customGainDb < rhs.customGainDb ? -1 : 1;
        }

        case AudioFileTableModel::columnStatus:
            return compareNaturalStrings(
                AudioAnalysisService::formatStatus(lhs), AudioAnalysisService::formatStatus(rhs)
            );

        case AudioFileTableModel::columnOverallPeak:
        default:
            return comparePeaks(lhs.overallPeak, rhs.overallPeak);
    }
}
}  // namespace audiobatch::gui

using namespace audiobatch::gui;

/// Plugin chains created for one processing run, or the reason the run cannot start.
struct ChainInstantiation {
    /// One chain of plugin instances per worker.
    std::vector<PluginProcessingCoordinator::PluginChainInstances> chains;
    /// Empty when every plugin of every chain was created.
    juce::String errorMessage;
    /// Name of the plugin that failed to load, when errorMessage is not empty.
    juce::String failedPluginName;
};

/// Creates one chain of plugin instances per worker on the calling thread, which must be the message thread.
/// Stops at the first plugin that fails, since a chain missing any plugin must not run.
static ChainInstantiation instantiateChains(
    const juce::AudioPluginFormatManager& pluginFormatManager,
    const std::vector<PluginChain::EnabledChainPlugin>& enabledPlugins,
    const int workerCount
)
{
    // Use a reasonable default sample rate. The chain is prepared again per file with the file's rate.
    constexpr double defaultSampleRate = 48000.0;
    constexpr int defaultBlockSize = 1024;

    ChainInstantiation instantiation;
    instantiation.chains.reserve(static_cast<std::size_t>(workerCount));

    for (int worker = 0; worker < workerCount; ++worker) {
        PluginProcessingCoordinator::PluginChainInstances chainInstances;
        chainInstances.reserve(enabledPlugins.size());

        for (const auto& enabledPlugin : enabledPlugins) {
            juce::String error;
            auto instance = pluginFormatManager.createPluginInstance(
                enabledPlugin.description, defaultSampleRate, defaultBlockSize, error
            );

            if (instance == nullptr) {
                instantiation.errorMessage = error.isNotEmpty() ? error : juce::String("Plugin instantiation failed");
                instantiation.failedPluginName = enabledPlugin.description.name;
                return instantiation;
            }

            chainInstances.push_back(std::move(instance));
        }

        instantiation.chains.push_back(std::move(chainInstances));
    }

    return instantiation;
}

void AudioBatchComponent::configureResultsTable()
{
    resultsTable.setHeader(std::make_unique<SecondarySortTableHeader>([this](const int columnId) {
        handleSecondarySortRequested(columnId);
    }));
    AudioFileTableModel::configureHeader(resultsTable.getHeader());
    resultsTable.setModel(&fileTableModel);
    resultsTable.setColour(juce::ListBox::backgroundColourId, juce::CustomLookAndFeel::greyMediumDark());
    resultsTable.setMultipleSelectionEnabled(true);
    resultsTable.setOutlineThickness(0);
    resultsTable.setRowHeight(layout::resultsTableRowHeight);
    resultsTable.getHeader().setSortColumnId(currentSortColumnId, currentSortForwards);
    refreshSortIndicators();
    addAndMakeVisible(resultsTable);
    resultsTable.addKeyListener(this);
}

void AudioBatchComponent::configurePreviewControls()
{
    // Default to a larger list view while allowing the lower preview area to be resized.
    mainVerticalLayout.setItemLayout(
        0,
        layout::resultsTableMinimumHeight,
        layout::resultsTableMaximumProportion,
        layout::resultsTablePreferredProportion
    );
    mainVerticalLayout.setItemLayout(1, layout::resizeBarHeight, layout::resizeBarHeight, layout::resizeBarHeight);
    mainVerticalLayout.setItemLayout(
        2, layout::previewMinimumHeight, layout::previewMaximumProportion, layout::previewPreferredProportion
    );
    addAndMakeVisible(waveformResizeBar);

    thumbnail = std::make_unique<ThumbnailComponent>(formatManager, transportSource);
    addAndMakeVisible(thumbnail.get());
    thumbnail->addChangeListener(this);
    transportSource.addChangeListener(this);
    thumbnail->setMouseWheelZoomCallback([this](const double zoomFactor) {
        zoomSlider.setValue(zoomSlider.getValue() * zoomFactor);
    });
    thumbnail->setMouseWheelGainCallback([this](const float deltaDb) {
        if (!gainSlider.isEnabled() || juce::approximatelyEqual(deltaDb, 0.0f)) {
            return;
        }
        const auto interval = gainSlider.getInterval() > 0.0 ? gainSlider.getInterval() : gain_range::stepDecibels;
        const auto step = (deltaDb > 0.0f ? 1.0 : -1.0) * interval;
        const auto newValue
            = juce::jlimit(gainSlider.getMinimum(), gainSlider.getMaximum(), gainSlider.getValue() + step);
        gainSlider.setValue(newValue, juce::sendNotificationSync);
    });
    thumbnail->setThumbnailFullyLoadedCallback([this] { handleThumbnailFullyLoaded(); });

    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium());
    startStopButton.setColour(juce::TextButton::buttonOnColourId, juce::CustomLookAndFeel::green());
    startStopButton.setTooltip("Start or stop preview playback for the selected file.");
    startStopButton.onClick = [this] { startOrStop(); };
    addAndMakeVisible(startStopButton);

    zoomSlider.setRange(zoom_range::minimumPercent, zoom_range::maximumPercent, 1);
    zoomSlider.setNumDecimalPlacesToDisplay(0);
    zoomSlider.setSkewFactor(0.5);
    zoomSlider.setValue(zoom_range::minimumPercent);
    zoomSlider.setTextValueSuffix("%");
    zoomSlider.setTooltip("Adjust waveform zoom. You can also use the mouse wheel over the waveform.");
    zoomSlider.onValueChange = [this] { zoomLevelChanged(zoomSlider.getValue() * zoom_range::percentToScaleFactor); };
    addAndMakeVisible(zoomSlider);

    zoomLabel.setText("Zoom", juce::dontSendNotification);
    zoomLabel.attachToComponent(&zoomSlider, true);
    zoomSlider.setTextBoxStyle(
        juce::Slider::TextBoxLeft, false, layout::zoomTextBoxWidth, zoomSlider.getTextBoxHeight()
    );
    addAndMakeVisible(zoomLabel);

    audioInfo = std::make_unique<AudioInfoPanel>();
    audioInfoViewport.setViewedComponent(audioInfo.get(), false);
    audioInfoViewport.setScrollBarsShown(true, false);
    audioInfoViewport.setScrollBarThickness(8);
    addAndMakeVisible(audioInfoViewport);

    audioInfo->setPreferredHeightChangedCallback([this] { resized(); });
}

void AudioBatchComponent::configureProcessingControls()
{
    pluginAppProperties.setStorageParameters(utils::createApplicationSettingsOptions());

    pluginChain = std::make_unique<PluginChain>(pluginAppProperties);
    pluginChain->setChainChangedCallback([this] { updateProcessButtonState(); });

    pluginButton.setTooltip("Edit the plugin chain, add plugins, or scan for plugins.");
    pluginButton.onClick = [this] {
        if (pluginChain != nullptr) {
            pluginChain->showMenu(pluginButton);
        }
    };
    addAndMakeVisible(pluginButton);

    gainLabel.setText("Gain", juce::dontSendNotification);
    gainLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(gainLabel);

    gainSlider.setRange(-gain_range::maximumDecibels, gain_range::maximumDecibels, gain_range::stepDecibels);
    gainSlider.setValue(0.0, juce::dontSendNotification);
    gainSlider.setNumDecimalPlacesToDisplay(1);
    gainSlider.setTextValueSuffix(" dB");
    gainSlider.setTooltip("Per-file gain in dB. Non-zero values override normalization for the file.");
    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, gainSlider.getTextBoxHeight());
    gainSlider.setDoubleClickReturnValue(true, 0.0);
    gainSlider.setEnabled(false);
    gainSlider.onValueChange = [this] {
        const auto value = static_cast<float>(gainSlider.getValue());
        applyCustomGainToSelection(value, !juce::approximatelyEqual(value, 0.0f));
    };
    addAndMakeVisible(gainSlider);

    gainClearButton.setTooltip("Clear per-file gain for the current selection.");
    gainClearButton.setEnabled(false);
    gainClearButton.onClick = [this] {
        gainSlider.setValue(0.0, juce::dontSendNotification);
        applyCustomGainToSelection(0.0f, false);
    };
    addAndMakeVisible(gainClearButton);

    normalizeBeforePluginToggle.setTooltip(
        "When enabled, files without a custom gain are peak-normalized before the plugin chain."
    );
    addAndMakeVisible(normalizeBeforePluginToggle);

    forceOverwriteToggle.setTooltip(
        "When enabled, normalizing or processing replaces an existing output file with the same name. "
        "The replaced file is moved to the system trash."
    );
    addAndMakeVisible(forceOverwriteToggle);

    normalizeModeBox.addItem("Peak", normalize_target::peakModeItemId);
    normalizeModeBox.addItem("Loudness", normalize_target::loudnessModeItemId);
    normalizeModeBox.setTooltip(
        "Normalize to a sample peak level, or to an integrated loudness limited to a -0.3 dBTP true peak."
    );
    normalizeModeBox.onChange = [this] {
        updateNormalizeTargetSlider();
        saveNormalizationSettings();
    };
    addAndMakeVisible(normalizeModeBox);

    normalizeTargetSlider.setSliderStyle(juce::Slider::LinearBar);
    normalizeTargetSlider.setNumDecimalPlacesToDisplay(1);
    normalizeTargetSlider.setTooltip("Target level for normalization.");
    normalizeTargetSlider.onValueChange = [this] {
        if (normalizeModeBox.getSelectedId() == normalize_target::loudnessModeItemId) {
            normalizeTargetLufs = normalizeTargetSlider.getValue();
        } else {
            normalizeTargetPeakDbfs = normalizeTargetSlider.getValue();
        }
        saveNormalizationSettings();
    };
    addAndMakeVisible(normalizeTargetSlider);
    loadNormalizationSettings();

    processButton.setTooltip("Process selected files through the plugin chain.");
    updateProcessButtonState();
    processButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium());
    processButton.onClick = [this] { processSelectedRecords(); };
    addAndMakeVisible(processButton);

    settingsButton.setTooltip("Open audio device settings.");
    settingsButton.onClick = [this] { showAudioSettingsWindow(); };
    addAndMakeVisible(settingsButton);
}

void AudioBatchComponent::connectCoordinatorCallbacks()
{
    const SafePointer safeThis(this);

    // Coordinator callbacks arrive on worker threads.
    // The handler runs on the message thread, and only while this component still exists.
    const auto postToMessageThread = [safeThis](auto handler) {
        return [safeThis, handler](const auto&... arguments) {
            juce::MessageManager::callAsync([safeThis, handler, arguments...] {
                if (safeThis != nullptr) {
                    (safeThis.getComponent()->*handler)(arguments...);
                }
            });
        };
    };

    analysisCoordinator.setPlanCallback(postToMessageThread(&AudioBatchComponent::handleAnalysisPlan));
    analysisCoordinator.setResultCallback(postToMessageThread(&AudioBatchComponent::handleAnalysisResult));
    analysisCoordinator.setCompletionCallback(postToMessageThread(&AudioBatchComponent::handleAnalysisComplete));
    analysisCoordinator.setStartingCallback([safeThis](const juce::File& file, const int runId) {
        juce::MessageManager::callAsync([safeThis, file, runId] {
            if (safeThis != nullptr && runId == safeThis->analysisRunId) {
                safeThis->activeFileStatusLabels[file.getFullPathName()] = "Analyzing";
                safeThis->resultsTable.repaint();
            }
        });
    });

    normalizeCoordinator.setResultCallback(postToMessageThread(&AudioBatchComponent::handleNormalizeResult));
    normalizeCoordinator.setCompletionCallback(postToMessageThread(&AudioBatchComponent::handleNormalizeComplete));

    pluginCoordinator.setResultCallback(postToMessageThread(&AudioBatchComponent::handleProcessingResult));
    pluginCoordinator.setCompletionCallback(postToMessageThread(&AudioBatchComponent::handleProcessingComplete));

    pluginCoordinator.setStartErrorCallback([safeThis](const juce::String& errorMessage) {
        juce::MessageManager::callAsync([safeThis, errorMessage] {
            if (safeThis != nullptr) {
                safeThis->pluginProcessingInProgress = false;
                safeThis->statusLabel.setText(errorMessage, juce::dontSendNotification);
                safeThis->showPluginProcessingWarning(errorMessage);
            }
        });
    });
}

void AudioBatchComponent::requestAudioDeviceAccess()
{
    utils::logDebug("AudioBatchComponent startup: requesting record-audio permission");
    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [this](const bool granted) {
        const int numInputChannels = granted ? 2 : 0;
        const auto audioInitialiseStartedAtMs = juce::Time::getMillisecondCounterHiRes();

        utils::logDebug(
            "AudioBatchComponent startup: audio permission callback granted={}", granted ? "true" : "false"
        );
        utils::logDebug(
            "AudioBatchComponent startup: audio device init begin (inputs={}, outputs=2)", numInputChannels
        );

        const auto initialiseError = audioDeviceManager.initialise(numInputChannels, 2, nullptr, true, {}, nullptr);
        const auto audioInitialiseElapsedMs = juce::Time::getMillisecondCounterHiRes() - audioInitialiseStartedAtMs;

        if (initialiseError.isEmpty()) {
            utils::logDebug(
                "AudioBatchComponent startup: audio device init succeeded ({:.1f} ms)", audioInitialiseElapsedMs
            );
        } else {
            utils::logError(
                "AudioBatchComponent startup: audio device init failed ({:.1f} ms): {}",
                audioInitialiseElapsedMs,
                initialiseError
            );
        }
    });
}

AudioBatchComponent::AudioBatchComponent() :
    analysisCoordinator(analysisCache),
    fileTableModel(
        analysisResults,
        [this](const int row) { handleSelectionChanged(row); },
        [this](const int columnId, const bool isForwards) { handleSortRequested(columnId, isForwards); },
        [this](const int row, const int columnId, const juce::MouseEvent& event) {
            handleFileContextMenuRequested(row, columnId, event);
        },
        [this](const AudioAnalysisRecord& record) { return getActiveStatusLabel(record); },
        [this](const AudioAnalysisRecord& record) { return getOperationMessage(record); },
        [] { return getActivityPhase(); }
    ),
    audioSetupComponent(audioDeviceManager, 0, 0, 0, 2, false, false, true, false)
{
    const auto constructorStartedAtMs = juce::Time::getMillisecondCounterHiRes();
    const auto logStartupCheckpoint = [constructorStartedAtMs](const juce::String& step) {
        utils::logDebug(
            "AudioBatchComponent startup: {} ({:.1f} ms)",
            step,
            juce::Time::getMillisecondCounterHiRes() - constructorStartedAtMs
        );
    };

    logStartupCheckpoint("constructor begin");
    formatManager.registerBasicFormats();
    logStartupCheckpoint("audio formats registered");

    currentRootLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(currentRootLabel);

    statusLabel.setTooltip("Background analysis and normalization status.");
    statusLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(statusLabel);

    configureResultsTable();
    configurePreviewControls();
    configureProcessingControls();

    tooltipWindow = std::make_unique<juce::TooltipWindow>(this, tooltipDelayMilliseconds);
    logStartupCheckpoint("ui controls created");

    thread.startThread(juce::Thread::Priority::high);
    logStartupCheckpoint("audio preview thread started");

    utils::logDebug("AudioBatchComponent startup: opening analysis cache");
    const auto analysisCacheOpened = analysisCache.open();
    if (analysisCacheOpened) {
        logStartupCheckpoint("analysis cache opened");

        // Checking every cached path touches the file system, so it runs in the background.
        cachePruneThread = std::jthread([this](const std::stop_token& stopToken) {
            analysisCache.pruneMissingFiles([&stopToken] { return stopToken.stop_requested(); });
        });
    } else {
        utils::logError("AudioBatchComponent startup: analysis cache open failed");
    }

    connectCoordinatorCallbacks();
    logStartupCheckpoint("analysis and normalization callbacks configured");

    requestAudioDeviceAccess();

    audioDeviceManager.addAudioCallback(&audioSourcePlayer);
    audioSourcePlayer.setSource(&transportSource);
    logStartupCheckpoint("audio callback connected");

    setOpaque(true);
    setSize(layout::initialComponentWidth, layout::initialComponentHeight);
    setWantsKeyboardFocus(true);
    logStartupCheckpoint("constructor complete");
}

AudioBatchComponent::~AudioBatchComponent()
{
    analysisCoordinator.cancelAndWait();
    normalizeCoordinator.cancelAndWait();
    pluginCoordinator.cancelAndWait();

    stopTimer();
    transportSource.stop();
    transportSource.setSource(nullptr);
    audioSourcePlayer.setSource(nullptr);

    audioDeviceManager.removeAudioCallback(&audioSourcePlayer);

    transportSource.removeChangeListener(this);
    thumbnail->removeChangeListener(this);
    thumbnail->setMouseWheelZoomCallback({});
    thumbnail->setMouseWheelGainCallback({});
    thumbnail->setThumbnailFullyLoadedCallback({});
    resultsTable.setModel(nullptr);

    if (childWindows.size() != 0) {
        for (auto& window : childWindows) {
            window.deleteAndZero();
        }
        childWindows.clear();
    }

    thread.stopThread(previewThreadStopTimeoutMilliseconds);
}

void AudioBatchComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

bool AudioBatchComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    return !files.isEmpty();
}

void AudioBatchComponent::filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/)
{
    handleDroppedPaths(files);
}

void AudioBatchComponent::resized()
{
    auto bounds = getLocalBounds().reduced(4);
    auto topBar = bounds.removeFromTop(layout::topBarHeight);

    statusLabel.setBounds(topBar.removeFromRight(layout::statusLabelWidth));
    topBar.removeFromRight(layout::sectionSpacing);
    currentRootLabel.setBounds(topBar);

    bounds.removeFromTop(layout::sectionSpacing);

    std::array<juce::Component*, 3> verticalSections {&resultsTable, &waveformResizeBar, thumbnail.get()};
    mainVerticalLayout.layOutComponents(
        verticalSections.data(), 3, bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), true, true
    );
    updateResultsTableColumnWidths();

    auto previewArea = thumbnail->getBounds();

    const auto space = juce::jmin(280, juce::jmax(180, juce::roundToIntAccurate(bounds.getWidth() * 0.22)));
    auto control = previewArea.removeFromLeft(space);

    const auto buttonGap = layout::sectionSpacing;

    // Bottom row: Process and Play/Stop.
    auto buttonsBottom = control.removeFromBottom(layout::controlButtonHeight);
    const auto bottomButtonWidth = (buttonsBottom.getWidth() - buttonGap) / 2;
    processButton.setBounds(buttonsBottom.removeFromLeft(bottomButtonWidth));
    buttonsBottom.removeFromLeft(buttonGap);
    startStopButton.setBounds(buttonsBottom);

    control.removeFromBottom(4);

    // Row above: Audio Settings and Plugins.
    auto buttonsTop = control.removeFromBottom(layout::controlButtonHeight);
    const auto topButtonWidth = (buttonsTop.getWidth() - buttonGap) / 2;
    settingsButton.setBounds(buttonsTop.removeFromLeft(topButtonWidth));
    buttonsTop.removeFromLeft(buttonGap);
    pluginButton.setBounds(buttonsTop);

    control.removeFromBottom(8);

    // The normalize and force overwrite toggles share a row above the buttons.
    auto toggleRow = control.removeFromBottom(layout::normalizeToggleHeight);
    normalizeBeforePluginToggle.setBounds(toggleRow.removeFromLeft((toggleRow.getWidth() - buttonGap) / 2));
    toggleRow.removeFromLeft(buttonGap);
    forceOverwriteToggle.setBounds(toggleRow);

    control.removeFromBottom(layout::sectionSpacing);

    // The normalization mode and target share the row above the toggles.
    auto targetRow = control.removeFromBottom(layout::normalizeToggleHeight);
    normalizeModeBox.setBounds(targetRow.removeFromLeft((targetRow.getWidth() - buttonGap) / 2));
    targetRow.removeFromLeft(buttonGap);
    normalizeTargetSlider.setBounds(targetRow);

    control.removeFromBottom(layout::sectionSpacing);

    zoomSlider.setBounds(control.removeFromBottom(zoomSlider.getTextBoxHeight()));

    control.removeFromBottom(layout::sectionSpacing);

    // Gain row: inline label, slider, then the clear button on the right.
    auto gainRow = control.removeFromBottom(gainSlider.getTextBoxHeight());
    gainClearButton.setBounds(gainRow.removeFromRight(layout::gainClearButtonWidth));
    gainRow.removeFromRight(4);
    const auto gainLabelWidth = 36;
    gainLabel.setBounds(gainRow.removeFromLeft(gainLabelWidth));
    gainSlider.setBounds(gainRow);

    control.removeFromBottom(8);

    audioInfoViewport.setBounds(control);
    const auto viewportWidth = audioInfoViewport.getMaximumVisibleWidth();
    const auto preferredHeight = audioInfo->getPreferredHeight();
    const auto viewportHeight = audioInfoViewport.getMaximumVisibleHeight();
    audioInfo->setSize(viewportWidth, juce::jmax(viewportHeight, preferredHeight));

    previewArea.removeFromLeft(layout::sectionSpacing);

    thumbnail->setBounds(previewArea);
}

void AudioBatchComponent::updateResultsTableColumnWidths() const
{
    auto& header = resultsTable.getHeader();

    const auto fixedColumnWidth = header.getColumnWidth(AudioFileTableModel::columnOverallPeak)
        + (header.isColumnVisible(AudioFileTableModel::columnType)
               ? header.getColumnWidth(AudioFileTableModel::columnType)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnBitrate)
               ? header.getColumnWidth(AudioFileTableModel::columnBitrate)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnSampleRate)
               ? header.getColumnWidth(AudioFileTableModel::columnSampleRate)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnPeakLeft)
               ? header.getColumnWidth(AudioFileTableModel::columnPeakLeft)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnPeakRight)
               ? header.getColumnWidth(AudioFileTableModel::columnPeakRight)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnOverallTruePeak)
               ? header.getColumnWidth(AudioFileTableModel::columnOverallTruePeak)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnMaxShortTermLufs)
               ? header.getColumnWidth(AudioFileTableModel::columnMaxShortTermLufs)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnIntegratedLufs)
               ? header.getColumnWidth(AudioFileTableModel::columnIntegratedLufs)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnCustomGain)
               ? header.getColumnWidth(AudioFileTableModel::columnCustomGain)
               : 0)
        + (header.isColumnVisible(AudioFileTableModel::columnStatus)
               ? header.getColumnWidth(AudioFileTableModel::columnStatus)
               : 0);

    const auto availableFlexibleWidth = resultsTable.getVisibleContentWidth() - fixedColumnWidth;
    const auto currentNameWidth = header.getColumnWidth(AudioFileTableModel::columnName);
    const auto currentPathWidth = header.getColumnWidth(AudioFileTableModel::columnPath);
    const auto currentFlexibleWidth = currentNameWidth + currentPathWidth;
    const auto defaultFlexibleWidth = nameColumnDefaultWidth + pathColumnDefaultWidth;
    const auto widthRatio = static_cast<double>(currentFlexibleWidth > 0 ? currentNameWidth : nameColumnDefaultWidth)
        / static_cast<double>(currentFlexibleWidth > 0 ? currentFlexibleWidth : defaultFlexibleWidth);

    int nameWidth = nameColumnMinimumWidth;
    int pathWidth = pathColumnMinimumWidth;

    if (availableFlexibleWidth >= nameColumnMinimumWidth + pathColumnMinimumWidth) {
        nameWidth = juce::roundToInt(static_cast<double>(availableFlexibleWidth) * widthRatio);
        nameWidth = juce::jlimit(nameColumnMinimumWidth, availableFlexibleWidth - pathColumnMinimumWidth, nameWidth);
        pathWidth = availableFlexibleWidth - nameWidth;
    }

    header.setColumnWidth(AudioFileTableModel::columnName, nameWidth);
    header.setColumnWidth(AudioFileTableModel::columnPath, pathWidth);
}

bool AudioBatchComponent::keyPressed(const juce::KeyPress& key, juce::Component* /*originator*/)
{
    return keyPressed(key);
}

bool AudioBatchComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::backspaceKey && key.getModifiers().isCtrlDown()) {
        moveSelectedRecordsToTrash(!key.getModifiers().isShiftDown());
        return true;
    }

    if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey) {
        removeSelectedRecords();
        return true;
    }

    if (key == juce::KeyPress('r', juce::ModifierKeys::commandModifier, 0)) {
        const auto selectedRows = resultsTable.getSelectedRows();
        if (const auto row = getSelectionDisplayRow(selectedRows, -1); row >= 0) {
            revealRecordInFileManager(row);
        }
        return true;
    }

    if (key == juce::KeyPress::spaceKey) {
        startOrStop();
        return true;
    }

    if (key == juce::KeyPress::F5Key) {
        refreshAnalysis(true);
        return true;
    }

    return false;
}

void AudioBatchComponent::chooseRootFolder()
{
    launchFolderChooser(false);
}

void AudioBatchComponent::addRootFolder()
{
    launchFolderChooser(true);
}

void AudioBatchComponent::launchFolderChooser(const bool appendToRoots)
{
    const auto lastRoot = currentRoots.isEmpty() ? juce::File() : currentRoots.getLast();
    const auto startDirectory = lastRoot.isDirectory() ? lastRoot : getDefaultBrowseDirectory();
    directoryChooser = std::make_unique<juce::FileChooser>(
        appendToRoots ? "Add Audio Folder" : "Choose Audio Folder", startDirectory, supportedAudioFilePatterns, true
    );

    directoryChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis = SafePointer(this), appendToRoots](const juce::FileChooser& chooser) {
            if (safeThis == nullptr) {
                return;
            }

            if (const auto selectedFolder = chooser.getResult(); selectedFolder.isDirectory()) {
                utils::logInfo(
                    "{} directory: {}", appendToRoots ? "Added" : "Opened", selectedFolder.getFullPathName().quoted()
                );

                if (appendToRoots) {
                    safeThis->addRoots({selectedFolder});
                } else {
                    safeThis->setRoots({selectedFolder});
                }
            }

            safeThis->directoryChooser.reset();
        }
    );
}

void AudioBatchComponent::setRoots(const juce::Array<juce::File>& folders)
{
    currentRoots.clear();

    for (const auto& folder : folders) {
        // Keep only the outermost folders, so no file is collected twice.
        const auto isInsideOtherFolder
            = std::ranges::any_of(folders, [&folder](const juce::File& other) { return folder.isAChildOf(other); });

        if (!isInsideOtherFolder) {
            currentRoots.addIfNotAlreadyThere(folder);
        }
    }

    updateCurrentRootLabel();
    refreshAnalysis(false);
}

void AudioBatchComponent::addRoots(const juce::Array<juce::File>& folders)
{
    bool rootsChanged = false;

    for (const auto& folder : folders) {
        const auto alreadyIncluded = std::ranges::any_of(currentRoots, [&folder](const juce::File& root) {
            return folder == root || folder.isAChildOf(root);
        });

        if (alreadyIncluded) {
            utils::logInfo("Folder is already included in the roots: {}", folder.getFullPathName().quoted());
            continue;
        }

        // A new parent folder absorbs the existing roots inside it.
        currentRoots.removeIf([&folder](const juce::File& root) { return root.isAChildOf(folder); });
        currentRoots.add(folder);
        rootsChanged = true;
    }

    if (!rootsChanged) {
        statusLabel.setText("Folder is already included", juce::dontSendNotification);
        return;
    }

    updateCurrentRootLabel();

    // Re-collect every root instead of only the new folder.
    // Rows already in the table and cached files are skipped quickly,
    // and an analysis run that is still in progress gets restarted with all of its files included.
    juce::Array<juce::File> existingRoots;
    for (const auto& root : currentRoots) {
        if (root.isDirectory()) {
            existingRoots.add(root);
        }
    }

    startAnalysis(existingRoots, true, false, false);
}

void AudioBatchComponent::updateCurrentRootLabel()
{
    if (currentRoots.isEmpty()) {
        currentRootLabel.setText("", juce::dontSendNotification);
        currentRootLabel.setTooltip("");
        return;
    }

    const auto firstPath = currentRoots.getFirst().getFullPathName();
    const auto labelText
        = currentRoots.size() == 1 ? firstPath : utils::format("{} (+{} more)", firstPath, currentRoots.size() - 1);

    juce::StringArray rootPaths;
    for (const auto& root : currentRoots) {
        rootPaths.add(root.getFullPathName());
    }

    currentRootLabel.setText(labelText, juce::dontSendNotification);
    currentRootLabel.setTooltip(rootPaths.joinIntoString("\n"));
}

void AudioBatchComponent::rescanCurrentRoot()
{
    refreshAnalysis(true);
}

bool AudioBatchComponent::hasSelectedRecords() const
{
    return resultsTable.getNumSelectedRows() > 0;
}

bool AudioBatchComponent::hasAnyRecords() const
{
    return !analysisResults.empty();
}

void AudioBatchComponent::clearAllRecords()
{
    if (analysisResults.empty()) {
        return;
    }

    analysisResults.clear();
    activeFileStatusLabels.clear();
    clearCurrentAudioPreview();
    audioInfo->clear();
    expectedResults = 0;
    completedResults = 0;
    analyzedFilesThisRun = 0;

    resultsTable.deselectAllRows();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    resultsTable.repaint();
    syncActivityTimer();
    statusLabel.setText("No files", juce::dontSendNotification);
    updateProcessButtonState();
}

void AudioBatchComponent::removeSelectedRecords()
{
    const auto selectedPaths = getSelectedRecordPaths();

    if (selectedPaths.isEmpty()) {
        return;
    }

    const auto selectedRows = resultsTable.getSelectedRows();
    const int fallbackRow = selectedRows.isEmpty() ? 0 : selectedRows[0];

    for (const auto& path : selectedPaths) {
        unmarkFileProcessing(path);
    }

    removeRecordsByPath(selectedPaths, fallbackRow);
}

void AudioBatchComponent::reanalyzeSelectedRecords()
{
    if (isAnalysisInProgress() || normalizeInProgress) {
        return;
    }

    const auto selectedFiles = getSelectedRecordFiles();

    if (selectedFiles.isEmpty()) {
        return;
    }

    startAnalysis(selectedFiles, false, true, false);
}

void AudioBatchComponent::showAudioSettingsWindow()
{
    openDialogWindow(settingsWindow, &audioSetupComponent, "Audio Settings");
}

void AudioBatchComponent::showSupportedNormalizationFormats()
{
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOk(
            juce::MessageBoxIconType::InfoIcon,
            "Normalization Format Support",
            AudioNormalizationService::getFormatSupportSummary(),
            "OK",
            this
        ),
        nullptr
    );
}

void AudioBatchComponent::loadNormalizationSettings()
{
    const auto* settings = pluginAppProperties.getUserSettings();
    normalizeTargetPeakDbfs = juce::jlimit(
        AudioNormalizationOptions::minimumTargetPeakDbfs,
        AudioNormalizationOptions::maximumTargetPeakDbfs,
        settings->getDoubleValue(
            normalize_target::targetPeakSettingKey, AudioNormalizationOptions::defaultTargetPeakDbfs
        )
    );
    normalizeTargetLufs = juce::jlimit(
        AudioNormalizationOptions::minimumTargetLufs,
        AudioNormalizationOptions::maximumTargetLufs,
        settings->getDoubleValue(normalize_target::targetLufsSettingKey, AudioNormalizationOptions::defaultTargetLufs)
    );

    const auto isLoudnessMode = settings->getValue(normalize_target::modeSettingKey) == "loudness";
    normalizeModeBox.setSelectedId(
        isLoudnessMode ? normalize_target::loudnessModeItemId : normalize_target::peakModeItemId,
        juce::dontSendNotification
    );
    updateNormalizeTargetSlider();
}

void AudioBatchComponent::saveNormalizationSettings()
{
    auto* settings = pluginAppProperties.getUserSettings();
    const auto isLoudnessMode = normalizeModeBox.getSelectedId() == normalize_target::loudnessModeItemId;
    settings->setValue(normalize_target::modeSettingKey, isLoudnessMode ? "loudness" : "peak");
    settings->setValue(normalize_target::targetPeakSettingKey, normalizeTargetPeakDbfs);
    settings->setValue(normalize_target::targetLufsSettingKey, normalizeTargetLufs);
}

void AudioBatchComponent::updateNormalizeTargetSlider()
{
    if (normalizeModeBox.getSelectedId() == normalize_target::loudnessModeItemId) {
        normalizeTargetSlider.setRange(
            AudioNormalizationOptions::minimumTargetLufs,
            AudioNormalizationOptions::maximumTargetLufs,
            normalize_target::loudnessStepDecibels
        );
        normalizeTargetSlider.setTextValueSuffix(" LUFS");
        normalizeTargetSlider.setValue(normalizeTargetLufs, juce::dontSendNotification);
    } else {
        normalizeTargetSlider.setRange(
            AudioNormalizationOptions::minimumTargetPeakDbfs,
            AudioNormalizationOptions::maximumTargetPeakDbfs,
            normalize_target::peakStepDecibels
        );
        normalizeTargetSlider.setTextValueSuffix(" dBFS");
        normalizeTargetSlider.setValue(normalizeTargetPeakDbfs, juce::dontSendNotification);
    }
}

AudioNormalizationOptions AudioBatchComponent::getNormalizationOptions() const
{
    AudioNormalizationOptions options;
    options.mode = normalizeModeBox.getSelectedId() == normalize_target::loudnessModeItemId
        ? AudioNormalizationMode::loudness
        : AudioNormalizationMode::peak;
    options.targetPeakDbfs = normalizeTargetPeakDbfs;
    options.targetLufs = normalizeTargetLufs;
    return options;
}

juce::String AudioBatchComponent::formatNormalizeTarget() const
{
    if (normalizeModeBox.getSelectedId() == normalize_target::loudnessModeItemId) {
        return utils::format("{:.1f} LUFS", normalizeTargetLufs);
    }

    return utils::format("{:.1f} dBFS peak", normalizeTargetPeakDbfs);
}

juce::File AudioBatchComponent::getDefaultBrowseDirectory()
{
    auto homeDirectory = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    // Prefer the Dropbox folder where the music library is usually kept,
    // then the system Music folder, and finally the home folder.
    // A user config file for this is listed in the README TODO.
#if JUCE_WINDOWS
    auto dropboxDirectory = juce::File("D:\\Dropbox");
#else
    auto dropboxDirectory = homeDirectory.getChildFile("Dropbox");
#endif

    if (dropboxDirectory.isDirectory()) {
        return dropboxDirectory;
    }

    if (const auto musicDirectory = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
        musicDirectory.isDirectory())
    {
        return musicDirectory;
    }

    return homeDirectory;
}

void AudioBatchComponent::handleDroppedPaths(const juce::StringArray& paths)
{
    juce::Array<juce::File> droppedFiles;
    juce::Array<juce::File> droppedDirectories;

    for (const auto& path : paths) {
        const juce::File droppedPath(path);

        if (droppedPath.isDirectory()) {
            droppedDirectories.addIfNotAlreadyThere(droppedPath);
            continue;
        }

        if (droppedPath.existsAsFile()) {
            droppedFiles.addIfNotAlreadyThere(droppedPath);
        }
    }

    if (droppedFiles.isEmpty() && !droppedDirectories.isEmpty()) {
        // The drop callback carries no modifier state, so read the live keyboard state instead.
        const auto appendToRoots = juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();

        for (const auto& directory : droppedDirectories) {
            utils::logInfo(
                "Dropped directory{}: {}", appendToRoots ? " to add" : "", directory.getFullPathName().quoted()
            );
        }

        if (appendToRoots) {
            addRoots(droppedDirectories);
        } else {
            setRoots(droppedDirectories);
        }

        return;
    }

    if (droppedFiles.isEmpty()) {
        statusLabel.setText("No supported dropped files", juce::dontSendNotification);
        return;
    }

    utils::logInfo("Dropped {} files", droppedFiles.size());
    startAnalysis(droppedFiles, false, false, false);
}

void AudioBatchComponent::refreshAnalysis(const bool forceRefresh)
{
    juce::Array<juce::File> existingRoots;
    for (const auto& root : currentRoots) {
        if (root.isDirectory()) {
            existingRoots.add(root);
        }
    }

    if (!existingRoots.isEmpty()) {
        startAnalysis(existingRoots, true, forceRefresh, true);
        return;
    }

    // No root directory set, so re-analyze individual files already in the results table.
    juce::Array<juce::File> existingFiles;
    for (const auto& record : analysisResults) {
        if (record.file.existsAsFile()) {
            existingFiles.addIfNotAlreadyThere(record.file);
        }
    }

    if (existingFiles.isEmpty()) {
        return;
    }

    startAnalysis(existingFiles, false, forceRefresh, true);
}

void AudioBatchComponent::handleFileContextMenuRequested(const int row, int columnId, const juce::MouseEvent& event)
{
    juce::ignoreUnused(columnId);

    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    if (!resultsTable.isRowSelected(row)) {
        resultsTable.selectRow(row, true, true);
    }

    showFileContextMenu(row, event.getScreenPosition());
}

void AudioBatchComponent::showFileContextMenu(int row, const juce::Point<int> screenPosition)
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(row)];
    const auto parentDirectory = record.file.getParentDirectory();
    const auto selectedRecords = getSelectedNormalizableRecords();
    const auto canNormalize = !normalizeInProgress && !isAnalysisInProgress() && canNormalizeRecords(selectedRecords);

    juce::PopupMenu menu;
    menu.addItem(revealFileMenuItemId, getRevealFileMenuLabel(), record.file.exists());
    menu.addItem(openParentDirectoryMenuItemId, "Open Parent Folder", parentDirectory.isDirectory());
    menu.addSeparator();
    menu.addItem(reanalyzeMenuItemId, "Re-analyze Selected", !isAnalysisInProgress() && !normalizeInProgress);
    menu.addItem(normalizeMenuItemId, utils::format("Normalize to {}", formatNormalizeTarget()), canNormalize);
    menu.addItem(normalizeSupportMenuItemId, "Normalization Format Support...");
    menu.addSeparator();
    const bool canProcess = !pluginProcessingInProgress && !isAnalysisInProgress() && !normalizeInProgress
        && pluginChain != nullptr && pluginChain->getNumEnabledValidSlots() > 0;
    menu.addItem(processMenuItemId, "Process With Plugin Chain (output AIFF)", canProcess);
    const bool anyHasGain = std::ranges::any_of(selectedRecords, [](const AudioAnalysisRecord& selectedRecord) {
        return selectedRecord.hasCustomGain;
    });
    menu.addItem(clearCustomGainMenuItemId, "Clear Per-file Gain", anyHasGain);
    menu.addSeparator();
    menu.addItem(removeFromListMenuItemId, "Remove from List");
    menu.addItem(moveToTrashMenuItemId, "Move to Trash", record.file.exists());

    const auto safeThis = SafePointer(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetScreenArea(juce::Rectangle(screenPosition.x, screenPosition.y, 1, 1))
            .withParentComponent(this),
        [safeThis, row](const int result) {
            if (safeThis == nullptr) {
                return;
            }

            switch (result) {
                case revealFileMenuItemId:
                    safeThis->revealRecordInFileManager(row);
                    break;
                case openParentDirectoryMenuItemId:
                    safeThis->revealRecordParentDirectory(row);
                    break;
                case normalizeMenuItemId:
                    safeThis->normalizeSelectedRecords();
                    break;
                case normalizeSupportMenuItemId:
                    safeThis->showSupportedNormalizationFormats();
                    break;
                case moveToTrashMenuItemId:
                    safeThis->moveSelectedRecordsToTrash(true);
                    break;
                case removeFromListMenuItemId:
                    safeThis->removeSelectedRecords();
                    break;
                case reanalyzeMenuItemId:
                    safeThis->reanalyzeSelectedRecords();
                    break;
                case processMenuItemId:
                    safeThis->processSelectedRecords();
                    break;
                case clearCustomGainMenuItemId:
                    safeThis->gainSlider.setValue(0.0, juce::dontSendNotification);
                    safeThis->applyCustomGainToSelection(0.0f, false);
                    break;
                default:
                    break;
            }
        }
    );
}

void AudioBatchComponent::revealRecordInFileManager(const int row) const
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    if (const auto& record = analysisResults[static_cast<std::size_t>(row)]; record.file.exists()) {
        record.file.revealToUser();
    } else {
        record.file.getParentDirectory().revealToUser();
    }
}

void AudioBatchComponent::revealRecordParentDirectory(const int row) const
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto parentDirectory = analysisResults[static_cast<std::size_t>(row)].file.getParentDirectory();

    if (parentDirectory.isDirectory()) {
        parentDirectory.revealToUser();
    }
}

void AudioBatchComponent::moveSelectedRecordsToTrash(const bool promptForConfirmation)
{
    const auto selectedRows = resultsTable.getSelectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    juce::Array<juce::File> filesToTrash;
    juce::StringArray removedPaths;

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        const auto& record = analysisResults[static_cast<std::size_t>(rowNumber)];

        if (!record.file.exists()) {
            continue;
        }

        filesToTrash.add(record.file);
        removedPaths.addIfNotAlreadyThere(record.fullPath);
    }

    if (filesToTrash.isEmpty()) {
        return;
    }

    const auto fileCount = filesToTrash.size();
    const auto message = fileCount == 1 ? utils::format("Move\n\n{}\n\nto the system trash?", filesToTrash.getFirst())
                                        : utils::format("Move {} selected files to the system trash?", fileCount);

    if (!promptForConfirmation) {
        runMoveToTrash(filesToTrash, removedPaths, selectedRows[0]);
        return;
    }

    const auto safeThis = SafePointer(this);
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::WarningIcon, "Move to Trash", message, "Move to Trash", "Cancel", this
        ),
        [safeThis, filesToTrash, removedPaths, fallbackRow = selectedRows[0]](const int result) {
            if (safeThis == nullptr || result == 0) {
                return;
            }
            safeThis->runMoveToTrash(filesToTrash, removedPaths, fallbackRow);
        }
    );
}

void AudioBatchComponent::runMoveToTrash(
    const juce::Array<juce::File>& filesToTrash,
    const juce::StringArray& removedPaths,
    const int fallbackRow
)
{
    juce::StringArray failedPaths;
    juce::StringArray successfullyRemovedPaths;

    if (currentAudioFile.exists()) {
        for (const auto& file : filesToTrash) {
            if (file == currentAudioFile) {
                clearCurrentAudioPreview();
                break;
            }
        }
    }

    for (int index = 0; index < filesToTrash.size(); ++index) {
        const auto& file = filesToTrash.getReference(index);

        if (!utils::moveToTrash(file)) {
            failedPaths.add(file.getFullPathName());
            continue;
        }

        if (juce::isPositiveAndBelow(index, removedPaths.size())) {
            successfullyRemovedPaths.addIfNotAlreadyThere(removedPaths[index]);
        }
    }

    removeRecordsByPath(successfullyRemovedPaths, fallbackRow);

    if (!failedPaths.isEmpty()) {
        for (const auto& failedPath : failedPaths) {
            utils::logError("Move to trash failed for {}", failedPath.quoted());
        }

        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Move to Trash Failed",
                failedPaths.size() == 1
                    ? utils::format("Could not move this file to the system trash:\n\n{}", failedPaths[0])
                    : utils::format("Could not move {} files to the system trash.", failedPaths.size()),
                "OK",
                this
            ),
            nullptr
        );
    }
}

int AudioBatchComponent::findRecordIndex(const juce::String& fullPath) const
{
    for (std::size_t index = 0; index < analysisResults.size(); ++index) {
        if (analysisResults[index].fullPath == fullPath) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

juce::Array<juce::File> AudioBatchComponent::getSelectedRecordFiles() const
{
    juce::Array<juce::File> selectedFiles;
    const auto selectedRows = resultsTable.getSelectedRows();

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        if (const auto& file = analysisResults[static_cast<std::size_t>(rowNumber)].file; file.existsAsFile()) {
            selectedFiles.addIfNotAlreadyThere(file);
        }
    }

    return selectedFiles;
}

std::vector<AudioAnalysisRecord> AudioBatchComponent::getSelectedNormalizableRecords() const
{
    std::vector<AudioAnalysisRecord> selectedRecords;
    const auto selectedRows = resultsTable.getSelectedRows();

    selectedRecords.reserve(static_cast<std::size_t>(selectedRows.size()));

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        const auto& record = analysisResults[static_cast<std::size_t>(rowNumber)];

        if (!record.file.existsAsFile() || !record.isReady()) {
            selectedRecords.clear();
            return selectedRecords;
        }

        selectedRecords.push_back(record);
    }

    return selectedRecords;
}

bool AudioBatchComponent::canNormalizeRecords(const std::vector<AudioAnalysisRecord>& records)
{
    if (records.empty()) {
        return false;
    }

    return std::ranges::all_of(records, [](const auto& record) {
        return AudioNormalizationService::canNormalizeFile(record.file);
    });
}

juce::String AudioBatchComponent::buildNormalizationUnavailableMessage(const std::vector<AudioAnalysisRecord>& records)
{
    juce::StringArray unsupportedLines;

    for (const auto& record : records) {
        const auto reason = AudioNormalizationService::getNormalizationSupportMessage(record.file);

        if (reason.isEmpty()) {
            continue;
        }

        unsupportedLines.add(utils::format("{} ({}): {}", record.fileName, formatLabelForFile(record.file), reason));
    }

    if (unsupportedLines.isEmpty()) {
        return {};
    }

    return utils::format(
        "The selected files cannot be normalized with the current build:\n\n{}\n\n{}",
        unsupportedLines.joinIntoString(juce::newLine),
        AudioNormalizationService::getFormatSupportSummary()
    );
}

void AudioBatchComponent::markFilesProcessing(const juce::Array<juce::File>& files, const juce::String& activityLabel)
{
    for (const auto& file : files) {
        activeFileStatusLabels[file.getFullPathName()] = activityLabel;
        operationMessages.erase(file.getFullPathName());
    }

    syncActivityTimer();
    resultsTable.repaint();
}

void AudioBatchComponent::unmarkFileProcessing(const juce::String& fullPath)
{
    activeFileStatusLabels.erase(fullPath);
    syncActivityTimer();
}

void AudioBatchComponent::reconcilePendingAnalysisResults()
{
    const auto selectedPaths = getSelectedRecordPaths();
    bool resultsChanged = false;

    for (auto& record : analysisResults) {
        const auto activeStatus = getActiveStatusLabel(record);
        const auto needsRefresh
            = activeStatus == "Analyzing" || activeStatus == "Waiting" || record.status == AudioAnalysisStatus::pending;

        if (!needsRefresh) {
            continue;
        }

        AudioAnalysisRecord refreshedRecord;

        if (analysisCache.getAnalysis(record.file, refreshedRecord)) {
            record = std::move(refreshedRecord);
            resultsChanged = true;
            continue;
        }

        if (!record.file.existsAsFile()) {
            record.status = AudioAnalysisStatus::failed;
            record.errorMessage = "File does not exist";
            utils::logError("Analysis failed for {}: {}", record.fullPath.quoted(), record.errorMessage);
            resultsChanged = true;
            continue;
        }

        auto analyzedRecord = AudioAnalysisService::analyzeFile(record.file);

        // An unchanged file keeps the custom gain the user set for it.
        if (float customGainDb = 0.0f; analysisCache.getCustomGain(
                record.file, analyzedRecord.fileSize, analyzedRecord.modifiedTimeMs, customGainDb
            ))
        {
            analyzedRecord.customGainDb = customGainDb;
            analyzedRecord.hasCustomGain = true;
        }

        analysisCache.storeAnalysis(analyzedRecord);
        record = std::move(analyzedRecord);
        resultsChanged = true;
    }

    for (auto iterator = activeFileStatusLabels.begin(); iterator != activeFileStatusLabels.end();) {
        if (iterator->second == "Analyzing" || iterator->second == "Waiting") {
            iterator = activeFileStatusLabels.erase(iterator);
        } else {
            ++iterator;
        }
    }

    completedResults = expectedResults;
    syncActivityTimer();

    if (resultsChanged) {
        sortResults();
        resultsTable.updateContent();
        updateResultsTableColumnWidths();
        restoreSelectionByPaths(selectedPaths);

        if (currentAudioFile.existsAsFile()) {
            if (const auto currentIndex = findRecordIndex(currentAudioFile.getFullPathName()); currentIndex >= 0) {
                updateAudioInfo(analysisResults[static_cast<std::size_t>(currentIndex)]);
            }
        }
    }

    resultsTable.repaint();
}

juce::StringArray AudioBatchComponent::getSelectedRecordPaths() const
{
    juce::StringArray selectedPaths;
    const auto selectedRows = resultsTable.getSelectedRows();

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        selectedPaths.addIfNotAlreadyThere(analysisResults[static_cast<std::size_t>(rowNumber)].fullPath);
    }

    return selectedPaths;
}

void AudioBatchComponent::restoreSelectionByPaths(const juce::StringArray& selectedPaths)
{
    juce::SparseSet<int> selectedRows;

    for (const auto& path : selectedPaths) {
        if (const auto rowNumber = findRecordIndex(path); rowNumber >= 0) {
            selectedRows.addRange(juce::Range<int>::withStartAndLength(rowNumber, 1));
        }
    }

    resultsTable.setSelectedRows(selectedRows, juce::dontSendNotification);
}

void AudioBatchComponent::removeRecordsByPath(const juce::StringArray& removedPaths, const int fallbackRow)
{
    if (removedPaths.isEmpty()) {
        return;
    }

    auto remainingSelectedPaths = getSelectedRecordPaths();
    for (int index = remainingSelectedPaths.size(); --index >= 0;) {
        if (removedPaths.contains(remainingSelectedPaths[index])) {
            remainingSelectedPaths.remove(index);
        }
    }

    const auto currentFileRemoved
        = currentAudioFile.exists() && removedPaths.contains(currentAudioFile.getFullPathName());

    analysisResults.erase(
        std::ranges::remove_if(
            analysisResults,
            [&removedPaths](const AudioAnalysisRecord& record) { return removedPaths.contains(record.fullPath); }
        ).begin(),
        analysisResults.end()
    );

    expectedResults = juce::jmax(0, expectedResults - removedPaths.size());
    completedResults = juce::jmin(completedResults, expectedResults);

    if (currentFileRemoved) {
        clearCurrentAudioPreview();
    }

    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(remainingSelectedPaths);

    if (resultsTable.getNumSelectedRows() == 0 && !analysisResults.empty()) {
        resultsTable.selectRow(juce::jlimit(0, static_cast<int>(analysisResults.size()) - 1, fallbackRow));
    }

    if (analysisResults.empty()) {
        audioInfo->clear();
    }

    updateStatusLabel();
    resultsTable.repaint();
    updateProcessButtonState();
}

void AudioBatchComponent::handleAnalysisResult(const AudioAnalysisRecord& record, const int runId)
{
    if (runId != analysisRunId) {
        return;
    }

    ++completedResults;
    unmarkFileProcessing(record.fullPath);

    if (!record.file.exists()) {
        updateStatusLabel();
        return;
    }

    const auto selectedPaths = getSelectedRecordPaths();

    auto mergedRecord = record;
    if (const auto existingIndex = findRecordIndex(record.fullPath); existingIndex >= 0) {
        const auto& existing = analysisResults[static_cast<std::size_t>(existingIndex)];

        // A custom gain only applies to the version of the file it was set for, so a changed file starts without one.
        if (existing.hasCustomGain && existing.fileSize == record.fileSize
            && existing.modifiedTimeMs == record.modifiedTimeMs)
        {
            mergedRecord.customGainDb = existing.customGainDb;
            mergedRecord.hasCustomGain = existing.hasCustomGain;
        }
        analysisResults[static_cast<std::size_t>(existingIndex)] = mergedRecord;
    } else {
        analysisResults.push_back(mergedRecord);
    }

    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(selectedPaths);

    if (currentAudioFile == mergedRecord.file) {
        updateAudioInfo(mergedRecord);
        updateGainControlsForSelection();
        updateThumbnailDisplayGain();
    }

    updateStatusLabel();
    updateProcessButtonState();
}

void AudioBatchComponent::handleNormalizeResult(const AudioNormalizationResult& result)
{
    ++normalizedResultsCompleted;
    unmarkFileProcessing(result.fullPath);

    if (result.succeeded) {
        auto selectedPaths = getSelectedRecordPaths();

        for (int index = 0; index < selectedPaths.size(); ++index) {
            if (selectedPaths[index] == result.fullPath) {
                selectedPaths.set(index, result.analysisRecord.fullPath);
            }
        }

        analysisResults.erase(
            std::ranges::remove_if(
                analysisResults,
                [&result](const AudioAnalysisRecord& record) {
                    return record.fullPath == result.fullPath && record.fullPath != result.analysisRecord.fullPath;
                }
            ).begin(),
            analysisResults.end()
        );

        if (const auto existingIndex = findRecordIndex(result.analysisRecord.fullPath); existingIndex >= 0) {
            analysisResults[static_cast<std::size_t>(existingIndex)] = result.analysisRecord;
        } else {
            analysisResults.push_back(result.analysisRecord);
        }

        analysisCache.storeAnalysis(result.analysisRecord);

        // A converted original was moved to the trash, so its cached row is no longer needed.
        if (result.analysisRecord.fullPath != result.fullPath) {
            analysisCache.removeAnalysis(result.file);
        }

        sortResults();
        resultsTable.updateContent();
        updateResultsTableColumnWidths();
        restoreSelectionByPaths(selectedPaths);

        if (currentAudioFile == result.file) {
            currentAudioFile = result.analysisRecord.file;
            showAudioResource(juce::URL(currentAudioFile));
            updateAudioInfo(result.analysisRecord);
        } else if (currentAudioFile == result.analysisRecord.file) {
            updateAudioInfo(result.analysisRecord);
        }
    } else {
        normalizationFailures.add(utils::format("{}: {}", result.fileName, result.errorMessage));
        operationMessages[result.fullPath] = {.text = result.errorMessage, .isError = true};
        resultsTable.repaint();
    }

    updateStatusLabel();
}

void AudioBatchComponent::handleNormalizeComplete(const int totalFiles)
{
    normalizedResultsExpected = totalFiles;
    normalizeInProgress = false;
    syncActivityTimer();

    if (!normalizationFailures.isEmpty()) {
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Normalize",
                normalizationFailures.size() == 1 ? normalizationFailures[0]
                                                  : utils::format(
                                                        "Some files could not be normalized:\n\n{}",
                                                        buildNormalizationFailureMessage(normalizationFailures)
                                                    ),
                "OK",
                this
            ),
            nullptr
        );
    }

    if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateStatusLabel();
}

void AudioBatchComponent::handleAnalysisComplete(const int totalFiles, const int runId)
{
    if (runId != analysisRunId) {
        return;
    }

    expectedResults = totalFiles;

    // The plan handler already reported that no files were found.
    if (totalFiles == 0) {
        syncActivityTimer();
        updateProcessButtonState();
        return;
    }

    reconcilePendingAnalysisResults();
    updateStatusLabel();

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - analysisStartedAtMs;
    const auto cachedFileCount = juce::jmax(0, totalFiles - analyzedFilesThisRun);
    const auto failedFileCount
        = std::ranges::count_if(analysisResults, [](const auto& record) { return record.hasError(); });
    utils::logInfo(
        "Analysis complete: loaded {} files ({} analyzed, {} from cache, {} failed) in {:.2f} s",
        totalFiles,
        analyzedFilesThisRun,
        cachedFileCount,
        failedFileCount,
        elapsedMs / millisecondsPerSecond
    );

    if (resultsTable.getSelectedRow() < 0 && !analysisResults.empty()) {
        resultsTable.selectRow(0);
    } else if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateProcessButtonState();
}

void AudioBatchComponent::updateProcessButtonState()
{
    const bool hasChain = pluginChain != nullptr && pluginChain->getNumEnabledValidSlots() > 0;
    processButton.setEnabled(hasChain && hasAnyRecords() && !pluginProcessingInProgress && !isAnalysisInProgress());
}

void AudioBatchComponent::updateStatusLabel()
{
    if (pluginProcessingInProgress && processedResultsExpected > 0) {
        statusLabel.setText(
            utils::format("Processing {}/{}", processedResultsCompleted, processedResultsExpected),
            juce::dontSendNotification
        );
        return;
    }

    if (normalizeInProgress && normalizedResultsExpected > 0) {
        statusLabel.setText(
            utils::format("Normalizing {}/{}", normalizedResultsCompleted, normalizedResultsExpected),
            juce::dontSendNotification
        );
        return;
    }

    if (expectedResults <= 0) {
        statusLabel.setText("No supported audio files found", juce::dontSendNotification);
        return;
    }

    if (completedResults < expectedResults) {
        statusLabel.setText(
            utils::format("Analyzing {}/{}", completedResults, expectedResults), juce::dontSendNotification
        );
        return;
    }

    statusLabel.setText(utils::format("Loaded {} files", analysisResults.size()), juce::dontSendNotification);
}

void AudioBatchComponent::normalizeSelectedRecords()
{
    if (normalizeInProgress) {
        utils::logError("Normalization requested while another normalization pass is already in progress");
        statusLabel.setText("Normalization already in progress", juce::dontSendNotification);
        return;
    }

    if (isAnalysisInProgress()) {
        utils::logError("Normalization requested before analysis finished");
        statusLabel.setText("Wait for analysis to finish before normalizing", juce::dontSendNotification);
        return;
    }

    const auto recordsToNormalize = getSelectedNormalizableRecords();

    if (recordsToNormalize.empty()) {
        return;
    }

    if (!canNormalizeRecords(recordsToNormalize)) {
        utils::logError("Normalization unavailable for the selected files");
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Normalization Unavailable",
                buildNormalizationUnavailableMessage(recordsToNormalize),
                "OK",
                this
            ),
            nullptr
        );
        statusLabel.setText("Selected file type cannot be normalized in this build", juce::dontSendNotification);
        return;
    }

    juce::Array<juce::File> filesToNormalize;
    for (const auto& record : recordsToNormalize) {
        filesToNormalize.add(record.file);

        if (record.file == currentAudioFile) {
            clearCurrentAudioPreview();
        }
    }

    normalizationFailures.clear();
    normalizedResultsCompleted = 0;
    normalizeInProgress = true;
    auto normalizationOptions = getNormalizationOptions();
    normalizationOptions.overwriteExisting = forceOverwriteToggle.getToggleState();
    normalizedResultsExpected = normalizeCoordinator.start(recordsToNormalize, normalizationOptions);

    if (normalizedResultsExpected <= 0) {
        normalizeInProgress = false;
        updateStatusLabel();
        return;
    }

    markFilesProcessing(filesToNormalize, "Normalizing");

    updateStatusLabel();
}

void AudioBatchComponent::sortResults()
{
    std::ranges::sort(analysisResults, [this](const auto& lhs, const auto& rhs) {
        if (lhs.hasError() != rhs.hasError()) {
            return !lhs.hasError();
        }

        const auto compareUsingSort = [&](int columnId, bool isForwards) {
            if (columnId <= 0) {
                return 0;
            }

            const auto comparison = compareRecordsByColumn(lhs, rhs, columnId);
            if (comparison == 0) {
                return 0;
            }

            return isForwards ? comparison : -comparison;
        };

        if (const auto primaryComparison = compareUsingSort(currentSortColumnId, currentSortForwards);
            primaryComparison != 0)
        {
            return primaryComparison < 0;
        }

        if (const auto secondaryComparison = compareUsingSort(secondarySortColumnId, secondarySortForwards);
            secondaryComparison != 0)
        {
            return secondaryComparison < 0;
        }

        if (const auto nameComparison = compareNaturalStrings(lhs.fileName, rhs.fileName); nameComparison != 0) {
            return nameComparison < 0;
        }

        return compareNaturalStrings(lhs.fullPath, rhs.fullPath) < 0;
    });
}

void AudioBatchComponent::startAnalysis(
    const juce::Array<juce::File>& inputPaths,
    const bool recursive,
    const bool forceRefresh,
    const bool clearResults
)
{
    if (normalizeInProgress) {
        statusLabel.setText("Normalization in progress", juce::dontSendNotification);
        return;
    }

    if (clearResults) {
        analysisResults.clear();
        activeFileStatusLabels.clear();
        clearCurrentAudioPreview();
        audioInfo->clear();
        resultsTable.updateContent();
        updateResultsTableColumnWidths();
        resultsTable.repaint();
    }

    AudioAnalysisOptions options;
    options.inputPaths = inputPaths;
    options.recursive = recursive;
    options.refresh = forceRefresh;

    analysisStartedAtMs = juce::Time::getMillisecondCounterHiRes();
    completedResults = 0;
    analysisReplacesResults = clearResults;
    // Expected until the plan arrives, so the run counts as in progress while the folders are scanned.
    expectedResults = 1;
    statusLabel.setText("Scanning for audio files...", juce::dontSendNotification);

    if (clearResults) {
        updateCurrentRootLabel();
    }

    analysisRunId = analysisCoordinator.start(options);
    updateProcessButtonState();
}

void AudioBatchComponent::handleAnalysisPlan(const AnalysisCoordinator::AnalysisPlan& plan)
{
    if (plan.runId != analysisRunId) {
        return;
    }

    expectedResults = plan.files.size();
    analyzedFilesThisRun = plan.staleFiles.size();
    utils::logInfo("Found {} audio files", expectedResults);

    if (expectedResults == 0) {
        statusLabel.setText(
            analysisReplacesResults ? "No supported audio files found" : "No supported dropped files",
            juce::dontSendNotification
        );
        updateProcessButtonState();
        return;
    }

    for (const auto& file : plan.staleFiles) {
        if (findRecordIndex(file.getFullPathName()) >= 0) {
            continue;
        }

        auto placeholder = AudioAnalysisRecord::fromFile(file);
        placeholder.status = AudioAnalysisStatus::pending;
        analysisResults.push_back(std::move(placeholder));
    }

    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    markFilesProcessing(plan.staleFiles, "Waiting");

    statusLabel.setText(
        utils::format("Analyzing {}/{}", completedResults, expectedResults), juce::dontSendNotification
    );
}

juce::String AudioBatchComponent::getActiveStatusLabel(const AudioAnalysisRecord& record) const
{
    if (const auto iterator = activeFileStatusLabels.find(record.fullPath); iterator != activeFileStatusLabels.end()) {
        return iterator->second;
    }

    return {};
}

OperationMessage AudioBatchComponent::getOperationMessage(const AudioAnalysisRecord& record) const
{
    if (const auto iterator = operationMessages.find(record.fullPath); iterator != operationMessages.end()) {
        return iterator->second;
    }

    return {};
}

float AudioBatchComponent::getActivityPhase()
{
    constexpr auto rotationsPerSecond = 0.85;
    const auto phase = std::fmod(juce::Time::getMillisecondCounterHiRes() * 0.001 * rotationsPerSecond, 1.0);
    return static_cast<float>(phase);
}

bool AudioBatchComponent::isAnalysisInProgress() const
{
    return completedResults < expectedResults;
}

bool AudioBatchComponent::isAnyFileProcessing() const
{
    return !activeFileStatusLabels.empty();
}

void AudioBatchComponent::syncActivityTimer()
{
    if (isAnyFileProcessing()) {
        startTimerHz(activityIndicatorTimerHz);
    } else {
        stopTimer();
    }
}

void AudioBatchComponent::timerCallback()
{
    if (!isAnyFileProcessing()) {
        stopTimer();
        return;
    }

    resultsTable.repaint();
}

void AudioBatchComponent::handleSortRequested(const int columnId, const bool isForwards)
{
    currentSortColumnId = columnId;
    currentSortForwards = isForwards;

    if (secondarySortColumnId == currentSortColumnId) {
        secondarySortColumnId = 0;
        secondarySortForwards = true;
    }

    refreshSortIndicators();

    const auto selectedPaths = getSelectedRecordPaths();

    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(selectedPaths);
}

void AudioBatchComponent::handleSecondarySortRequested(const int columnId)
{
    if (columnId <= 0) {
        return;
    }

    if (columnId == currentSortColumnId) {
        secondarySortColumnId = 0;
        secondarySortForwards = true;
    } else if (secondarySortColumnId == columnId) {
        secondarySortForwards = !secondarySortForwards;
    } else {
        secondarySortColumnId = columnId;
        secondarySortForwards = true;
    }

    refreshSortIndicators();

    const auto selectedPaths = getSelectedRecordPaths();

    sortResults();
    resultsTable.updateContent();
    updateResultsTableColumnWidths();
    restoreSelectionByPaths(selectedPaths);
}

void AudioBatchComponent::refreshSortIndicators()
{
    getSecondarySortHeader(resultsTable).setSecondarySortIndicator(secondarySortColumnId, secondarySortForwards);
}

int AudioBatchComponent::getSelectionDisplayRow(
    const juce::SparseSet<int>& selectedRows,
    const int lastRowSelected
) const
{
    if (selectedRows.isEmpty()) {
        return -1;
    }

    if (currentAudioFile.existsAsFile()) {
        if (const auto currentRow = findRecordIndex(currentAudioFile.getFullPathName());
            currentRow >= 0 && selectedRows.contains(currentRow))
        {
            return currentRow;
        }
    }

    if (juce::isPositiveAndBelow(lastRowSelected, static_cast<int>(analysisResults.size()))
        && selectedRows.contains(lastRowSelected))
    {
        return lastRowSelected;
    }

    return selectedRows[0];
}

void AudioBatchComponent::handleSelectionChanged(const int lastRowSelected)
{
    const auto selectedRows = resultsTable.getSelectedRows();
    const auto row = getSelectionDisplayRow(selectedRows, lastRowSelected);

    if (!juce::isPositiveAndBelow(row, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(row)];

    if (record.file.existsAsFile() && currentAudioFile != record.file) {
        currentAudioFile = record.file;
        showAudioResource(juce::URL(record.file));
        utils::logDebug("Loaded file: {}", record.fileName);
    }

    updateAudioInfo(record);
    updateGainControlsForSelection();
    updateThumbnailDisplayGain();
}

bool AudioBatchComponent::shouldDropFilesWhenDraggedExternally(
    const juce::DragAndDropTarget::SourceDetails& sourceDetails,
    juce::StringArray& files,
    bool& canMoveFiles
)
{
    files = juce::StringArray::fromLines(sourceDetails.description.toString());
    files.removeEmptyStrings();
    files.removeDuplicates(false);

    for (int index = files.size(); --index >= 0;) {
        if (!juce::File(files[index]).existsAsFile()) {
            files.remove(index);
        }
    }

    canMoveFiles = false;
    return !files.isEmpty();
}

void AudioBatchComponent::updateAudioInfo(const AudioAnalysisRecord& record)
{
    std::vector<AudioInfoRow> rows;

    if (!record.isReady() && !record.hasError()) {
        rows.emplace_back("File", record.fileName);
        rows.emplace_back(
            "Status", getActiveStatusLabel(record).isNotEmpty() ? getActiveStatusLabel(record) : "Pending"
        );
        audioInfo->setRows(std::move(rows));
        return;
    }

    if (record.hasError()) {
        rows.emplace_back("File", record.fileName);
        rows.emplace_back("Error", record.errorMessage);
        audioInfo->setRows(std::move(rows));
        return;
    }

    rows.emplace_back("Format", getRecordTypeLabel(record));
    rows.emplace_back("Channels", juce::String(record.channels));
    rows.emplace_back("Bits per sample", AudioAnalysisService::formatBitsPerSampleDisplay(record));
    rows.emplace_back("Sample rate", juce::String(record.sampleRate));
    rows.emplace_back("Bitrate", AudioAnalysisService::formatBitrateDisplay(record));
    rows.emplace_back("Duration", juce::RelativeTime(record.durationSeconds).getDescription());
    rows.emplace_back("Samples", juce::String(record.lengthInSamples));
    rows.emplace_back("Peak Max", AudioAnalysisService::formatPeakDisplay(record.overallPeak));
    rows.emplace_back("Peak L", AudioAnalysisService::formatPeakDisplay(record.peakLeft));
    rows.emplace_back("Peak R", AudioAnalysisService::formatPeakDisplay(record.peakRight));
    rows.emplace_back("True Peak Max", AudioAnalysisService::formatTruePeakDisplay(record.overallTruePeak));
    rows.emplace_back("Max Short-term", AudioAnalysisService::formatLoudnessDisplay(record.maxShortTermLufs));
    rows.emplace_back("Integrated Loudness", AudioAnalysisService::formatLoudnessDisplay(record.integratedLufs));
    rows.emplace_back("Status", AudioAnalysisService::formatStatus(record));

    audioInfo->setRows(std::move(rows));
}

void AudioBatchComponent::showAudioResource(juce::URL resource)
{
    if (loadURLIntoTransport(resource)) {
        currentAudioUrl = std::move(resource);
        currentWaveformLoadedFromCache = false;
        startStopButton.setEnabled(true);
        startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::green());
        zoomSlider.setValue(100.0, juce::dontSendNotification);

        juce::MemoryBlock waveformData;
        if (currentAudioFile.existsAsFile() && analysisCache.getWaveformData(currentAudioFile, waveformData)
            && thumbnail->loadFromCacheData(waveformData))
        {
            currentWaveformLoadedFromCache = true;
        } else {
            thumbnail->setURL(currentAudioUrl);
        }
    } else {
        currentAudioUrl = {};
        currentWaveformLoadedFromCache = false;
        startStopButton.setEnabled(false);
        startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium());
        thumbnail->setURL(juce::URL());
    }
}

void AudioBatchComponent::handleThumbnailFullyLoaded()
{
    if (currentWaveformLoadedFromCache || !currentAudioFile.existsAsFile()) {
        return;
    }

    const auto waveformData = thumbnail->saveToCacheData();
    if (waveformData.getSize() == 0) {
        return;
    }

    currentWaveformLoadedFromCache = analysisCache.storeWaveformData(currentAudioFile, waveformData);
}

bool AudioBatchComponent::loadURLIntoTransport(const juce::URL& audioURL)
{
    // Unload the previous file source and delete it.
    transportSource.stop();
    transportSource.setSource(nullptr);
    currentAudioFileSource.reset();

    juce::AudioFormatReader* reader = nullptr;

    if (reader == nullptr) {
        reader = formatManager.createReaderFor(
            audioURL.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress))
        );
    }
    if (reader != nullptr) {
        currentAudioFileSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
        transportSource.setSource(currentAudioFileSource.get(), 32768, &thread, reader->sampleRate);

        return true;
    }

    return false;
}

void AudioBatchComponent::startOrStop()
{
    if (transportSource.isPlaying()) {
        transportSource.stop();
    } else {
        transportSource.start();
    }
}

void AudioBatchComponent::updatePlaybackButtonForTransportState()
{
    if (!startStopButton.isEnabled()) {
        return;
    }

    const auto colour = transportSource.isPlaying() ? juce::CustomLookAndFeel::red() : juce::CustomLookAndFeel::green();
    startStopButton.setColour(juce::TextButton::buttonColourId, colour);
}

void AudioBatchComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &transportSource) {
        updatePlaybackButtonForTransportState();
        return;
    }

    if (source == thumbnail.get()) {
        const auto droppedFiles = thumbnail->getLastDroppedFiles();

        if (!droppedFiles.isEmpty()) {
            handleDroppedPaths(droppedFiles);
            return;
        }
    }
}

void AudioBatchComponent::clearCurrentAudioPreview()
{
    currentAudioFile = juce::File();
    currentAudioUrl = {};
    currentWaveformLoadedFromCache = false;
    transportSource.stop();
    transportSource.setSource(nullptr);
    currentAudioFileSource.reset();
    thumbnail->setURL(juce::URL());
    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium());
}

void AudioBatchComponent::mouseMagnify(const juce::MouseEvent& /*event*/, const float scaleFactor)
{
    const auto newZoom = zoomSlider.getValue() * static_cast<double>(scaleFactor * scaleFactor);
    zoomSlider.setValue(newZoom);
}

void AudioBatchComponent::zoomLevelChanged(const double zoomLevel)
{
    thumbnail->setZoom(zoomLevel);
}

void AudioBatchComponent::openDialogWindow(
    SafePointer<juce::DialogWindow>& window,
    const SafePointer<juce::Component>& component,
    const juce::String& title
)
{
    // Create the window the first time it is opened.
    if (window == nullptr) {
        juce::DialogWindow::LaunchOptions launchOptions;
        launchOptions.dialogTitle = title;
        launchOptions.content.setNonOwned(component);
        launchOptions.content->setSize(layout::dialogContentWidth, layout::dialogContentHeight);
        window = launchOptions.create();
        childWindows.add(window);
    }
#if JUCE_WINDOWS
    window->setUsingNativeTitleBar(false);
    window->setTitleBarTextCentred(false);
    window->setTitleBarHeight(30);
#else
    window->setUsingNativeTitleBar(true);
#endif
    window->setSize(window->getWidth(), window->getHeight());
    window->setResizable(true, true);
    window->setResizeLimits(window->getWidth(), window->getHeight(), 4096, 4096);
    window->setColour(juce::DialogWindow::backgroundColourId, juce::CustomLookAndFeel::greySemiDark());
    window->setVisible(true);
    window->toFront(true);
}

std::vector<AudioAnalysisRecord> AudioBatchComponent::getSelectedProcessableRecords() const
{
    std::vector<AudioAnalysisRecord> selected;
    const auto selectedRows = resultsTable.getSelectedRows();

    selected.reserve(static_cast<std::size_t>(selectedRows.size()));

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        const auto& record = analysisResults[static_cast<std::size_t>(rowNumber)];

        if (!record.file.existsAsFile() || !record.isReady()) {
            continue;
        }

        if (!PluginProcessingService::canProcessFile(record.file)) {
            continue;
        }

        selected.push_back(record);
    }

    return selected;
}

void AudioBatchComponent::applyCustomGainToSelection(const float gainDb, const bool hasGain)
{
    const auto selectedRows = resultsTable.getSelectedRows();
    if (selectedRows.size() == 0) {
        return;
    }

    bool anyChanged = false;

    for (int index = 0; index < selectedRows.size(); ++index) {
        const auto rowNumber = selectedRows[index];

        if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(analysisResults.size()))) {
            continue;
        }

        auto& record = analysisResults[static_cast<std::size_t>(rowNumber)];
        const auto previousHadGain = record.hasCustomGain;
        const auto previousValue = record.customGainDb;

        record.customGainDb = hasGain ? gainDb : 0.0f;
        record.hasCustomGain = hasGain;

        if (previousHadGain != record.hasCustomGain || !juce::approximatelyEqual(previousValue, record.customGainDb)) {
            analysisCache.storeCustomGain(record.file, record.customGainDb, record.hasCustomGain);
            anyChanged = true;
        }
    }

    if (anyChanged) {
        resultsTable.repaint();
        updateThumbnailDisplayGain();
        gainClearButton.setEnabled(hasGain);
    }
}

void AudioBatchComponent::updateGainControlsForSelection()
{
    const auto selectedRows = resultsTable.getSelectedRows();
    const bool hasSelection = selectedRows.size() > 0;

    gainSlider.setEnabled(hasSelection);
    gainClearButton.setEnabled(false);

    if (!hasSelection) {
        gainSlider.setValue(0.0, juce::dontSendNotification);
        return;
    }

    // Use the currently selected display row to drive the slider value.
    int referenceRow = selectedRows[0];
    if (currentAudioFile.existsAsFile()) {
        if (const auto currentRow = findRecordIndex(currentAudioFile.getFullPathName());
            currentRow >= 0 && selectedRows.contains(currentRow))
        {
            referenceRow = currentRow;
        }
    }

    if (!juce::isPositiveAndBelow(referenceRow, static_cast<int>(analysisResults.size()))) {
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(referenceRow)];
    gainSlider.setValue(
        record.hasCustomGain ? static_cast<double>(record.customGainDb) : 0.0, juce::dontSendNotification
    );
    gainClearButton.setEnabled(record.hasCustomGain);
}

void AudioBatchComponent::updateThumbnailDisplayGain()
{
    if (thumbnail == nullptr) {
        return;
    }

    if (!currentAudioFile.existsAsFile()) {
        thumbnail->setDisplayGain(1.0f);
        return;
    }

    const auto currentRow = findRecordIndex(currentAudioFile.getFullPathName());
    if (!juce::isPositiveAndBelow(currentRow, static_cast<int>(analysisResults.size()))) {
        thumbnail->setDisplayGain(1.0f);
        return;
    }

    const auto& record = analysisResults[static_cast<std::size_t>(currentRow)];
    const auto gain = record.hasCustomGain ? juce::Decibels::decibelsToGain(record.customGainDb) : 1.0f;
    thumbnail->setDisplayGain(gain);
}

void AudioBatchComponent::showPluginProcessingWarning(const juce::String& message)
{
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOk(
            juce::MessageBoxIconType::WarningIcon, "Plugin Processing", message, "OK", this
        ),
        nullptr
    );
}

juce::String AudioBatchComponent::getProcessingBlockedReason() const
{
    if (pluginProcessingInProgress) {
        return "Processing already in progress";
    }

    if (normalizeInProgress) {
        return "Wait for normalization to finish before processing";
    }

    if (isAnalysisInProgress()) {
        return "Wait for analysis to finish before processing";
    }

    return {};
}

juce::Array<juce::File> AudioBatchComponent::releasePreviewBeforeProcessing(
    const std::vector<AudioAnalysisRecord>& records
)
{
    // Always stop playback before processing, since files may be replaced on disk.
    if (transportSource.isPlaying()) {
        transportSource.stop();
    }
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::green());

    juce::Array<juce::File> filesToProcess;
    for (const auto& record : records) {
        filesToProcess.add(record.file);
    }

    if (!filesToProcess.contains(currentAudioFile)) {
        return filesToProcess;
    }

    // Release file handles so the on-disk file can be replaced.
    // Keep currentAudioFile so the result handler can re-load the produced output when processing completes.
    transportSource.setSource(nullptr);
    currentAudioFileSource.reset();
    thumbnail->setURL(juce::URL());
    currentWaveformLoadedFromCache = false;
    startStopButton.setEnabled(false);
    startStopButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMedium());

    return filesToProcess;
}

void AudioBatchComponent::processSelectedRecords()
{
    if (const auto blockedReason = getProcessingBlockedReason(); blockedReason.isNotEmpty()) {
        statusLabel.setText(blockedReason, juce::dontSendNotification);
        return;
    }

    if (pluginChain == nullptr) {
        return;
    }

    // Captures live state from any open editors so the run uses the latest tweaks.
    const auto enabledPlugins = pluginChain->getEnabledPlugins();
    if (enabledPlugins.empty()) {
        showPluginProcessingWarning("The plugin chain is empty. Use the Plugins button to add plugins.");
        return;
    }

    const auto records = getSelectedProcessableRecords();
    if (records.empty()) {
        statusLabel.setText("Select files that can be processed", juce::dontSendNotification);
        return;
    }

    PluginProcessingOptions options;
    options.plugins.reserve(enabledPlugins.size());
    for (const auto& enabledPlugin : enabledPlugins) {
        options.plugins.push_back(enabledPlugin.descriptorRef);
    }
    options.normalizeBeforePlugin = normalizeBeforePluginToggle.getToggleState();
    options.overwriteExisting = forceOverwriteToggle.getToggleState();

    // Pre-instantiate one chain of plugin instances per worker on the message thread.
    const auto workerCount
        = juce::jlimit(1, 4, juce::jmin(static_cast<int>(records.size()), juce::SystemStats::getNumCpus()));

    statusLabel.setText("Loading plugins...", juce::dontSendNotification);

    auto instantiation = instantiateChains(pluginChain->getFormatManager(), enabledPlugins, workerCount);

    if (instantiation.chains.empty() || instantiation.errorMessage.isNotEmpty()) {
        // All-or-nothing: a chain missing any plugin must not run at all.
        showPluginProcessingWarning(
            utils::format(
                "Could not load plugin {}:\n\n{}",
                instantiation.failedPluginName.quoted(),
                instantiation.errorMessage.isNotEmpty() ? instantiation.errorMessage : juce::String("Unknown error")
            )
        );
        statusLabel.setText("Plugin load failed", juce::dontSendNotification);
        return;
    }

    const auto filesToProcess = releasePreviewBeforeProcessing(records);

    processingFailures.clear();
    processedResultsCompleted = 0;
    pluginProcessingInProgress = true;
    updateProcessButtonState();

    processedResultsExpected = pluginCoordinator.start(records, options, std::move(instantiation.chains));

    if (processedResultsExpected <= 0) {
        pluginProcessingInProgress = false;
        updateProcessButtonState();
        updateStatusLabel();
        return;
    }

    markFilesProcessing(filesToProcess, "Processing");
    updateStatusLabel();
}

void AudioBatchComponent::handleProcessingResult(const PluginProcessingResult& result)
{
    ++processedResultsCompleted;
    unmarkFileProcessing(result.originalFullPath);

    if (result.succeeded) {
        auto selectedPaths = getSelectedRecordPaths();

        for (int index = 0; index < selectedPaths.size(); ++index) {
            if (selectedPaths[index] == result.originalFullPath) {
                selectedPaths.set(index, result.analysisRecord.fullPath);
            }
        }

        if (result.notice.isNotEmpty()) {
            operationMessages[result.analysisRecord.fullPath] = {.text = result.notice, .isError = false};
        }

        if (result.notice.isNotEmpty()) {
            operationMessages[result.analysisRecord.fullPath] = {.text = result.notice, .isError = false};
        }

        // Remove any record matching the original path that does not match the new path (in case extension changed).
        std::erase_if(analysisResults, [&result](const AudioAnalysisRecord& record) {
            return record.fullPath == result.originalFullPath && record.fullPath != result.analysisRecord.fullPath;
        });

        if (const auto existingIndex = findRecordIndex(result.analysisRecord.fullPath); existingIndex >= 0) {
            analysisResults[static_cast<std::size_t>(existingIndex)] = result.analysisRecord;
        } else {
            analysisResults.push_back(result.analysisRecord);
        }

        analysisCache.storeAnalysis(result.analysisRecord);
        // Clear any persisted gain for the new output (gain is baked in).
        analysisCache.storeCustomGain(result.analysisRecord.file, 0.0f, false);

        // If the output path differs from the original (extension change), drop the stale cache row.
        if (result.originalFile != result.analysisRecord.file) {
            analysisCache.removeAnalysis(result.originalFile);
        }

        sortResults();
        resultsTable.updateContent();
        updateResultsTableColumnWidths();
        restoreSelectionByPaths(selectedPaths);

        if (currentAudioFile == result.originalFile) {
            currentAudioFile = result.analysisRecord.file;
            showAudioResource(juce::URL(currentAudioFile));
            updateAudioInfo(result.analysisRecord);
            updateGainControlsForSelection();
            updateThumbnailDisplayGain();
        }
    } else {
        processingFailures.add(utils::format("{}: {}", result.fileName, result.errorMessage));
        operationMessages[result.originalFullPath] = {.text = result.errorMessage, .isError = true};
        resultsTable.repaint();
        utils::logError("Plugin processing failed for {}: {}", result.originalFullPath.quoted(), result.errorMessage);
    }

    updateStatusLabel();
}

void AudioBatchComponent::handleProcessingComplete(const int totalFiles)
{
    processedResultsExpected = totalFiles;
    pluginProcessingInProgress = false;
    syncActivityTimer();

    if (pluginChain != nullptr) {
        updateProcessButtonState();
    }

    if (!processingFailures.isEmpty()) {
        juce::String body;
        const auto linesToShow = juce::jmin(8, processingFailures.size());
        for (int i = 0; i < linesToShow; ++i) {
            body << utils::format("- {}", processingFailures[i]) << juce::newLine;
        }
        if (processingFailures.size() > linesToShow) {
            body << utils::format("- ...and {} more", processingFailures.size() - linesToShow);
        }

        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Plugin Processing",
                processingFailures.size() == 1
                    ? processingFailures[0]
                    : utils::format("Some files could not be processed:\n\n{}", body.trimEnd()),
                "OK",
                this
            ),
            nullptr
        );
    }

    if (!currentAudioFile.existsAsFile() && resultsTable.getSelectedRow() >= 0) {
        handleSelectionChanged(resultsTable.getSelectedRow());
    }

    updateStatusLabel();
}
