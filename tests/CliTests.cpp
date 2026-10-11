/// Tests for the CLI argument parsing in AudioAnalysisCli::parse.
/// Covers option values, aliases, and the combinations the CLI rejects.

#include "AudioAnalysisCli.h"
#include "TestSuites.h"

#include <optional>

namespace audiobatch::tests
{
/// Parses the given arguments as if they followed the executable name on the command line.
static std::optional<AudioAnalysisCliOptions> parseArguments(
    const juce::StringArray& arguments,
    juce::String& errorMessage
)
{
    return AudioAnalysisCli::parse(juce::ArgumentList("audiobatch", arguments), errorMessage);
}

/// Checks CLI parsing results for valid and invalid argument lists.
class CliTests final : public juce::UnitTest
{
public:
    CliTests() : juce::UnitTest("CLI argument parsing", "AudioBatch") { }

    /// Parses arguments that are expected to be valid, recording a failure with the parse error when they are not.
    /// Returns default options on failure so the caller can keep checking fields.
    AudioAnalysisCliOptions expectParsed(const juce::StringArray& arguments)
    {
        juce::String errorMessage;
        const auto options = parseArguments(arguments, errorMessage);
        expect(options.has_value(), errorMessage);
        return options.value_or(AudioAnalysisCliOptions {});
    }

    /// Runs the CLI parsing checks.
    void runTest() override
    {
        juce::String errorMessage;

        beginTest("Separate short options are parsed");
        {
            const auto options = expectParsed({"-r", "-f", "music"});
            expect(options.recursive);
            expect(options.refresh);
            expectEquals(options.inputPaths.size(), 1);
        }

        beginTest("Combined short options are rejected");
        {
            expect(!parseArguments({"-rf"}, errorMessage).has_value());
            expect(errorMessage.contains("-rf"), errorMessage);
        }

        beginTest("Option values are accepted after an equals sign or as the next argument");
        {
            auto options = expectParsed({"--jobs=4"});
            expectEquals(options.workerCount, 4);

            options = expectParsed({"-j", "3"});
            expectEquals(options.workerCount, 3);

            expect(!parseArguments({"-j", "0"}, errorMessage).has_value());
        }

        beginTest("Sort modes and the loudness alias are recognised");
        {
            auto options = expectParsed({"--sort", "loudness"});
            expect(options.sortMode == AudioAnalysisSortMode::loudness);

            options = expectParsed({"-s", "name"});
            expect(options.sortMode == AudioAnalysisSortMode::name);

            expect(!parseArguments({"--sort", "bogus"}, errorMessage).has_value());
        }

        beginTest("Plugin listing options require --list-plugins and exclude analysis options");
        {
            expect(!parseArguments({"-g", "type"}, errorMessage).has_value());
            expect(!parseArguments({"-R"}, errorMessage).has_value());
            expect(!parseArguments({"-l", "-r"}, errorMessage).has_value());
            expect(!parseArguments({"-l", "music"}, errorMessage).has_value());

            const auto options = expectParsed({"-l", "-g", "manufacturer"});
            expect(options.pluginGrouping == PluginListGrouping::manufacturer);
        }

        beginTest("Force requires normalize");
        {
            expect(!parseArguments({"--force"}, errorMessage).has_value());

            const auto options = expectParsed({"-n", "-F"});
            expect(options.normalize);
            expect(options.force);
        }

        beginTest("Output formats and reverse sorting are parsed");
        {
            auto options = expectParsed({"--output", "json", "--reverse"});
            expect(options.outputFormat == AudioAnalysisOutputFormat::json);
            expect(options.reverse);

            options = expectParsed({"--output=csv"});
            expect(options.outputFormat == AudioAnalysisOutputFormat::csv);

            expect(!parseArguments({"-o", "xml"}, errorMessage).has_value());
            expect(!parseArguments({"-l", "-o", "json"}, errorMessage).has_value());
        }

        beginTest("Normalization targets are parsed and validated");
        {
            auto options = expectParsed({"-n", "--lufs", "-14"});
            expect(options.hasNormalizationTarget);
            expect(options.normalization.mode == AudioNormalizationMode::loudness);
            expectEquals(options.normalization.targetLufs, -14.0);

            options = expectParsed({"-n", "--lufs=-16.5"});
            expectEquals(options.normalization.targetLufs, -16.5);

            options = expectParsed({"-n", "--peak", "-1"});
            expect(options.normalization.mode == AudioNormalizationMode::peak);
            expectEquals(options.normalization.targetPeakDbfs, -1.0);

            options = expectParsed({"-n"});
            expect(options.normalization.mode == AudioNormalizationMode::peak);
            expectEquals(options.normalization.targetPeakDbfs, 0.0);

            expect(!parseArguments({"-n", "--lufs", "-30"}, errorMessage).has_value());
            expect(!parseArguments({"-n", "--peak", "1"}, errorMessage).has_value());
            expect(!parseArguments({"-n", "--lufs", "loud"}, errorMessage).has_value());
            expect(!parseArguments({"-n", "--peak", "-1", "--lufs", "-14"}, errorMessage).has_value());
            expect(!parseArguments({"--lufs", "-14"}, errorMessage).has_value());
        }

        beginTest("Unknown options are rejected");
        {
            expect(!parseArguments({"--bogus"}, errorMessage).has_value());
            expect(errorMessage.contains("--bogus"), errorMessage);
        }
    }
};

std::unique_ptr<juce::UnitTest> createCliTests()
{
    return std::make_unique<CliTests>();
}
}  // namespace audiobatch::tests
