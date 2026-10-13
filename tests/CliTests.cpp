/// Tests for the CLI argument parsing in cli::parseArguments and the CSV and JSON output.
/// Covers option values, aliases, the combinations the CLI rejects, and the machine-readable record fields.

#include "AudioAnalysisCli.h"
#include "AudioAnalysisCliOutput.h"
#include "TestSuites.h"

#include <optional>
#include <vector>

namespace audiobatch::tests
{
/// Parses the given arguments as if they followed the executable name on the command line.
static std::optional<AudioAnalysisCliOptions> parseArguments(
    const juce::StringArray& arguments,
    juce::String& errorMessage
)
{
    return cli::parseArguments(juce::ArgumentList("audiobatch", arguments), errorMessage);
}

/// Checks CLI parsing results for valid and invalid argument lists.
class CliTests final : public juce::UnitTest
{
public:
    CliTests() : juce::UnitTest("CLI argument parsing", "AudioBatch") {}

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

        beginTest("Negative numbers never match short options");
        {
            expect(!parseArguments({"-j", "-1"}, errorMessage).has_value());
            expect(errorMessage.contains("greater than zero"), errorMessage);

            expect(!parseArguments({"-12", "song.wav"}, errorMessage).has_value());
            expect(errorMessage.contains("Unexpected number -12"), errorMessage);

            const juce::ArgumentList negativeValue("audiobatch", juce::StringArray {"-j", "-1"});
            expect(!cli::containsExactOption(negativeValue, {"--help", "-h"}));
            const juce::ArgumentList help("audiobatch", juce::StringArray {"-h"});
            expect(cli::containsExactOption(help, {"--help", "-h"}));
        }

        beginTest("Boolean options never absorb following negative numbers");
        {
            for (const auto& option : juce::StringArray {"--refresh", "--normalize", "--reverse"}) {
                expect(!parseArguments({option, "-14", "song.wav"}, errorMessage).has_value());
                expect(errorMessage.contains("Unexpected number -14"), errorMessage);
            }
        }

        beginTest("Option values are accepted after an equals sign or as the next argument");
        {
            auto options = expectParsed({"--jobs=4"});
            expectEquals(options.workerCount, 4);

            options = expectParsed({"-j", "3"});
            expectEquals(options.workerCount, 3);

            expect(!parseArguments({"-j", "0"}, errorMessage).has_value());
            expect(!parseArguments({"-j", "4x"}, errorMessage).has_value());
            expect(!parseArguments({"-j", "2.5"}, errorMessage).has_value());
            expect(!parseArguments({"-j", "100000"}, errorMessage).has_value());
            expect(errorMessage.contains("at most"), errorMessage);
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

        runOutputTests();
    }

private:
    /// Checks the CSV and JSON output of a quoted path, a silent file, and a failed file.
    void runOutputTests()
    {
        AudioAnalysisRecord quoted;
        quoted.fullPath = R"(/music/a, "b".wav)";
        quoted.fileName = R"(a, "b".wav)";
        quoted.overallPeak = 0.5f;
        quoted.overallTruePeak = 0.5;
        quoted.integratedLufs = -14.0;
        quoted.status = AudioAnalysisStatus::analyzed;

        AudioAnalysisRecord silent;
        silent.fullPath = "/music/silence.wav";
        silent.fileName = "silence.wav";
        silent.integratedLufs = AudioAnalysisRecord::negativeInfinityLoudness;
        silent.maxShortTermLufs = AudioAnalysisRecord::negativeInfinityLoudness;
        silent.status = AudioAnalysisStatus::analyzed;

        AudioAnalysisRecord failed;
        failed.fullPath = "/music/broken.wav";
        failed.fileName = "broken.wav";
        failed.status = AudioAnalysisStatus::failed;
        failed.errorMessage = "Unsupported or unreadable audio file";

        const std::vector records {quoted, silent, failed};

        beginTest("CSV output escapes fields and always has a header");
        {
            const auto emptyLines = juce::StringArray::fromLines(cli::formatRecordsAsCsv({}).trimEnd());
            expectEquals(emptyLines.size(), 1);
            expect(emptyLines[0].startsWith("path,name,"), emptyLines[0]);

            const auto csv = cli::formatRecordsAsCsv(records);
            expect(!csv.containsChar('\r'));
            const auto lines = juce::StringArray::fromLines(csv.trimEnd());
            expectEquals(lines.size(), 4);
            expect(lines[1].startsWith(R"("/music/a, ""b"".wav")"), lines[1]);
            expect(lines[3].contains("failed"), lines[3]);
            expect(lines[3].contains("Unsupported or unreadable audio file"), lines[3]);
        }

        beginTest("JSON output writes null for silence and keeps failed records");
        {
            const auto json = cli::formatRecordsAsJson(records);
            expect(!json.containsChar('\r'));
            const auto parsed = juce::JSON::parse(json);
            expect(parsed.isArray());
            expectEquals(parsed.size(), 3);

            expectEquals(static_cast<double>(parsed[0]["peakDbfs"]), -6.02);
            expectEquals(parsed[0]["path"].toString(), quoted.fullPath);
            expect(parsed[1]["peakDbfs"].isVoid());
            expect(parsed[1]["integratedLufs"].isVoid());
            expectEquals(parsed[2]["status"].toString(), juce::String("failed"));
            expectEquals(parsed[2]["error"].toString(), failed.errorMessage);
        }
    }
};

std::unique_ptr<juce::UnitTest> createCliTests()
{
    return std::make_unique<CliTests>();
}
}  // namespace audiobatch::tests
