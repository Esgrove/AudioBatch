/// Real main-view workflows on the JUCE message thread without a desktop peer or audio hardware.
/// Each case owns explicit temporary storage and copied fixtures, uses the view's semantic actions or controls,
/// and pumps bounded event-loop slices until observable state settles.

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponent.h"
#include "gui/AudioBatchComponentConstants.h"
#include "gui/components/CustomLookAndFeel.h"
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
    cancelAndClear,
    restore,
};

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
    IntegrationDirectory& operator=(const IntegrationDirectory&) = delete;
    IntegrationDirectory(IntegrationDirectory&&) = delete;
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

/// Finds a real normalization control through its semantic button text.
juce::Button* findButton(const juce::Component& component, const juce::String& text)
{
    std::vector<juce::Component*> pending(component.getChildren().begin(), component.getChildren().end());
    while (!pending.empty()) {
        auto* child = pending.back();
        pending.pop_back();
        if (auto* button = dynamic_cast<juce::Button*>(child); button != nullptr && button->getButtonText() == text) {
            return button;
        }
        pending.insert(pending.end(), child->getChildren().begin(), child->getChildren().end());
    }
    return nullptr;
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
    AudioBatchIntegrationTest& operator=(const AudioBatchIntegrationTest&) = delete;
    AudioBatchIntegrationTest(AudioBatchIntegrationTest&&) = delete;
    AudioBatchIntegrationTest& operator=(AudioBatchIntegrationTest&&) = delete;

private:
    /// Dispatches a workflow with no native window or installed plugins.
    void TestBody() override
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
            case IntegrationWorkflow::cancelAndClear:
                cancelAndClear();
                break;
            case IntegrationWorkflow::restore:
                restorePreferencesAndGain();
                break;
        }
    }

    /// Builds per-view configuration with no process-global environment mutation.
    [[nodiscard]] AudioBatchComponentConfig configuration() const
    {
        AudioBatchComponentConfig config;
        config.cacheDatabaseFile = temporary.get().getChildFile("AudioBatch/analysis.db");
        config.settingsOptions = utils::createApplicationSettingsOptions(temporary.get());
        return config;
    }

    /// Copies a small committed fixture before any view can modify it.
    [[nodiscard]] juce::File copyFixture(const juce::String& name) const
    {
        const auto source = juce::File(AUDIOBATCH_TEST_FIXTURES_DIR).getChildFile(name);
        const auto target = temporary.get().getChildFile(name);
        if (!source.copyFileTo(target)) {
            throw std::runtime_error(utils::format("Could not copy fixture {}", name).toStdString());
        }
        return target;
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

    /// Selects by stable source identity through the actual table callback.
    static bool selectFile(AudioBatchComponent& view, const juce::File& file)
    {
        const auto row = view.analysisResults.findRow(file.getFullPathName());
        if (!row) {
            return false;
        }
        view.resultsTable.selectRow(*row);
        return view.getSelectedRecordPaths().contains(file.getFullPathName());
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

    /// Pins invalid override rejection and the exact isolated settings/plugin directory without touching native paths.
    void storageIsolation()
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
        EXPECT_NE(
            isolatedLock, utils::scopedPluginScanLockName(pluginScanLockName, temporary.get().getChildFile("other"))
        );
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
    void openAndSort()
    {
        const auto valid = copyFixture("serato_rainbow_stereo_44100_16.flac");
        const auto other = copyFixture("sine_mono_44100_16.wav");
        const auto broken = copyFixture("broken.wav");
        AudioBatchComponent view(configuration());
        view.openPaths({valid.getFullPathName(), broken.getFullPathName(), other.getFullPathName()}, false);
        ASSERT_TRUE(waitForRows(view, 3));
        const auto brokenRow = view.analysisResults.findRow(broken.getFullPathName());
        if (!brokenRow) {
            ADD_FAILURE() << "The broken input was lost instead of producing an error row";
            return;
        }
        EXPECT_TRUE(view.analysisResults[*brokenRow].hasError());
        ASSERT_TRUE(selectFile(view, valid));
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
        view.resultsTable.getHeader().setSortColumnId(toColumnId(AudioFileTableColumn::name), false);
        ASSERT_TRUE(dispatchUntil([&] { return view.sortState.primaryColumn == AudioFileTableColumn::name; }));
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
        EXPECT_EQ(view.getPeer(), nullptr);
    }

    /// Converts only a copied WAV through the real Normalize control and follows the committed output everywhere.
    void normalizeSelection()
    {
        const auto source = copyFixture("sine_stereo_44100_16.wav");
        const auto output = source.withFileExtension(".aif");
        AudioBatchComponent view(configuration());
        view.openPaths({source.getFullPathName()}, false);
        ASSERT_TRUE(waitForRows(view, 1));
        ASSERT_TRUE(selectFile(view, source));
        ASSERT_TRUE(waitForWaveform(view, source));
        auto* normalize = findButton(view.previewWorkspace.normalizationTargetControl, "Normalize");
        ASSERT_NE(normalize, nullptr);
        ASSERT_TRUE(normalize->isEnabled());
        normalize->onClick();
        ASSERT_TRUE(dispatchUntil([&] {
            return !view.isBackgroundWorkRunning()
                && view.analysisResults.findRow(output.getFullPathName()).has_value();
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

    /// Cancels and clears before queued delivery.
    /// Completes a new load to expose stale callbacks deterministically.
    void cancelAndClear()
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
    void restorePreferencesAndGain()
    {
        const auto file = copyFixture("sine_stereo_44100_16.wav");
        bool expectedPreference = false;
        {
            AudioBatchComponent view(configuration());
            view.openPaths({file.getFullPathName()}, false);
            ASSERT_TRUE(waitForRows(view, 1));
            ASSERT_TRUE(selectFile(view, file));
            view.previewWorkspace.gainSlider.setValue(-3.0, juce::sendNotificationSync);
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

    IntegrationDirectory temporary;
    IntegrationWorkflow workflow;
};
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
        std::pair {"CancelAndClearRejectStaleCallbacks", audiobatch::IntegrationWorkflow::cancelAndClear},
        std::pair {"RecreateRestoresGainAndPreference", audiobatch::IntegrationWorkflow::restore},
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
