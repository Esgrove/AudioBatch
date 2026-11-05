/// Real main-view workflows on the JUCE message thread without a desktop peer or audio hardware.
/// Each case owns explicit temporary storage and copied fixtures, uses the view's semantic actions or controls,
/// and pumps bounded event-loop slices until observable state settles.

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisFormatting.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/SpectrogramData.h"
#include "gui/AudioBatchComponent.h"
#include "gui/AudioBatchComponentConstants.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/components/AudioInfoPanel.h"
#include "gui/components/AudioSettingsComponent.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiControlIds.h"
#include "gui/GuiLayout.h"
#include "gui/MainWindow.h"
#include "GuiDriver.h"
#include "normalization/AudioNormalization.h"
#include "plugins/PluginScanner.h"
#include "utils/AppInfo.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
enum class IntegrationWorkflow : std::uint8_t {
    storage,
    openAndSort,
    normalize,
    previewModes,
    previewWidths,
    cancelAndClear,
    restore,
    menuShortcuts,
    manifestControls,
    dropFiles,
    normalizeOptions,
    disabledControls,
};

/// Horizontal offset of row clicks from the left edge of a row, inside its first visible cell.
constexpr int ROW_CLICK_INSET {8};

/// Storage owned by one case, deleted only after its view and all synchronous workers are destroyed.
class IntegrationDirectory final
{
public:
    /// Creates a unique folder without relying on a shared environment variable.
    IntegrationDirectory() :
        directory(
            juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile(utils::format("AudioBatchIntegration-{}", juce::Uuid().toString()))
        )
    {
        if (directory.createDirectory().failed()) {
            throw std::runtime_error("Could not create integration temporary directory");
        }
    }

    /// Removes only the folder this case created.
    ~IntegrationDirectory()
    {
        directory.deleteRecursively();
    }

    IntegrationDirectory(const IntegrationDirectory&) = delete;
    IntegrationDirectory(IntegrationDirectory&&) = delete;
    IntegrationDirectory& operator=(const IntegrationDirectory&) = delete;
    IntegrationDirectory& operator=(IntegrationDirectory&&) = delete;

    /// Returns the root while retaining its ownership in this scope.
    [[nodiscard]] const juce::File& get() const noexcept
    {
        return directory;
    }

private:
    juce::File directory;
};

/// Runs native dispatch, timers and worker callbacks with an absolute deadline, never a timing sleep.
bool dispatchUntil(const std::function<bool()>& complete)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!complete() && std::chrono::steady_clock::now() < deadline) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    }
    return complete();
}

/// Reads visible inspector text, rather than duplicating its formatting logic in the test.
bool containsLabel(const juce::Component& component, const juce::String& text)
{
    std::vector<const juce::Component*> pending(component.getChildren().begin(), component.getChildren().end());
    while (!pending.empty()) {
        const auto* child = pending.back();
        pending.pop_back();
        if (const auto* label = dynamic_cast<const juce::Label*>(child); label != nullptr && label->getText() == text) {
            return true;
        }
        pending.insert(pending.end(), child->getChildren().begin(), child->getChildren().end());
    }
    return false;
}
}  // namespace

/// Friend access is limited to observation and real table/control events, never direct result-handler calls.
class AudioBatchIntegrationTest : public testing::Test
{
public:
    /// Selects one independently discoverable workflow.
    explicit AudioBatchIntegrationTest(const IntegrationWorkflow selectedWorkflow) : workflow(selectedWorkflow) {}

    ~AudioBatchIntegrationTest() override = default;

    AudioBatchIntegrationTest(const AudioBatchIntegrationTest&) = delete;
    AudioBatchIntegrationTest(AudioBatchIntegrationTest&&) = delete;
    AudioBatchIntegrationTest& operator=(const AudioBatchIntegrationTest&) = delete;
    AudioBatchIntegrationTest& operator=(AudioBatchIntegrationTest&&) = delete;

private:
    /// Selects by stable source identity through the actual table callback.
    /// Used only by peerless cases. Driven cases click the row with clickRow instead.
    static bool selectFile(AudioBatchComponent& view, const juce::File& file)
    {
        const auto row = view.analysisResults.findRow(file.getFullPathName());
        if (!row) {
            return false;
        }
        view.resultsTable.selectRow(*row);
        return view.getSelectedRecordPaths().contains(file.getFullPathName());
    }

    /// Clicks the first cell of a file's row through the window peer, with optional modifiers.
    static bool clickRow(
        tests::GuiDriver& driver,
        AudioBatchComponent& view,
        const juce::File& file,
        juce::ModifierKeys modifiers = {}
    )
    {
        const auto row = view.analysisResults.findRow(file.getFullPathName());
        if (!row) {
            return false;
        }
        view.resultsTable.scrollToEnsureRowIsOnscreen(*row);
        auto& table = view.resultsTable;
        const auto rowBounds = table.getRowPosition(*row, true);
        driver.clickAt(table, rowBounds.getCentre().withX(rowBounds.getX() + ROW_CLICK_INSET), modifiers);
        return view.getSelectedRecordPaths().contains(file.getFullPathName());
    }

    /// Clicks a visible column title in the table header through the window peer, with optional modifiers.
    static void clickHeader(
        tests::GuiDriver& driver,
        AudioBatchComponent& view,
        const AudioFileTableColumn column,
        const juce::ModifierKeys modifiers = {}
    )
    {
        auto& header = view.resultsTable.getHeader();
        const auto index = header.getIndexOfColumnId(toColumnId(column), true);
        if (index < 0) {
            throw std::runtime_error("The header column to click is hidden");
        }
        driver.clickAt(header, header.getColumnPosition(index).getCentre(), modifiers);
    }

    /// Toggles a column through the View menu's Columns command handler, which the menu bar invokes.
    /// Popup menus run in separate windows that macOS closes when the test process is not the foreground application,
    /// so the driven tier never opens them and calls the item handler that their choice reaches.
    static void toggleColumn(AudioBatchComponent& view, const AudioFileTableColumn column)
    {
        constexpr int firstItemId {2000};
        const auto menu = view.createColumnsMenu(firstItemId);
        const auto itemId = firstItemId + toColumnId(column);
        bool listed = false;
        for (juce::PopupMenu::MenuItemIterator item(menu); item.next();) {
            listed = listed || item.getItem().itemID == itemId;
        }
        if (!listed) {
            throw std::runtime_error("The Columns menu does not list the column");
        }
        view.handleColumnsMenuItem(itemId, firstItemId);
    }

    /// Right-clicks a file's row through the window peer, opening the row context menu.
    static void rightClickRow(tests::GuiDriver& driver, AudioBatchComponent& view, const juce::File& file)
    {
        const auto row = view.analysisResults.findRow(file.getFullPathName());
        if (!row) {
            throw std::runtime_error("The row to right-click is missing");
        }
        auto& table = view.resultsTable;
        const auto rowBounds = table.getRowPosition(*row, true);
        driver.rightClickAt(table, rowBounds.getCentre().withX(rowBounds.getX() + ROW_CLICK_INSET));
    }

    /// Waits for completed rows, including failures, and the view's normal idle transition.
    static bool waitForRows(AudioBatchComponent& view, const int expectedRows)
    {
        return dispatchUntil([&] {
            return !view.isBackgroundWorkRunning() && view.analysisResults.size() == expectedRows
                && std::ranges::none_of(view.analysisResults, [](const auto& row) {
                       return row.status == AudioAnalysisStatus::pending;
                   });
        });
    }

