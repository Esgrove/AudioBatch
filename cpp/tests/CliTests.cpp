/// Tests for the CLI argument parsing in cli::parseArguments and the CSV and JSON output.
/// Covers option values, aliases, the combinations the CLI rejects, and the machine-readable record fields,
/// the option matching and UTF-8 decoding of process arguments fixed in the JUCE fork,
/// and the errors for inputs that produced no result.

#include "cli/AudioAnalysisCli.h"
#include "cli/AudioAnalysisCliOutput.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <array>
#include <expected>
#include <variant>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Parses the given arguments as if they followed the executable name on the command line.
/// Also copies a parse error into errorMessage, so a check can match its text after a failed parse.
std::expected<CliCommand, juce::String> parseArguments(const juce::StringArray& arguments, juce::String& errorMessage)
{
    auto command = cli::parseArguments(juce::ArgumentList("audiobatch", arguments));
    errorMessage = command ? juce::String() : command.error();
    return command;
}
}  // namespace

/// Checks CLI parsing results for valid and invalid argument lists.
class CliTests final : public juce::UnitTest
{
public:
    CliTests() : juce::UnitTest("CLI argument parsing", "AudioBatch") {}

    /// Parses arguments that are expected to be a valid command of the given type,
    /// recording a failure with the parse error, or for another command type, when they are not.
    /// Returns a default command on failure so the caller can keep checking fields.
    template<typename Command>
    Command expectCommand(const juce::StringArray& arguments)
    {
        juce::String errorMessage;
        const auto command = parseArguments(arguments, errorMessage);
        expect(command.has_value(), errorMessage);

        const auto* parsed = command.has_value() ? std::get_if<Command>(&*command) : nullptr;
        expect(!command.has_value() || parsed != nullptr, "The arguments parsed into another command");
        return parsed != nullptr ? *parsed : Command {};
    }

