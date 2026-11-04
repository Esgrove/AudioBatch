/// Entry point for the AudioBatch test executable.
/// Registers the JUCE suites with GoogleTest for automatic CTest discovery and filtering.
/// Each discovered test runs its suite through juce::UnitTestRunner and reports any failures to GoogleTest.
/// The tests also start the executable again in a helper mode, which runs one helper instead of the suites.

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/SpectrogramData.h"
#include "PluginBenchmark.h"
#include "SpectrogramBenchmark.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"
#include <fmt/base.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Logger that writes to standard output.
/// On Windows the default logger only writes to the debugger, so CTest would not show any test output.
class ConsoleLogger final : public juce::Logger
{
protected:
    /// Prints the message on its own line and flushes, so the output survives a crash.
    void logMessage(const juce::String& message) override
    {
        fmt::print("{}\n", message);
        std::fflush(stdout);
    }
};

/// Adapts a registered JUCE suite to GoogleTest without changing its assertions or fixtures.
class JuceSuiteTest : public testing::Test
{
public:
    /// Refers to a suite owned by main for the entire test run.
    explicit JuceSuiteTest(juce::UnitTest& testSuite) : suite(testSuite) {}

    ~JuceSuiteTest() override = default;

    JuceSuiteTest(const JuceSuiteTest&) = delete;
    JuceSuiteTest(JuceSuiteTest&&) = delete;
    JuceSuiteTest& operator=(const JuceSuiteTest&) = delete;
    JuceSuiteTest& operator=(JuceSuiteTest&&) = delete;

private:
    /// Runs the suite and forwards its total failure count to GoogleTest.
    void TestBody() override
    {
        juce::Array<juce::UnitTest*> tests;
        tests.add(&suite);
        juce::UnitTestRunner runner;
        runner.setAssertOnFailure(false);
        runner.runTests(tests);

        int failures = 0;
        for (int index = 0; index < runner.getNumResults(); ++index) {
            failures += runner.getResult(index)->failures;
        }

        EXPECT_EQ(failures, 0);
    }

    juce::UnitTest& suite;
};

/// Measures repeated cache-only reads for an explicit directory of interoperability fixtures.
int benchmarkSharedCacheReads(AnalysisCache& cache, const juce::File& source)
{
    const auto files = source.findChildFiles(juce::File::findFiles, false, "*.wav");
    if (files.isEmpty()) {
        return 2;
    }
    constexpr int passes {20};
    int hits = 0;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    for (int pass = 0; pass < passes; ++pass) {
        for (const auto& file : files) {
            const auto record = cache.getAnalysis(file);
            hits += record && !record->hasError() ? 1 : 0;
        }
    }
    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - started;
    fmt::print("C++ warm lookup: {} hits, {:.3f} ms per {} rows\n", hits, elapsed / passes, files.size());
    return hits == files.size() * passes ? 0 : 1;
}

/// Stores either production FFT output or an exact byte pattern through the real spectral cache writer.
int writeSharedSpectrum(AnalysisCache& cache, const juce::File& source, const bool pattern)
{
    const utils::FileIdentity identity(source);
    const auto record = analyzeFile(source);
    if (record.hasError() || !record.spectrogram || !cache.storeAnalysis(record)) {
        return 1;
    }
    auto data = *record.spectrogram;
    if (pattern) {
        for (std::size_t index = 0; index < data.intensities.size(); ++index) {
            data.intensities[index] = static_cast<std::uint8_t>(index % 256);
        }
    }
    if (!cache.storeSpectrogramData(identity, data)) {
        return 1;
    }
    const auto loaded = cache.getSpectrogramData(source);
    return loaded && serializeSpectrogram(*loaded) == serializeSpectrogram(data) ? 0 : 1;
}

/// Reads without analysis and exports the reserialized artifact as hexadecimal for exact codec comparison.
/// Pattern mode additionally checks every payload byte independently of the Rust serializer.
int readSharedSpectrum(AnalysisCache& cache, const juce::File& source, const bool pattern)
{
    const auto record = cache.getAnalysis(source);
    const auto data = cache.getSpectrogramData(source);
    if (!record || !record->fromCache || record->hasError() || !data || !spectrogramMatches(*data, *record)) {
        return 1;
    }
    if (pattern) {
        for (std::size_t index = 0; index < data->intensities.size(); ++index) {
            if (data->intensities[index] != static_cast<std::uint8_t>(index % 256)) {
                return 1;
            }
        }
    }
    const auto bytes = serializeSpectrogram(*data);
    for (const auto byte : std::span(static_cast<const std::byte*>(bytes.getData()), bytes.getSize())) {
        fmt::print("{:02x}", std::to_integer<unsigned int>(byte));
    }
    fmt::print("\n");
    return 0;
}