    /// Checks both waveform readiness and source ownership after asynchronous enrichment.
    static bool waitForWaveform(AudioBatchComponent& view, const juce::File& file)
    {
        return dispatchUntil([&] {
            const auto& thumbnail = view.previewWorkspace.thumbnail;
            const auto& identity = thumbnail.getSourceIdentity();
            return view.preview.getFile() == file && identity && identity->matches(file)
                && thumbnail.saveToCacheData().getSize() > 0;
        });
    }

    [[nodiscard]] AudioBatchComponentConfig configuration() const;

    [[nodiscard]] juce::File copyFixture(const juce::String& name) const;

    void cancelAndClear();
    void disabledControlsRejectInput();
    void dropFilesAndFolders();
    void manifestControls();
    void menuShortcuts();
    void normalizeOptionsFollowControls();
    void normalizeSelection();
    void openAndSort();
    void previewModes();
    void previewWidths();
    void restorePreferencesAndGain();
    void storageIsolation();

    void TestBody() override;

    IntegrationDirectory temporary;
    IntegrationWorkflow workflow;
};

/// Builds per-view configuration with no process-global environment mutation.
AudioBatchComponentConfig AudioBatchIntegrationTest::configuration() const
{
    AudioBatchComponentConfig config;
    config.cacheDatabaseFile = temporary.get().getChildFile("AudioBatch/analysis.db");
    config.settingsOptions = utils::createApplicationSettingsOptions(temporary.get());
    return config;
}

/// Copies a small committed fixture before any view can modify it.
juce::File AudioBatchIntegrationTest::copyFixture(const juce::String& name) const
{
    const auto source = juce::File(AUDIOBATCH_TEST_FIXTURES_DIR).getChildFile(name);
    const auto target = temporary.get().getChildFile(name);
    if (!source.copyFileTo(target)) {
        throw std::runtime_error(utils::format("Could not copy fixture {}", name).toStdString());
    }
    return target;
}

/// Pins invalid override rejection and the exact isolated settings/plugin directory without touching native paths.
void AudioBatchIntegrationTest::storageIsolation()
{
    EXPECT_FALSE(utils::parseDataRootOverride({}));
    EXPECT_THROW(juce::ignoreUnused(utils::parseDataRootOverride("relative/root")), std::invalid_argument);
    EXPECT_THROW(juce::ignoreUnused(utils::parseDataRootOverride("~/root")), std::invalid_argument);
    EXPECT_THROW(juce::ignoreUnused(utils::parseDataRootOverride("   ")), std::invalid_argument);
    const auto root = utils::parseDataRootOverride(temporary.get().getFullPathName());
    if (!root) {
        ADD_FAILURE() << "The explicit absolute root was not accepted";
        return;
    }
    EXPECT_EQ(*root, temporary.get());
    EXPECT_EQ(utils::defaultAnalysisDatabaseFile(root), temporary.get().getChildFile("AudioBatch/analysis.db"));
    const auto isolatedLock = utils::scopedPluginScanLockName(pluginScanLockName, root);
    EXPECT_NE(isolatedLock, juce::String(pluginScanLockName));
    EXPECT_EQ(isolatedLock, utils::scopedPluginScanLockName(pluginScanLockName, root));
    EXPECT_NE(isolatedLock, utils::scopedPluginScanLockName(pluginScanLockName, temporary.get().getChildFile("other")));
    EXPECT_EQ(utils::scopedPluginScanLockName(pluginScanLockName, std::nullopt), juce::String(pluginScanLockName));
    const auto logger = utils::createDefaultLogger("audiobatch", "scanner", root);
    EXPECT_EQ(logger->getLogFile(), temporary.get().getChildFile("AudioBatch/logs/scanner.log"));
    EXPECT_TRUE(logger->getLogFile().existsAsFile());
    const auto options = utils::createApplicationSettingsOptions(root);
    EXPECT_EQ(options.getDefaultFile(), temporary.get().getChildFile("AudioBatch/settings/AudioBatch.settings"));
    AudioBatchComponent view(configuration());
    EXPECT_EQ(view.analysisCache.getDatabaseFile(), temporary.get().getChildFile("AudioBatch/analysis.db"));
    EXPECT_EQ(view.pluginAppProperties.getUserSettings()->getFile(), options.getDefaultFile());
    EXPECT_FALSE(view.previewPlayer.getOutputStatus().open);
    EXPECT_EQ(view.getPeer(), nullptr);
    view.showAudioSettingsWindow();
    EXPECT_EQ(view.settingsWindow.getComponent(), nullptr);
    juce::File defaultRoot;
    {
        AudioBatchComponent defaultView;
        defaultRoot = defaultView.configuration.testDirectory->getFile();
        EXPECT_TRUE(defaultRoot.isDirectory());
        EXPECT_EQ(defaultView.analysisCache.getDatabaseFile(), defaultRoot.getChildFile("AudioBatch/analysis.db"));
        EXPECT_EQ(
            defaultView.pluginAppProperties.getUserSettings()->getFile(),
            defaultRoot.getChildFile("AudioBatch/settings/AudioBatch.settings")
        );
        EXPECT_FALSE(defaultView.previewPlayer.getOutputStatus().open);
        EXPECT_NE(defaultRoot, temporary.get());
    }
    EXPECT_FALSE(defaultRoot.exists());
}

