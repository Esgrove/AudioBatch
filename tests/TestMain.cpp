/// Entry point for the AudioBatch test executable.
/// Runs every test suite through juce::UnitTestRunner and returns a non-zero exit code when any test fails,
/// which is what CTest checks.

#include "StringFormat.h"
#include "TestSuites.h"

#include <cstdio>
#include <memory>
#include <vector>

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

/// Runs all test suites and returns 1 when any test failed, otherwise 0.
int main()
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    ConsoleLogger consoleLogger;
    juce::Logger::setCurrentLogger(&consoleLogger);

    std::vector<std::unique_ptr<juce::UnitTest>> suites;
    suites.push_back(audiobatch::tests::createAnalysisTests());
    suites.push_back(audiobatch::tests::createCacheTests());
    suites.push_back(audiobatch::tests::createCliTests());
    suites.push_back(audiobatch::tests::createFileActivityTests());
    suites.push_back(audiobatch::tests::createFormatTests());
    suites.push_back(audiobatch::tests::createMetadataTests());
    suites.push_back(audiobatch::tests::createNormalizationTests());
    suites.push_back(audiobatch::tests::createProcessingTests());
    suites.push_back(audiobatch::tests::createSortingTests());
    suites.push_back(audiobatch::tests::createSeratoOverviewTests());
    suites.push_back(audiobatch::tests::createSeratoTagReaderTests());
    suites.push_back(audiobatch::tests::createSeratoWaveformColouringTests());
    suites.push_back(audiobatch::tests::createSeratoPreviewLoaderTests());
    suites.push_back(audiobatch::tests::createUtilsTests());

    juce::Array<juce::UnitTest*> tests;
    for (const auto& suite : suites) {
        tests.add(suite.get());
    }

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runTests(tests);

    int failures = 0;
    for (int index = 0; index < runner.getNumResults(); ++index) {
        failures += runner.getResult(index)->failures;
    }

    juce::Logger::setCurrentLogger(nullptr);
    return failures > 0 ? 1 : 0;
}