    /// Runs the CLI parsing checks.
    void runTest() override
    {
        juce::String errorMessage;

        beginTest("Separate short options are parsed");
        {
            const auto command = expectCommand<AnalyzeCommand>({"-r", "-f", "music"});
            expect(command.recursive);
            expect(command.refresh);
            expect(!command.normalization.has_value());
            expectEquals(command.inputPaths.size(), 1);
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
            expect(!negativeValue.containsOption("--help|-h"));
            const juce::ArgumentList help("audiobatch", juce::StringArray {"-h"});
            expect(help.containsOption("--help|-h"));
        }

        beginTest("Argument matching compares option letters and takes negative numbers as values");
        {
            // The JUCE fork compares the option letter itself.
            // Before that fix the letter became its number string, so "-a1" matched "-h" through the "1" of "104".
            const juce::ArgumentList digits("audiobatch", juce::StringArray {"-a1"});
            expect(!digits.containsOption("-h"));
            const juce::ArgumentList letters("audiobatch", juce::StringArray {"-ah"});
            expect(letters.containsOption("-h"));

            const juce::ArgumentList negative("audiobatch", juce::StringArray {"--lufs", "-14", "-j", "-.5"});
            expect(!negative[1].isOption());
            expect(!negative[3].isShortOption());
            expectEquals(negative.getValueForOption("--lufs"), juce::String("-14"));
            expectEquals(negative.getValueForOption("-j"), juce::String("-.5"));

            juce::ArgumentList longForm("audiobatch", juce::StringArray {"--sort", "name", "song.wav"});
            expectEquals(longForm.removeValueForOption("--sort|-s"), juce::String("name"));
            expectEquals(longForm.size(), 1);
            expectEquals(longForm[0].text, juce::String("song.wav"));
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
            expectEquals(expectCommand<AnalyzeCommand>({"--jobs=4"}).workerCount, 4);
            expectEquals(expectCommand<AnalyzeCommand>({"--jobs", "5"}).workerCount, 5);
            expectEquals(expectCommand<AnalyzeCommand>({"-j", "3"}).workerCount, 3);

            expect(!parseArguments({"-j", "0"}, errorMessage).has_value());
            expect(!parseArguments({"-j", "4x"}, errorMessage).has_value());
            expect(!parseArguments({"-j", "2.5"}, errorMessage).has_value());
            expect(!parseArguments({"-j", "100000"}, errorMessage).has_value());
            expect(errorMessage.contains("at most"), errorMessage);
        }

        beginTest("Sort modes and the loudness alias are recognised");
        {
            expect(expectCommand<AnalyzeCommand>({"--sort", "loudness"}).sortMode == AudioAnalysisSortMode::loudness);
            expect(expectCommand<AnalyzeCommand>({"--sort=LUFS"}).sortMode == AudioAnalysisSortMode::loudness);
            expect(expectCommand<AnalyzeCommand>({"-s", "name"}).sortMode == AudioAnalysisSortMode::name);

            expect(!parseArguments({"--sort", "bogus"}, errorMessage).has_value());
            expect(errorMessage.contains("peak, lufs, loudness, name, path"), errorMessage);
        }

        beginTest("Plugin listing options require --list-plugins and exclude analysis options");
        {
            expect(!parseArguments({"-g", "type"}, errorMessage).has_value());
            expect(!parseArguments({"-R"}, errorMessage).has_value());
            expect(!parseArguments({"-l", "-r"}, errorMessage).has_value());
            expect(!parseArguments({"-l", "music"}, errorMessage).has_value());

            expect(!parseArguments({"-l", "--bogus"}, errorMessage).has_value());
            expect(errorMessage.contains("--bogus"), errorMessage);
            expect(!parseArguments({"-l", "-g", "size"}, errorMessage).has_value());
            expect(errorMessage.contains("name, manufacturer, type, format"), errorMessage);

            auto command = expectCommand<ListPluginsCommand>({"-l", "-g", "manufacturer"});
            expect(command.grouping == PluginListGrouping::manufacturer);
            expect(!command.rescan);

            command = expectCommand<ListPluginsCommand>({"--list-plugins", "--group-by", "type", "-R"});
            expect(command.grouping == PluginListGrouping::type);
            expect(command.rescan);
        }

        beginTest("Force requires normalize");
        {
            expect(!parseArguments({"--force"}, errorMessage).has_value());

            const auto command = expectCommand<AnalyzeCommand>({"-n", "-F"});
            expect(command.normalization.has_value());
            expect(command.normalization.value_or(AudioNormalizationOptions {}).overwriteExisting);
            expect(!expectCommand<AnalyzeCommand>({"-n"})
                        .normalization.value_or(AudioNormalizationOptions {})
                        .overwriteExisting);
        }

        beginTest("Output formats and reverse sorting are parsed");
        {
            const auto command = expectCommand<AnalyzeCommand>({"--output", "json", "--reverse"});
            expect(command.outputFormat == AudioAnalysisOutputFormat::json);
            expect(command.reverse);

            expect(expectCommand<AnalyzeCommand>({"--output=csv"}).outputFormat == AudioAnalysisOutputFormat::csv);

            expect(!parseArguments({"-o", "xml"}, errorMessage).has_value());
            expect(!parseArguments({"-l", "-o", "json"}, errorMessage).has_value());
        }

        beginTest("Normalization targets are parsed and validated");
        {
            const auto normalizationOf = [this](const juce::StringArray& arguments) {
                const auto normalization = expectCommand<AnalyzeCommand>(arguments).normalization;
                expect(normalization.has_value());
                return normalization.value_or(AudioNormalizationOptions {});
            };

            auto normalization = normalizationOf({"-n", "--lufs", "-14"});
            expect(normalization.mode == AudioNormalizationMode::loudness);
            expectEquals(normalization.targetLufs, -14.0);

            normalization = normalizationOf({"-n", "--lufs=-16.5"});
            expectEquals(normalization.targetLufs, -16.5);

            normalization = normalizationOf({"-n", "--peak", "-1"});
            expect(normalization.mode == AudioNormalizationMode::peak);
            expectEquals(normalization.targetPeakDbfs, -1.0);

            normalization = normalizationOf({"-n"});
            expect(normalization.mode == AudioNormalizationMode::peak);
            expectEquals(normalization.targetPeakDbfs, 0.0);

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

#if !JUCE_WINDOWS
        // Windows passes argv in the ANSI code page, and JUCE reads the UTF-16 command line of the process instead.
        beginTest("Arguments from the process are decoded as UTF-8");
        {
            // "Björk.flac" and "-j", as the bytes a terminal passes in argv.
            std::array<char, 16> executable {"audiobatch"};
            std::array<char, 16> path {"Bj\xc3\xb6rk.flac"};
            std::array<char, 4> option {"-j"};
            std::array argv {executable.data(), path.data(), option.data()};

            const juce::ArgumentList arguments(static_cast<int>(argv.size()), argv.data());
            expectEquals(arguments.size(), 2);
            expectEquals(arguments[0].text, juce::String::fromUTF8("Bj\xc3\xb6rk.flac"));
            expectEquals(arguments[0].text.length(), 10);
            expectEquals(cli::joinArguments(arguments), juce::String::fromUTF8("Bj\xc3\xb6rk.flac -j"));
        }
#endif

        runInputTests();
        runOutputTests();
    }

private:
    /// Checks that inputs named on the command line which produce no result are reported as errors.
    void runInputTests()
    {
        const TemporaryDirectory directory;
        const auto audio = directory.copyFixture("sine_stereo_44100_16.wav", "tone.wav");
        const auto text = directory.getChildFile("notes.txt");
        expect(text.replaceWithText("not audio"));
        const auto hidden = directory.copyFixture("sine_stereo_44100_16.wav", ".hidden.wav");
        const auto missing = directory.getChildFile("missing.wav");

        AudioAnalysisRecord analyzed;
        analyzed.file = audio;
        analyzed.fullPath = audio.getFullPathName();
        const std::vector results {analyzed};

        beginTest("Inputs that produced a result, and folders, are not reported");
        {
            const juce::Array<juce::File> inputs {audio, directory.getChildFile({})};
            expect(cli::findSkippedInputs(inputs, results).isEmpty());
        }

        beginTest("Missing, unsupported and hidden inputs are reported");
        {
            const auto errors = cli::findSkippedInputs({missing, text, hidden}, results);
            expectEquals(errors.size(), 3);
            expect(errors[0].startsWith("Input not found"), errors[0]);
            expect(errors[1].startsWith("Not a supported audio file"), errors[1]);
            expect(errors[2].startsWith("Hidden files are not analyzed"), errors[2]);
        }
    }

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