/// Opens valid and broken files and waits for real analysis.
/// Sorting keeps the selected inspector and waveform identity intact.
void AudioBatchIntegrationTest::openAndSort()
{
    const auto valid = copyFixture("serato_rainbow_stereo_44100_16.flac");
    const auto other = copyFixture("sine_mono_44100_16.wav");
    const auto broken = copyFixture("broken.wav");
    AudioBatchComponent view(configuration());
    tests::GuiDriver driver(view);
    view.openPaths({valid.getFullPathName(), broken.getFullPathName(), other.getFullPathName()}, false);
    ASSERT_TRUE(waitForRows(view, 3));
    const auto& header = view.resultsTable.getHeader();
    EXPECT_TRUE(header.isColumnVisible(toColumnId(AudioFileTableColumn::folder)));
    EXPECT_FALSE(header.isColumnVisible(toColumnId(AudioFileTableColumn::path)));
    for (const auto column :
         {
             AudioFileTableColumn::plr,
             AudioFileTableColumn::psr,
             AudioFileTableColumn::crestFactor,
             AudioFileTableColumn::dcOffset,
             AudioFileTableColumn::clips,
             AudioFileTableColumn::correlation,
         })
    {
        EXPECT_FALSE(header.isColumnVisible(toColumnId(column)));
    }
    for (const auto column :
         {
             AudioFileTableColumn::plr,
             AudioFileTableColumn::psr,
             AudioFileTableColumn::crestFactor,
             AudioFileTableColumn::dcOffset,
             AudioFileTableColumn::correlation,
         })
    {
        toggleColumn(view, column);
        EXPECT_TRUE(header.isColumnVisible(toColumnId(column)));
    }
    toggleColumn(view, AudioFileTableColumn::clips);
    EXPECT_TRUE(header.isColumnVisible(toColumnId(AudioFileTableColumn::clips)));
    toggleColumn(view, AudioFileTableColumn::path);
    EXPECT_TRUE(header.isColumnVisible(toColumnId(AudioFileTableColumn::path)));
    for (const int width : {2200, 800}) {
        view.setSize(width, 800);
        view.updateResultsTableColumnWidths();
        int total = 0;
        int fixed = 0;
        for (int index = 0; index < header.getNumColumns(true); ++index) {
            const int columnId = header.getColumnIdOfIndex(index, true);
            total += header.getColumnWidth(columnId);
            if (columnId != toColumnId(AudioFileTableColumn::name)
                && columnId != toColumnId(AudioFileTableColumn::folder)
                && columnId != toColumnId(AudioFileTableColumn::path))
            {
                fixed += header.getColumnWidth(columnId);
            }
        }
        const int minimum = layout::table::nameColumnMinimumWidth + 2 * layout::table::pathColumnMinimumWidth;
        EXPECT_NEAR(total, std::max(view.resultsTable.getVisibleContentWidth(), fixed + minimum), 3);
        EXPECT_GE(header.getColumnWidth(toColumnId(AudioFileTableColumn::name)), layout::table::nameColumnMinimumWidth);
        EXPECT_GE(
            header.getColumnWidth(toColumnId(AudioFileTableColumn::folder)), layout::table::pathColumnMinimumWidth
        );
        EXPECT_GE(header.getColumnWidth(toColumnId(AudioFileTableColumn::path)), layout::table::pathColumnMinimumWidth);
    }
    view.setSize(1920, 1080);
    tests::GuiDriver::settle();
    EXPECT_FALSE(view.createColumnsMenu(2000).getNumItems() == 0);
    EXPECT_EQ(view.createOutputDeviceMenu(3000).getNumItems(), 0);
    ASSERT_TRUE(clickRow(driver, view, other));
    driver.press(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0));
    EXPECT_EQ(view.getSelectedRecordPaths().size(), 3);
    view.applyCustomGainToSelection(Decibels(2.0f));
    view.clearSelectedCustomGain();
    for (const auto& record : view.analysisResults) {
        EXPECT_FALSE(record.customGain.has_value());
    }
    view.previewPlayer.setPosition(0.25);
    driver.press(juce::KeyPress(juce::KeyPress::escapeKey));
    EXPECT_FALSE(view.previewPlayer.isPlaying());
    EXPECT_NEAR(view.previewPlayer.getCurrentPosition(), 0.0, 0.00001);
    clickHeader(driver, view, AudioFileTableColumn::folder);
    ASSERT_TRUE(dispatchUntil([&] { return view.sortState.primaryColumn == AudioFileTableColumn::folder; }));
    AnalysisCache completedArtifacts(temporary.get().getChildFile("AudioBatch/analysis.db"));
    EXPECT_TRUE(completedArtifacts.getWaveformData(valid).has_value());
    EXPECT_NE(completedArtifacts.getSpectrogramData(valid), nullptr);
    EXPECT_TRUE(completedArtifacts.getWaveformData(other).has_value());
    EXPECT_NE(completedArtifacts.getSpectrogramData(other), nullptr);
    const auto brokenRow = view.analysisResults.findRow(broken.getFullPathName());
    if (!brokenRow) {
        ADD_FAILURE() << "The broken input was lost instead of producing an error row";
        return;
    }
    EXPECT_TRUE(view.analysisResults[*brokenRow].hasError());
    ASSERT_TRUE(clickRow(driver, view, valid));
    EXPECT_EQ(view.getSelectedRecordPaths(), juce::StringArray(valid.getFullPathName()));
    ASSERT_TRUE(waitForWaveform(view, valid));
    ASSERT_TRUE(dispatchUntil([&] {
        const auto metadata = view.preview.getSeratoData();
        return metadata && metadata->overview.has_value();
    }));
    const auto metadataBeforeSort = view.preview.getSeratoData();
    if (!metadataBeforeSort || !metadataBeforeSort->overview) {
        ADD_FAILURE() << "The selected Serato metadata did not load";
        return;
    }
    EXPECT_EQ(metadataBeforeSort->overview->slices.size(), 240U);
    EXPECT_TRUE(containsLabel(view.previewWorkspace.audioInfo, valid.getFileName()));
    const auto selectedRow = view.analysisResults.findRow(valid.getFullPathName());
    if (!selectedRow) {
        ADD_FAILURE() << "The selected valid result disappeared before inspecting its source metrics";
        return;
    }
    const auto& metrics = view.analysisResults[*selectedRow].signalMetrics;
    EXPECT_EQ(
        view.fileTableModel.getCellTooltip(*selectedRow, toColumnId(AudioFileTableColumn::clips)),
        formatClipCounts(metrics)
    );
    EXPECT_EQ(
        view.fileTableModel.getCellTooltip(*selectedRow, toColumnId(AudioFileTableColumn::dcOffset)),
        formatDcOffsets(metrics)
    );
    EXPECT_EQ(
        AudioFileTableModel::getColumnDescription(AudioFileTableColumn::psr),
        juce::String("Source whole-file true peak minus maximum short-term loudness, dB")
    );
    EXPECT_EQ(
        AudioFileTableModel::getColumnDescription(AudioFileTableColumn::clips),
        juce::String("Source samples with magnitude at least one, summed across channels")
    );
    EXPECT_TRUE(containsLabel(
        view.previewWorkspace.audioInfo,
        utils::format(
            "PLR {} dB · PSR {} dB · Crest {} dB",
            formatMetric(metrics.plrDb),
            formatMetric(metrics.psrDb),
            formatMetric(metrics.crestFactorDb)
        )
    ));
    EXPECT_TRUE(containsLabel(
        view.previewWorkspace.audioInfo, utils::format("{} total, {}", metrics.totalClips(), formatClipCounts(metrics))
    ));
    clickHeader(driver, view, AudioFileTableColumn::name);
    ASSERT_TRUE(dispatchUntil([&] { return view.sortState.primaryColumn == AudioFileTableColumn::name; }));
    clickHeader(driver, view, AudioFileTableColumn::folder, juce::ModifierKeys::commandModifier);
    EXPECT_EQ(view.sortState.primaryColumn, AudioFileTableColumn::name);
    EXPECT_EQ(view.sortState.secondaryColumn, AudioFileTableColumn::folder);
    EXPECT_EQ(view.getSelectedRecordPaths(), juce::StringArray(valid.getFullPathName()));
    EXPECT_EQ(view.preview.getFile(), valid);
    const auto metadataAfterSort = view.preview.getSeratoData();
    if (!metadataAfterSort || !metadataAfterSort->overview) {
        ADD_FAILURE() << "Sorting discarded the selected Serato metadata";
        return;
    }
    EXPECT_EQ(metadataAfterSort->overview->slices.size(), 240U);
    EXPECT_TRUE(containsLabel(view.previewWorkspace.audioInfo, valid.getFileName()));
    ASSERT_TRUE(waitForWaveform(view, valid));
    EXPECT_FALSE(view.previewPlayer.getOutputStatus().open);
}

