/// Tests for PreviewController:
/// the transitions between an empty, released, and loaded preview, and which file each state keeps.

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/PreviewController.h"
#include "serato/SeratoTagTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/FileUtils.h"

#include <cstdint>
#include <memory>
#include <variant>

namespace audiobatch::tests
{
/// Checks how loading, releasing, and clearing move the preview between its states.
class PreviewControllerTests final : public juce::UnitTest
{
public:
    PreviewControllerTests() : juce::UnitTest("Preview controller", "AudioBatch") {}

    /// Runs the preview state checks.
    void runTest() override
    {
        // The cache is never opened, because these checks request no Serato tags.
        AnalysisCache cache(
            juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("AudioBatch preview controller tests.db")
        );
        TestAudioDeviceState device;
        AudioPreviewPlayer player;
        installTestAudioDevice(player.getDeviceManager(), device);
        PreviewController preview(
            cache,
            player,
            [](const utils::FileIdentity&, std::uint64_t, const std::shared_ptr<const serato::SeratoData>&) {}
        );
        const auto file = fixtureFile("silence_stereo_44100_16.wav");

        beginTest("A new preview is empty");
        {
            expect(std::holds_alternative<NoPreview>(preview.getState()));
            expect(preview.getFile() == juce::File());
            expect(!preview.isWaveformLoadedFromCache());
        }

        beginTest("Loading a file keeps its identity and a fresh waveform flag");
        {
            expect(preview.load(file, false));
            const auto* loaded = std::get_if<LoadedPreview>(&preview.getState());
            expect(loaded != nullptr);
            expect(loaded != nullptr && loaded->identity.matches(file));
            expect(preview.getFile() == file);
            expect(player.hasLoadedSource());
            expect(!preview.isWaveformLoadedFromCache());
            preview.setWaveformLoadedFromCache(true);
            expect(preview.isWaveformLoadedFromCache());
        }

        beginTest("Releasing keeps the file selected but closes the player");
        {
            preview.release();
            expect(std::holds_alternative<ReleasedPreview>(preview.getState()));
            expect(preview.getFile() == file);
            expect(!player.hasLoadedSource());
            expect(!preview.isWaveformLoadedFromCache());
            expect(preview.getSeratoData() == nullptr);
            expect(!preview.adoptSeratoData(utils::FileIdentity(file), 1, nullptr));
        }

        beginTest("A file that fails to load stays selected as a released preview");
        {
            const auto missing = file.getSiblingFile("missing preview file.wav");
            expect(!preview.load(missing, false));
            expect(std::holds_alternative<ReleasedPreview>(preview.getState()));
            expect(preview.getFile() == missing);
            expect(!player.hasLoadedSource());
        }

        beginTest("Clearing forgets the file");
        {
            expect(preview.load(file, false));
            preview.clear();
            expect(std::holds_alternative<NoPreview>(preview.getState()));
            expect(preview.getFile() == juce::File());
            expect(!player.hasLoadedSource());
        }

        beginTest("Source histories require ready analysis and matching selected, loaded and on-disk identities");
        {
            const TemporaryDirectory directory;
            const auto selected = directory.copyFixture("silence_stereo_44100_16.wav", "selected.wav");
            expect(preview.load(selected, false));
            const utils::FileIdentity identity(selected);
            AudioAnalysisRecord record;
            record.file = selected;
            record.fullPath = identity.path;
            record.fileSize = identity.fileSize;
            record.modifiedTimeMs = identity.modifiedTimeMs;
            record.status = AudioAnalysisStatus::analyzed;
            expect(preview.canDisplayAnalysis(record));
            record.status = AudioAnalysisStatus::cached;
            expect(preview.canDisplayAnalysis(record));
            record.customGain = Decibels {-12.0f};
            expect(preview.canDisplayAnalysis(record));

            record.status = AudioAnalysisStatus::pending;
            expect(!preview.canDisplayAnalysis(record));
            record.status = AudioAnalysisStatus::failed;
            expect(!preview.canDisplayAnalysis(record));
            record.status = AudioAnalysisStatus::analyzed;
            ++record.fileSize;
            expect(!preview.canDisplayAnalysis(record));
            record.fileSize = identity.fileSize;
            ++record.modifiedTimeMs;
            expect(!preview.canDisplayAnalysis(record));
            record.modifiedTimeMs = identity.modifiedTimeMs;
            record.file = file;
            expect(!preview.canDisplayAnalysis(record));
            record.file = selected;
            record.fullPath = file.getFullPathName();
            expect(!preview.canDisplayAnalysis(record));
            record.fullPath = identity.path;

            expect(selected.setLastModificationTime(selected.getLastModificationTime() + juce::RelativeTime::hours(1)));
            expect(!preview.canDisplayAnalysis(record));
            record.modifiedTimeMs = selected.getLastModificationTime().toMilliseconds();
            expect(!preview.canDisplayAnalysis(record));
            expect(preview.load(selected, false));
            expect(preview.canDisplayAnalysis(record));
            preview.release();
            expect(!preview.canDisplayAnalysis(record));
            preview.clear();
            expect(!preview.canDisplayAnalysis(record));
        }

        preview.stop();
    }
};

std::unique_ptr<juce::UnitTest> createPreviewControllerTests()
{
    return std::make_unique<PreviewControllerTests>();
}
}  // namespace audiobatch::tests