/// Runs interoperability helpers against an explicit temporary database, never the user's cache.
/// Other argument shapes fall through to the test executable's remaining helper modes.
std::optional<int> runSharedCacheHelper(const juce::StringArray& arguments)
{
    if (arguments.size() != 4 || arguments[0] != "--shared-cache") {
        return std::nullopt;
    }
    const auto& mode = arguments[1];
    const juce::File database(arguments[2]);
    const juce::File source(arguments[3]);
    AnalysisCache cache(database);
    if (!cache.open()) {
        return 1;
    }
    if (mode == "benchmark-read") {
        return benchmarkSharedCacheReads(cache, source);
    }
    if (mode == "write-spectrum" || mode == "write-spectrum-pattern") {
        return writeSharedSpectrum(cache, source, mode == "write-spectrum-pattern");
    }
    if (mode == "read-spectrum" || mode == "read-spectrum-pattern") {
        return readSharedSpectrum(cache, source, mode == "read-spectrum-pattern");
    }
    if (mode == "write") {
        const auto record = analyzeFile(source);
        return !record.hasError() && cache.storeAnalysis(record) ? 0 : 1;
    }
    if (mode == "read-rust") {
        const auto record = cache.getAnalysis(source);
        return record && record->fromCache && !record->hasError()
                && record->measurementProducer == MeasurementProducer::rust
            ? 0
            : 1;
    }
    return 2;
}
}  // namespace
}  // namespace audiobatch::tests

/// Registers all compiled suites and delegates discovery, selection and results to GoogleTest.
/// A command line that selects a helper mode runs only that helper, for tests that start the executable again.
int main(int argumentCount, char* arguments[])
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::StringArray helperArguments;
    for (const auto* argument : std::span(arguments, static_cast<std::size_t>(argumentCount)).subspan(1)) {
        helperArguments.add(juce::String::fromUTF8(argument));
    }
    if (const auto helperExitCode = audiobatch::tests::runSharedCacheHelper(helperArguments)) {
        return *helperExitCode;
    }
    if (helperArguments.contains("--benchmark-spectrogram")) {
        return audiobatch::tests::runSpectrogramBenchmark(helperArguments);
    }
    if (helperArguments.contains("--vst3-benchmark")) {
        return audiobatch::tests::runPluginBenchmark(helperArguments);
    }
    if (const auto helperExitCode = audiobatch::tests::runHelperProcess(helperArguments)) {
        return *helperExitCode;
    }

    audiobatch::tests::ConsoleLogger consoleLogger;
    juce::Logger::setCurrentLogger(&consoleLogger);

    std::vector<std::unique_ptr<juce::UnitTest>> suites;
    suites.push_back(audiobatch::tests::createAnalysisTests());
    suites.push_back(audiobatch::tests::createMeasurementTests());
    suites.push_back(audiobatch::tests::createAnalysisCoordinatorTests());
    suites.push_back(audiobatch::tests::createAnalysisPreviewTests());
    suites.push_back(audiobatch::tests::createArtworkTests());
    suites.push_back(audiobatch::tests::createBackgroundWorkTests());
    suites.push_back(audiobatch::tests::createCacheTests());
    suites.push_back(audiobatch::tests::createSeratoTagCacheTests());
    suites.push_back(audiobatch::tests::createCliTests());
    suites.push_back(audiobatch::tests::createFileActivityTests());
    suites.push_back(audiobatch::tests::createFormatTests());
    suites.push_back(audiobatch::tests::createGuiFontTests());
    suites.push_back(audiobatch::tests::createMetadataTests());
    suites.push_back(audiobatch::tests::createNormalizationTests());
    suites.push_back(audiobatch::tests::createNormalizeCoordinatorTests());
    suites.push_back(audiobatch::tests::createOverloadHistoryTests());
    suites.push_back(audiobatch::tests::createPluginCoordinatorTests());
    suites.push_back(audiobatch::tests::createPluginPresetTests());
    suites.push_back(audiobatch::tests::createPluginScanTests());
    suites.push_back(audiobatch::tests::createProcessingTests());
    suites.push_back(audiobatch::tests::createPreviewControllerTests());
    suites.push_back(audiobatch::tests::createPreviewEnrichmentTests());
    suites.push_back(audiobatch::tests::createPreviewPlaybackTests());
    suites.push_back(audiobatch::tests::createPreviewPlayerTests());
    suites.push_back(audiobatch::tests::createPlaybackStatusTests());
    suites.push_back(audiobatch::tests::createPreviewSeekTests());
    suites.push_back(audiobatch::tests::createResultsModelTests());
    suites.push_back(audiobatch::tests::createSortingTests());
    suites.push_back(audiobatch::tests::createSpectrogramTests());
    suites.push_back(audiobatch::tests::createSpectrogramFixtureTests());
    suites.push_back(audiobatch::tests::createSeratoOverviewTests());
    suites.push_back(audiobatch::tests::createSeratoTagReaderTests());
    suites.push_back(audiobatch::tests::createRealMetadataTests());
    suites.push_back(audiobatch::tests::createSeratoTagsTests());
    suites.push_back(audiobatch::tests::createSeratoPreviewLoaderTests());
    suites.push_back(audiobatch::tests::createSeratoPreviewTests());
    suites.push_back(audiobatch::tests::createUtilsTests());

    for (const auto& suite : suites) {
        const auto name = suite->getName().replaceCharacter(' ', '_');
        testing::RegisterTest(
            "AudioBatch", name.toRawUTF8(), nullptr, nullptr, __FILE__, __LINE__, [testSuite = suite.get()] {
                return new audiobatch::tests::JuceSuiteTest(*testSuite);
            }
        );
    }

    testing::InitGoogleTest(&argumentCount, arguments);
    const auto exitCode = RUN_ALL_TESTS();
    juce::Logger::setCurrentLogger(nullptr);
    return exitCode;
}