/// Converts only a copied WAV through the real Normalize control and follows the committed output everywhere.
void AudioBatchIntegrationTest::normalizeSelection()
{
    const auto source = copyFixture("sine_stereo_44100_16.wav");
    const auto output = source.withFileExtension(".aif");
    AudioBatchComponent view(configuration());
    tests::GuiDriver driver(view);
    EXPECT_FALSE(driver.find(control_ids::NORMALIZE_SELECTED).enabled);
    view.openPaths({source.getFullPathName()}, false);
    ASSERT_TRUE(waitForRows(view, 1));
    ASSERT_TRUE(clickRow(driver, view, source));
    ASSERT_TRUE(waitForWaveform(view, source));
    ASSERT_TRUE(driver.find(control_ids::NORMALIZE_SELECTED).enabled);
    driver.click(control_ids::NORMALIZE_SELECTED);
    ASSERT_TRUE(dispatchUntil([&] {
        return !view.isBackgroundWorkRunning() && view.analysisResults.findRow(output.getFullPathName()).has_value();
    }));
    EXPECT_TRUE(output.existsAsFile());
    EXPECT_FALSE(source.exists());
    EXPECT_EQ(view.analysisResults.size(), 1);
    EXPECT_FALSE(view.analysisResults.findRow(source.getFullPathName()));
    EXPECT_EQ(view.getSelectedRecordPaths(), juce::StringArray(output.getFullPathName()));
    EXPECT_EQ(view.preview.getFile(), output);
    ASSERT_TRUE(waitForWaveform(view, output));
    EXPECT_TRUE(containsLabel(view.previewWorkspace.audioInfo, output.getFileName()));
    const auto outputRow = view.analysisResults.findRow(output.getFullPathName());
    if (!outputRow) {
        ADD_FAILURE() << "The output row was not retained";
        return;
    }
    const auto& record = view.analysisResults[*outputRow];
    EXPECT_TRUE(record.isReady());
    EXPECT_NEAR(
        static_cast<double>(juce::Decibels::gainToDecibels(std::abs(record.overallPeak))),
        AudioNormalizationOptions::defaultTargetPeakDbfs,
        0.01
    );
    EXPECT_FALSE(view.previewPlayer.getOutputStatus().open);
}

/// Exercises canonical mode actions against the real selected preview, its cache, and persisted preferences.
void AudioBatchIntegrationTest::previewModes()
{
    const auto file = copyFixture("serato_rainbow_stereo_44100_16.flac");
    {
        AudioInfoPanel panel;
        auto record = analyzeFile(file);
        record.customGain = Decibels {2.0f};
        const auto hardClip = measureGainAdjustedLoudness(file, *record.customGain);
        ASSERT_TRUE(hardClip.has_value());
        if (!hardClip) {
            return;
        }
        const auto estimated
            = estimateLimiterIntegratedLoudness(record.integratedLufs, *record.customGain, hardClip->integratedLufs);
        ASSERT_TRUE(estimated.has_value());
        panel.showRecord(record, {});
        ASSERT_TRUE(dispatchUntil([&] { return containsLabel(panel, utils::format("(≈{:.1f})", *estimated)); }));
        EXPECT_TRUE(containsLabel(panel, utils::format("(≈{:.1f})", hardClip->maxShortTermLufs)));
        EXPECT_TRUE(containsLabel(
            panel, utils::format("({:+.1f})", 20.0 * std::log10(static_cast<double>(record.overallPeak)) + 2.0)
        ));
        EXPECT_TRUE(containsLabel(panel, utils::format("({:+.1f})", 20.0 * std::log10(record.overallTruePeak) + 2.0)));
        record.customGain = Decibels {12.0f};
        panel.showRecord(record, {});
        panel.clear();
        // Let an unrelated replacement request finish, proving cleared work cannot repopulate the old selection.
        auto replacement = analyzeFile(copyFixture("silence_stereo_44100_16.wav"));
        replacement.customGain = Decibels {3.0f};
        panel.showRecord(replacement, {});
        ASSERT_TRUE(dispatchUntil([&] { return containsLabel(panel, juce::String::fromUTF8("(≈-INF)")); }));
        panel.showRecord(record, {});
        record.status = AudioAnalysisStatus::failed;
        panel.showRecord(record, {});
        EXPECT_FALSE(containsLabel(panel, juce::String::fromUTF8("(≈-INF)")));
        panel.clear();
    }
    {
        AudioBatchComponent view(configuration());
        tests::GuiDriver driver(view);
        EXPECT_FALSE(driver.find(control_ids::GAIN_TRACK).enabled);
        view.openPaths({file.getFullPathName()}, false);
        ASSERT_TRUE(waitForRows(view, 1));
        ASSERT_TRUE(clickRow(driver, view, file));
        ASSERT_TRUE(waitForWaveform(view, file));
        AnalysisCache observer(temporary.get().getChildFile("AudioBatch/analysis.db"));
        ASSERT_TRUE(dispatchUntil([&] {
            return observer.getSpectrogramData(file) != nullptr
                && view.previewWorkspace.thumbnail.hasSpectrogramRaster();
        }));
        const auto spectrum = observer.getSpectrogramData(file);
        ASSERT_NE(spectrum, nullptr);
        EXPECT_TRUE(isValidSpectrogramData(*spectrum));
        driver.setValue(control_ids::ZOOM_TRACK, 200.0);
        driver.setValue(control_ids::GAIN_TRACK, -3.0);
        EXPECT_TRUE(containsLabel(view.previewWorkspace.audioInfo, "(-9.0)"));
        const auto selectedRow = view.analysisResults.findRow(file.getFullPathName());
        ASSERT_TRUE(selectedRow.has_value());
        if (!selectedRow) {
            return;
        }
        const auto row = *selectedRow;
        const auto measured = view.analysisResults[row];
        EXPECT_TRUE(view.isCustomGainEstimatesEnabled());
        view.toggleCustomGainEstimates();
        EXPECT_FALSE(view.isCustomGainEstimatesEnabled());
        EXPECT_FALSE(containsLabel(view.previewWorkspace.audioInfo, "(-9.0)"));
        EXPECT_TRUE(
            containsLabel(view.previewWorkspace.audioInfo, formatPeakCompact(static_cast<double>(measured.overallPeak)))
        );
        EXPECT_TRUE(containsLabel(view.previewWorkspace.audioInfo, formatLoudnessCompact(measured.integratedLufs)));
        const auto retainedGain = view.analysisResults[row].customGain;
        ASSERT_TRUE(retainedGain.has_value());
        ASSERT_TRUE(measured.customGain.has_value());
        if (retainedGain && measured.customGain) {
            EXPECT_EQ(retainedGain->value, measured.customGain->value);
        }
        EXPECT_EQ(view.analysisResults[row].overallPeak, measured.overallPeak);
        EXPECT_EQ(view.analysisResults[row].integratedLufs, measured.integratedLufs);
        EXPECT_FALSE(loadAudioPreviewPreferences(*view.pluginAppProperties.getUserSettings()).showGainAdjustedMetrics);
        view.toggleCustomGainEstimates();
        EXPECT_TRUE(view.isCustomGainEstimatesEnabled());
        EXPECT_TRUE(loadAudioPreviewPreferences(*view.pluginAppProperties.getUserSettings()).showGainAdjustedMetrics);
        EXPECT_TRUE(containsLabel(view.previewWorkspace.audioInfo, "(-9.0)"));
        auto preferences = view.previewPreferences;
        // Successive edits replace the debounced request without displaying the naive gain-only value.
        driver.setValue(control_ids::GAIN_TRACK, 12.0);
        EXPECT_FALSE(containsLabel(view.previewWorkspace.audioInfo, juce::String::fromUTF8("(≈-1.0)")));
        driver.setValue(control_ids::GAIN_TRACK, 9.0);
        const auto adjusted = measureGainAdjustedLoudness(file, Decibels {9.0f});
        ASSERT_TRUE(adjusted.has_value());
        if (!adjusted) {
            return;
        }
        const auto adjustedText = utils::format("(≈{:.1f})", adjusted->integratedLufs);
        ASSERT_TRUE(dispatchUntil([&] { return containsLabel(view.previewWorkspace.audioInfo, adjustedText); }));
        preferences.showGainAdjustedMetrics = false;
        view.setPreviewPreferences(preferences);
        EXPECT_FALSE(containsLabel(view.previewWorkspace.audioInfo, adjustedText));
        driver.setValue(control_ids::GAIN_TRACK, -3.0);
        preferences.showGainAdjustedMetrics = true;
        view.setPreviewPreferences(preferences);
        const auto attenuated = measureGainAdjustedLoudness(file, Decibels {-3.0f});
        ASSERT_TRUE(attenuated.has_value());
        ASSERT_TRUE(dispatchUntil([&] {
            return containsLabel(
                view.previewWorkspace.audioInfo, utils::format("(≈{:.1f})", attenuated->integratedLufs)
            );
        }));
        view.toggleLoudnessGraph();
        const auto waveform = view.previewWorkspace.thumbnail.saveToCacheData();
        constexpr std::array modes {
            PreviewViewMode::waveform,
            PreviewViewMode::spectrogram,
            PreviewViewMode::sideBySide,
            PreviewViewMode::stacked,
        };
        for (const auto mode : modes) {
            view.setPreviewViewMode(mode);
            EXPECT_EQ(view.getPreviewViewMode(), mode);
            EXPECT_EQ(view.preview.getFile(), file);
            EXPECT_EQ(view.getSelectedRecordPaths(), juce::StringArray(file.getFullPathName()));
            EXPECT_EQ(view.previewWorkspace.thumbnail.saveToCacheData(), waveform);
            EXPECT_NEAR(view.previewWorkspace.zoomSlider.getValue(), 200.0, 0.001);
            EXPECT_NEAR(view.previewWorkspace.gainSlider.getValue(), -3.0, 0.001);
            EXPECT_TRUE(view.isLoudnessGraphEnabled());
            EXPECT_FALSE(view.previewPlayer.getOutputStatus().open);
            const auto saved = loadAudioPreviewPreferences(*view.pluginAppProperties.getUserSettings());
            EXPECT_EQ(saved.viewMode, mode);
        }
        view.toggleSampleOverloads();
        view.toggleTruePeakOverloads();
        view.toggleOverloadLines();
        EXPECT_FALSE(view.isSampleOverloadVisible());
        EXPECT_FALSE(view.isTruePeakOverloadVisible());
        EXPECT_FALSE(view.areOverloadLinesVisible());
        const auto savedOverloads = loadAudioPreviewPreferences(*view.pluginAppProperties.getUserSettings());
        EXPECT_FALSE(savedOverloads.showSampleOverloads);
        EXPECT_FALSE(savedOverloads.showTruePeakOverloads);
        EXPECT_FALSE(savedOverloads.showOverloadLines);
        driver.click(control_ids::PREVIEW_SECTION);
        EXPECT_EQ(view.getPreviewViewMode(), PreviewViewMode::stacked);
        ASSERT_TRUE(dispatchUntil([&] { return view.previewWorkspace.isCompact(); }));
        driver.click(control_ids::PREVIEW_SECTION);
        EXPECT_EQ(view.getPreviewViewMode(), PreviewViewMode::stacked);
        ASSERT_TRUE(dispatchUntil([&] { return !view.previewWorkspace.isCompact(); }));
    }
    AudioBatchComponent recreated(configuration());
    EXPECT_EQ(recreated.getPreviewViewMode(), PreviewViewMode::stacked);
    EXPECT_FALSE(recreated.isSampleOverloadVisible());
    EXPECT_FALSE(recreated.isTruePeakOverloadVisible());
    EXPECT_FALSE(recreated.areOverloadLinesVisible());
    recreated.openPaths({file.getFullPathName()}, false);
    ASSERT_TRUE(waitForRows(recreated, 1));
    ASSERT_TRUE(selectFile(recreated, file));
    ASSERT_TRUE(waitForWaveform(recreated, file));
    EXPECT_EQ(recreated.getPreviewViewMode(), PreviewViewMode::stacked);
}

