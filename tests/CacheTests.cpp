/// Tests for AnalysisCache against a temporary SQLite database.
/// Covers the round trip of a stored record and invalidation when the file changes on disk.
/// The cached files are copies of the committed fixtures.

#include "AnalysisCache.h"
#include "AudioAnalysisService.h"
#include "TestHelpers.h"
#include "TestSuites.h"

namespace audiobatch::tests
{
/// Checks that cached analysis is returned for unchanged files and dropped for changed ones.
class CacheTests final : public juce::UnitTest
{
public:
    CacheTests() : juce::UnitTest("Analysis cache", "AudioBatch") { }

    /// Runs the cache checks.
    void runTest() override
    {
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("analysis.db"));

        beginTest("The cache database opens in a custom location");
        expect(cache.open());
        expect(directory.getChildFile("analysis.db").existsAsFile());

        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "tone.wav");
        const auto analyzed = AudioAnalysisService::analyzeFile(file);
        expect(!analyzed.hasError(), analyzed.errorMessage);

        beginTest("A stored record is returned for the unchanged file");
        {
            expect(cache.storeAnalysis(analyzed));

            AudioAnalysisRecord cached;
            expect(cache.getAnalysis(file, cached));
            expectEquals(cached.overallPeak, analyzed.overallPeak);
            expectEquals(cached.sampleRate, analyzed.sampleRate);
        }

        beginTest("A changed file is not served from the cache");
        {
            expect(fixtureFile("sine_stereo_48000_24.wav").copyFileTo(file));

            AudioAnalysisRecord cached;
            expect(!cache.getAnalysis(file, cached));
        }

        beginTest("A removed record is no longer returned");
        {
            const auto refreshed = AudioAnalysisService::analyzeFile(file);
            expect(cache.storeAnalysis(refreshed));
            expect(cache.removeAnalysis(file));

            AudioAnalysisRecord cached;
            expect(!cache.getAnalysis(file, cached));
        }
    }
};

std::unique_ptr<juce::UnitTest> createCacheTests()
{
    return std::make_unique<CacheTests>();
}
}  // namespace audiobatch::tests
