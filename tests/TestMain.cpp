/// Entry point for the AudioBatch test executable.
/// Runs every test suite through juce::UnitTestRunner and returns a non-zero exit code when any test fails,
/// which is what CTest checks.

#include "TestSuites.h"

#include <memory>
#include <vector>

/// Runs all test suites and returns 1 when any test failed, otherwise 0.
int main()
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    std::vector<std::unique_ptr<juce::UnitTest>> suites;
    suites.push_back(audiobatch::tests::createAnalysisTests());
    suites.push_back(audiobatch::tests::createCacheTests());
    suites.push_back(audiobatch::tests::createCliTests());
    suites.push_back(audiobatch::tests::createFormatTests());
    suites.push_back(audiobatch::tests::createMetadataTests());
    suites.push_back(audiobatch::tests::createNormalizationTests());
    suites.push_back(audiobatch::tests::createProcessingTests());
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

    return failures > 0 ? 1 : 0;
}