/// Drives the actual native divider components and verifies independent responsive persistence through recreation.
void AudioBatchIntegrationTest::previewWidths()
{
    double savedControls = 0.0;
    double savedInformation = 0.0;
    {
        AudioBatchComponent view(configuration());
        tests::GuiDriver driver(view);
        auto& workspace = view.previewWorkspace;
        auto* controlsDivider = driver.find(control_ids::CONTROLS_WIDTH_DIVIDER).component;
        const auto* informationDivider = driver.find(control_ids::INFORMATION_WIDTH_DIVIDER).component;
        const auto dragControls
            = [&](const int distance) { driver.drag(control_ids::CONTROLS_WIDTH_DIVIDER, {distance, 0}); };
        const auto dragInformation
            = [&](const int distance) { driver.drag(control_ids::INFORMATION_WIDTH_DIVIDER, {distance, 0}); };
        EXPECT_TRUE(controlsDivider->getWantsKeyboardFocus());
        EXPECT_TRUE(informationDivider->getWantsKeyboardFocus());
        EXPECT_EQ(controlsDivider->getMouseCursor(), juce::MouseCursor(juce::MouseCursor::LeftRightResizeCursor));
        EXPECT_FALSE(controlsDivider->getTitle().isEmpty());
        EXPECT_EQ(controlsDivider->getWidth(), 10);
        EXPECT_EQ(informationDivider->getWidth(), 10);
        EXPECT_EQ(informationDivider->getX(), workspace.audioInfo.getRight());
        EXPECT_EQ(informationDivider->getRight(), workspace.thumbnail.getX());
        const auto height = workspace.getHeight();
        const auto initialControlsWidth = controlsDivider->getX() - layout::compactPreview::padding;
        const auto initialInformationWidth = workspace.audioInfo.getWidth();
        auto& settings = *view.pluginAppProperties.getUserSettings();
        EXPECT_FALSE(settings.containsKey("previewControlsWidthScale"));
        EXPECT_FALSE(settings.containsKey("previewInformationWidthScale"));
        settings.setValue("unrelatedWidthSetting", "keep me");

        dragControls(24);
        EXPECT_EQ(controlsDivider->getX() - layout::compactPreview::padding, initialControlsWidth + 24);
        EXPECT_EQ(workspace.audioInfo.getWidth(), initialInformationWidth);
        EXPECT_EQ(workspace.getHeight(), height);
        EXPECT_TRUE(settings.containsKey("previewControlsWidthScale"));
        EXPECT_EQ(
            driver.find(control_ids::CONTROLS_WIDTH_DIVIDER).value,
            utils::format("{} pixels", controlsDivider->getX() - layout::compactPreview::padding)
        );
        const auto controlsAfterDrag = controlsDivider->getX();
        const auto storedControls = settings.getDoubleValue("previewControlsWidthScale");
        // Committing one divider saves both scales, so the untouched information panel keeps its default.
        EXPECT_NEAR(settings.getDoubleValue("previewInformationWidthScale"), 0.90, 0.000001);
        dragInformation(20);
        EXPECT_EQ(workspace.audioInfo.getWidth(), initialInformationWidth + 20);
        EXPECT_EQ(controlsDivider->getX(), controlsAfterDrag);
        EXPECT_GT(settings.getDoubleValue("previewInformationWidthScale"), 0.90);
        EXPECT_NEAR(settings.getDoubleValue("previewControlsWidthScale"), storedControls, 0.000001);
        driver.press(control_ids::CONTROLS_WIDTH_DIVIDER, juce::KeyPress(juce::KeyPress::leftKey));
        EXPECT_EQ(controlsDivider->getX(), controlsAfterDrag - 4);
        EXPECT_EQ(workspace.audioInfo.getWidth(), initialInformationWidth + 20);
        EXPECT_EQ(workspace.getHeight(), height);

        driver.doubleClick(control_ids::INFORMATION_WIDTH_DIVIDER);
        EXPECT_NEAR(settings.getDoubleValue("previewInformationWidthScale"), 0.90, 0.000001);
        EXPECT_EQ(controlsDivider->getX(), controlsAfterDrag - 4);
        driver.press(control_ids::INFORMATION_WIDTH_DIVIDER, juce::KeyPress(juce::KeyPress::rightKey));
        const auto informationAfterKey = workspace.audioInfo.getWidth();
        const auto baseline
            = layout::previewColumnBaseline(workspace.getLocalBounds().reduced(layout::compactPreview::padding));
        for (const auto distance : {-1000, 1000}) {
            dragControls(distance);
            EXPECT_EQ(workspace.audioInfo.getWidth(), informationAfterKey);
            EXPECT_EQ(
                controlsDivider->getX() - layout::compactPreview::padding,
                juce::roundToInt(baseline.controls.getWidth() * (distance < 0 ? 0.855 : 1.10))
            );
        }
        driver.doubleClick(control_ids::CONTROLS_WIDTH_DIVIDER);
        EXPECT_NEAR(settings.getDoubleValue("previewControlsWidthScale"), 0.95, 0.000001);
        EXPECT_EQ(workspace.audioInfo.getWidth(), informationAfterKey);
        driver.press(control_ids::CONTROLS_WIDTH_DIVIDER, juce::KeyPress(juce::KeyPress::leftKey));
        const auto controlsBeforeInformationBounds = controlsDivider->getX();
        for (const auto distance : {-1000, 1000}) {
            dragInformation(distance);
            EXPECT_EQ(controlsDivider->getX(), controlsBeforeInformationBounds);
            EXPECT_EQ(
                workspace.audioInfo.getWidth(),
                juce::roundToInt(baseline.information.getWidth() * (distance < 0 ? 0.81 : 1.10))
            );
        }
        driver.doubleClick(control_ids::INFORMATION_WIDTH_DIVIDER);
        driver.press(control_ids::INFORMATION_WIDTH_DIVIDER, juce::KeyPress(juce::KeyPress::rightKey));
        savedControls = settings.getDoubleValue("previewControlsWidthScale");
        savedInformation = settings.getDoubleValue("previewInformationWidthScale");
        const auto fullWidth = workspace.getWidth();
        const auto informationBeforeClamp = workspace.audioInfo.getWidth();
        workspace.setSize(300, height);
        EXPECT_GE(workspace.thumbnail.getWidth(), 140);
        EXPECT_NEAR(settings.getDoubleValue("previewControlsWidthScale"), savedControls, 0.000001);
        EXPECT_NEAR(settings.getDoubleValue("previewInformationWidthScale"), savedInformation, 0.000001);
        workspace.setSize(fullWidth, height);
        EXPECT_EQ(workspace.audioInfo.getWidth(), informationBeforeClamp);
        workspace.setCompact(true);
        EXPECT_FALSE(controlsDivider->isVisible());
        EXPECT_FALSE(informationDivider->isVisible());
        const auto compact
            = layout::previewColumns(workspace.getLocalBounds().reduced(layout::compactPreview::padding), false);
        EXPECT_EQ(workspace.startStopButton.getX(), layout::compactPreview::padding + 14 + 6);
        EXPECT_EQ(workspace.processButton.getRight(), layout::compactPreview::padding + compact.controls.getWidth());
        workspace.setCompact(false);
        EXPECT_TRUE(controlsDivider->isVisible());
        EXPECT_TRUE(informationDivider->isVisible());
        EXPECT_EQ(workspace.audioInfo.getWidth(), informationBeforeClamp);
        EXPECT_NEAR(settings.getDoubleValue("previewControlsWidthScale"), savedControls, 0.000001);
        EXPECT_NEAR(settings.getDoubleValue("previewInformationWidthScale"), savedInformation, 0.000001);
        EXPECT_EQ(settings.getValue("unrelatedWidthSetting"), juce::String("keep me"));
    }
    AudioBatchComponent recreated(configuration());
    recreated.setSize(1300, 900);
    const auto& workspace = recreated.previewWorkspace;
    const auto expected = layout::previewColumns(
        workspace.getLocalBounds().reduced(layout::compactPreview::padding), true, savedControls, savedInformation
    );
    EXPECT_EQ(workspace.audioInfo.getBounds(), expected.information);
    EXPECT_EQ(workspace.thumbnail.getBounds(), expected.waveform);
    EXPECT_FALSE(recreated.previewPlayer.getOutputStatus().open);
}

