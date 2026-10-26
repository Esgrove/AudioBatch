/// Entry point for the AudioBatch test executable.
/// Registers the JUCE suites with GoogleTest for automatic CTest discovery and filtering.
/// Each discovered test runs its suite through juce::UnitTestRunner and reports any failures to GoogleTest.

#include "TestSuites.h"
#include "utils/StringFormat.h"

#include <gtest/gtest.h>

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

namespace audiobatch::tests
{
/// Adapts a registered JUCE suite to GoogleTest without changing its assertions or fixtures.
class JuceSuiteTest : public testing::Test
{
public:
    /// Refers to a suite owned by main for the entire test run.
    explicit JuceSuiteTest(juce::UnitTest& testSuite) : suite(testSuite) {}

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
}  // namespace audiobatch::tests

/// Registers all compiled suites and delegates discovery, selection and results to GoogleTest.
int main(int argumentCount, char* arguments[])
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    ConsoleLogger consoleLogger;
    juce::Logger::setCurrentLogger(&consoleLogger);

    std::vector<std::unique_ptr<juce::UnitTest>> suites;
    suites.push_back(audiobatch::tests::createAnalysisTests());
    suites.push_back(audiobatch::tests::createBackgroundWorkTests());
    suites.push_back(audiobatch::tests::createCacheTests());
    suites.push_back(audiobatch::tests::createCliTests());
    suites.push_back(audiobatch::tests::createFileActivityTests());
    suites.push_back(audiobatch::tests::createFormatTests());
    suites.push_back(audiobatch::tests::createGuiFontTests());
    suites.push_back(audiobatch::tests::createMetadataTests());
    suites.push_back(audiobatch::tests::createNormalizationTests());
    suites.push_back(audiobatch::tests::createProcessingTests());
    suites.push_back(audiobatch::tests::createPreviewControllerTests());
    suites.push_back(audiobatch::tests::createPreviewPlaybackTests());
    suites.push_back(audiobatch::tests::createResultsModelTests());
    suites.push_back(audiobatch::tests::createSortingTests());
    suites.push_back(audiobatch::tests::createSeratoOverviewTests());
    suites.push_back(audiobatch::tests::createSeratoTagReaderTests());
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