/// Cancels and clears before queued delivery.
/// Completes a new load to expose stale callbacks deterministically.
void AudioBatchIntegrationTest::cancelAndClear()
{
    const auto first = copyFixture("sine_stereo_44100_16.wav");
    const auto second = copyFixture("sine_mono_44100_16.wav");
    AudioBatchComponent view(configuration());
    view.openPaths({first.getFullPathName()}, false);
    ASSERT_TRUE(view.isBackgroundWorkRunning());
    view.cancelCurrentWork();
    EXPECT_FALSE(view.isBackgroundWorkRunning());
    view.clearAllRecords();
    EXPECT_TRUE(view.analysisResults.empty());
    view.openPaths({first.getFullPathName()}, false);
    view.clearAllRecords();
    view.openPaths({second.getFullPathName()}, false);
    ASSERT_TRUE(waitForRows(view, 1));
    ASSERT_TRUE(selectFile(view, second));
    EXPECT_EQ(view.getPeer(), nullptr);
    ASSERT_TRUE(waitForWaveform(view, second));
    EXPECT_FALSE(view.analysisResults.findRow(first.getFullPathName()));
    EXPECT_EQ(view.preview.getFile(), second);
    EXPECT_EQ(view.getSelectedRecordPaths(), juce::StringArray(second.getFullPathName()));
    view.clearAllRecords();
    auto delivered = std::make_shared<bool>(false);
    juce::MessageManager::callAsync([delivered] { *delivered = true; });
    ASSERT_TRUE(dispatchUntil([&] { return *delivered; }));
    EXPECT_TRUE(view.analysisResults.empty());
    EXPECT_EQ(view.preview.getFile(), juce::File());
    EXPECT_FALSE(view.previewWorkspace.thumbnail.getSourceIdentity());
}

/// Persists an explicit slider edit and a real preference action, then restores them in a new view instance.
void AudioBatchIntegrationTest::restorePreferencesAndGain()
{
    const auto file = copyFixture("sine_stereo_44100_16.wav");
    bool expectedPreference = false;
    {
        AudioBatchComponent view(configuration());
        tests::GuiDriver driver(view);
        view.openPaths({file.getFullPathName()}, false);
        ASSERT_TRUE(waitForRows(view, 1));
        ASSERT_TRUE(clickRow(driver, view, file));
        ASSERT_TRUE(driver.find(control_ids::GAIN_TRACK).enabled);
        // Each wheel notch steps the gain by exactly one interval, and a double click resets it.
        const auto step = view.previewWorkspace.gainSlider.getInterval();
        driver.scroll(control_ids::GAIN_TRACK, -0.5f);
        driver.scroll(control_ids::GAIN_TRACK, -0.5f);
        EXPECT_NEAR(view.previewWorkspace.gainSlider.getValue(), -2.0 * step, 0.0001);
        driver.doubleClick(control_ids::GAIN_TRACK);
        EXPECT_NEAR(view.previewWorkspace.gainSlider.getValue(), 0.0, 0.0001);
        driver.setValue(control_ids::GAIN_TRACK, -3.0);
        EXPECT_EQ(driver.find(control_ids::GAIN_TRACK).value, view.previewWorkspace.gainSlider.getTextFromValue(-3.0));
        view.toggleLoudnessGraph();
        expectedPreference = view.isLoudnessGraphEnabled();
        AnalysisCache observer(temporary.get().getChildFile("AudioBatch/analysis.db"));
        ASSERT_TRUE(dispatchUntil([&] {
            const auto stored = observer.getAnalysis(file);
            return stored && stored->customGain && juce::approximatelyEqual(stored->customGain->value, -3.0f);
        }));
    }
    AudioBatchComponent recreated(configuration());
    EXPECT_EQ(recreated.isLoudnessGraphEnabled(), expectedPreference);
    recreated.openPaths({file.getFullPathName()}, false);
    ASSERT_TRUE(waitForRows(recreated, 1));
    ASSERT_TRUE(selectFile(recreated, file));
    const auto restoredRow = recreated.analysisResults.findRow(file.getFullPathName());
    if (!restoredRow) {
        ADD_FAILURE() << "The restored file was not loaded";
        return;
    }
    const auto& record = recreated.analysisResults[*restoredRow];
    if (!record.customGain) {
        ADD_FAILURE() << "The explicit custom gain was not restored";
        return;
    }
    EXPECT_NEAR(record.customGain->value, -3.0f, 0.001f);
    EXPECT_NEAR(recreated.previewWorkspace.gainSlider.getValue(), -3.0, 0.001);
    EXPECT_FALSE(recreated.previewPlayer.getOutputStatus().open);
}

/// Builds the real, never shown main window and checks that its command shortcuts reach the shared view actions.
/// The key mappings come from MainWindow::getCommandInfo, so this pins the menu shortcuts both apps document.
void AudioBatchIntegrationTest::menuShortcuts()
{
    MainWindow window("AudioBatch");
    auto& mappings = *window.commandManager.getKeyMappings();
    const auto command = [&](const juce::KeyPress& key) {
        return window.commandManager.getNameOfCommand(mappings.findCommandForKeyPress(key));
    };
    const juce::ModifierKeys primary {juce::ModifierKeys::commandModifier};
    EXPECT_EQ(command(juce::KeyPress('o', primary.withFlags(juce::ModifierKeys::shiftModifier), 0)), "Add Folder...");
    EXPECT_EQ(command(juce::KeyPress('p', primary, 0)), "Audio Settings...");
    EXPECT_EQ(command(juce::KeyPress('a', primary, 0)), "Select All Files");
    EXPECT_EQ(command(juce::KeyPress(juce::KeyPress::escapeKey)), "Stop Playback");
    EXPECT_EQ(command(juce::KeyPress('o', primary, 0)), "Choose Folder...");

    const auto file = copyFixture("sine_mono_44100_16.wav");
    auto& view = window.getAudioBatch();
    view.openPaths({file.getFullPathName()}, false);
    ASSERT_TRUE(waitForRows(view, 1));
    view.resultsTable.deselectAllRows();
    EXPECT_TRUE(mappings.keyPressed(juce::KeyPress('a', primary, 0), &view));
    EXPECT_TRUE(dispatchUntil([&] { return view.getSelectedRecordPaths().size() == 1; }));
}

/// Places the real view in a desktop window at the manifest size
/// and requires every shared control to exist exactly once with its shared role and,
/// where the manifest names one, its accessibility title.
void AudioBatchIntegrationTest::manifestControls()
{
    const auto manifestFile = juce::File(AUDIOBATCH_TEST_FIXTURES_DIR).getSiblingFile("tests/gui_controls.json");
    const auto manifest = juce::JSON::parse(manifestFile);
    ASSERT_TRUE(manifest.isObject()) << manifestFile.getFullPathName();
    const auto* window = manifest["window"].getDynamicObject();
    ASSERT_NE(window, nullptr);
    AudioBatchComponent view(configuration());
    const tests::GuiDriver driver(
        view, {0, 0, static_cast<int>(window->getProperty("width")), static_cast<int>(window->getProperty("height"))}
    );
    const auto& roles = manifest["roles"];
    const auto* controls = manifest["controls"].getArray();
    ASSERT_NE(controls, nullptr);
    ASSERT_FALSE(controls->isEmpty());
    for (const auto& entry : *controls) {
        const auto controlId = entry["id"].toString();
        const auto role = entry["role"].toString();
        SCOPED_TRACE(controlId.toStdString());
        const auto matches = driver.findAll(controlId);
        ASSERT_EQ(matches.size(), 1U) << "Registered ids: " << driver.registeredIds().joinIntoString(", ");
        const auto& control = matches.front();
        ASSERT_TRUE(control.role.has_value());
        const auto* nativeRoles = roles[juce::Identifier(role)]["juce"].getArray();
        ASSERT_NE(nativeRoles, nullptr) << role;
        if (control.role) {
            EXPECT_TRUE(nativeRoles->contains(tests::GuiDriver::roleName(*control.role)))
                << "Native role " << tests::GuiDriver::roleName(*control.role);
        }
        if (entry.hasProperty("label")) {
            EXPECT_EQ(control.title, entry["label"].toString());
        }
    }
}

/// Drops an unsupported file, a nested folder, and then an audio file through the window peer's drag and drop,
/// matching the Rust `integration_drop_files_and_folders` test.
void AudioBatchIntegrationTest::dropFilesAndFolders()
{
    const auto first = copyFixture("sine_stereo_44100_16.wav");
    const auto nested = temporary.get().getChildFile("nested/inner");
    ASSERT_TRUE(nested.createDirectory().wasOk());
    const auto second = nested.getChildFile("second.wav");
    ASSERT_TRUE(juce::File(AUDIOBATCH_TEST_FIXTURES_DIR).getChildFile("sine_mono_44100_16.wav").copyFileTo(second));
    const auto unsupported = temporary.get().getChildFile("notes.txt");
    ASSERT_TRUE(unsupported.replaceWithText("not audio"));
    AudioBatchComponent view(configuration());
    const tests::GuiDriver driver(view);
    const auto dropPoint = view.resultsTable.getBounds().getCentre();

    driver.dropFiles({unsupported.getFullPathName()}, dropPoint);
    EXPECT_TRUE(view.analysisResults.empty());

    driver.dropFiles({temporary.get().getChildFile("nested").getFullPathName()}, dropPoint);
    ASSERT_TRUE(waitForRows(view, 1));
    EXPECT_TRUE(view.analysisResults.findRow(second.getFullPathName()).has_value());

    driver.dropFiles({first.getFullPathName()}, dropPoint);
    ASSERT_TRUE(waitForRows(view, 2));
    EXPECT_TRUE(view.analysisResults.findRow(first.getFullPathName()).has_value());
    EXPECT_TRUE(view.analysisResults.findRow(second.getFullPathName()).has_value());
}

/// Switches the normalization mode and toggles the options through their controls,
/// matching the Rust `integration_normalize_options_follow_controls` test.
void AudioBatchIntegrationTest::normalizeOptionsFollowControls()
{
    AudioBatchComponent view(configuration());
    tests::GuiDriver driver(view);
    const auto& workspace = view.previewWorkspace;
    EXPECT_FALSE(driver.find(control_ids::PROCESS_NORMALIZE_FIRST).checked);
    EXPECT_FALSE(driver.find(control_ids::NORMALIZE_PEAK).enabled);
    driver.click(control_ids::PROCESS_NORMALIZE_FIRST);
    EXPECT_TRUE(workspace.normalizeBeforePluginToggle.getToggleState());
    EXPECT_TRUE(driver.find(control_ids::NORMALIZE_PEAK).enabled);
    driver.click(control_ids::NORMALIZE_LOUDNESS);
    driver.click(control_ids::NORMALIZE_OVERWRITE);
    EXPECT_TRUE(driver.find(control_ids::NORMALIZE_OVERWRITE).checked);
    EXPECT_TRUE(workspace.forceOverwriteToggle.getToggleState());
    EXPECT_EQ(workspace.normalizationTargetControl.getOptions().mode, AudioNormalizationMode::loudness);

    driver.click(control_ids::NORMALIZE_PEAK);
    driver.click(control_ids::PROCESS_NORMALIZE_FIRST);
    EXPECT_FALSE(driver.find(control_ids::PROCESS_NORMALIZE_FIRST).checked);
    EXPECT_FALSE(workspace.normalizeBeforePluginToggle.getToggleState());
    EXPECT_EQ(workspace.normalizationTargetControl.getOptions().mode, AudioNormalizationMode::peak);
}

/// Requires Normalize and Process to reject input without a selection,
/// then Normalize to accept it once a file is selected while Process stays disabled without plugins,
/// matching the Rust `integration_disabled_controls_reject_input` test.
/// Play is not part of this shared contract yet: this application enables it for a loaded preview
/// without an audio output, while the Rust application disables it until a player exists.
void AudioBatchIntegrationTest::disabledControlsRejectInput()
{
    const auto file = copyFixture("sine_stereo_44100_16.wav");
    AudioBatchComponent view(configuration());
    tests::GuiDriver driver(view);
    view.openPaths({file.getFullPathName()}, false);
    ASSERT_TRUE(waitForRows(view, 1));
    view.resultsTable.deselectAllRows();
    tests::GuiDriver::settle();
    for (const auto* controlId : {control_ids::NORMALIZE_SELECTED, control_ids::PROCESS_SELECTED}) {
        EXPECT_FALSE(driver.find(controlId).enabled) << controlId << " without a selection";
    }
    EXPECT_THROW(driver.click(control_ids::NORMALIZE_SELECTED), std::runtime_error);
    EXPECT_FALSE(view.isBackgroundWorkRunning());
    ASSERT_TRUE(clickRow(driver, view, file));
    EXPECT_TRUE(driver.find(control_ids::NORMALIZE_SELECTED).enabled);
    EXPECT_FALSE(driver.find(control_ids::PROCESS_SELECTED).enabled) << "Process without plugins";
}

/// Dispatches one workflow, with no installed plugins or audio hardware.
void AudioBatchIntegrationTest::TestBody()
{
    switch (workflow) {
        case IntegrationWorkflow::storage:
            storageIsolation();
            break;
        case IntegrationWorkflow::openAndSort:
            openAndSort();
            break;
        case IntegrationWorkflow::normalize:
            normalizeSelection();
            break;
        case IntegrationWorkflow::previewModes:
            previewModes();
            break;
        case IntegrationWorkflow::previewWidths:
            previewWidths();
            break;
        case IntegrationWorkflow::cancelAndClear:
            cancelAndClear();
            break;
        case IntegrationWorkflow::restore:
            restorePreferencesAndGain();
            break;
        case IntegrationWorkflow::menuShortcuts:
            menuShortcuts();
            break;
        case IntegrationWorkflow::manifestControls:
            manifestControls();
            break;
        case IntegrationWorkflow::dropFiles:
            dropFilesAndFolders();
            break;
        case IntegrationWorkflow::normalizeOptions:
            normalizeOptionsFollowControls();
            break;
        case IntegrationWorkflow::disabledControls:
            disabledControlsRejectInput();
            break;
    }
}

}  // namespace audiobatch

/// Initializes JUCE's real message thread but never creates or shows a desktop window.
int main(int argumentCount, char* arguments[])
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;
    audiobatch::CustomLookAndFeel lookAndFeel(true);
    juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
    constexpr std::array cases {
        std::pair {"StorageIsolation", audiobatch::IntegrationWorkflow::storage},
        std::pair {"OpenAndSortPreserveSelection", audiobatch::IntegrationWorkflow::openAndSort},
        std::pair {"NormalizeTracksOutputIdentity", audiobatch::IntegrationWorkflow::normalize},
        std::pair {"PreviewModesPreserveState", audiobatch::IntegrationWorkflow::previewModes},
        std::pair {"PreviewWidthsResizeIndependentlyAndPersist", audiobatch::IntegrationWorkflow::previewWidths},
        std::pair {"CancelAndClearRejectStaleCallbacks", audiobatch::IntegrationWorkflow::cancelAndClear},
        std::pair {"RecreateRestoresGainAndPreference", audiobatch::IntegrationWorkflow::restore},
        std::pair {"MenuShortcutsInvokeSharedCommands", audiobatch::IntegrationWorkflow::menuShortcuts},
        std::pair {"ManifestControlsExistWithRoles", audiobatch::IntegrationWorkflow::manifestControls},
        std::pair {"DropFilesAndFolders", audiobatch::IntegrationWorkflow::dropFiles},
        std::pair {"NormalizeOptionsFollowControls", audiobatch::IntegrationWorkflow::normalizeOptions},
        std::pair {"DisabledControlsRejectInput", audiobatch::IntegrationWorkflow::disabledControls},
    };
    for (const auto& [name, workflow] : cases) {
        testing::RegisterTest("AudioBatchIntegration", name, nullptr, nullptr, __FILE__, __LINE__, [workflow] {
            return new audiobatch::AudioBatchIntegrationTest(workflow);
        });
    }
    testing::InitGoogleTest(&argumentCount, arguments);
    const auto result = RUN_ALL_TESTS();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    return result;
}
