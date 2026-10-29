/// Tests for the CLI argument parsing in cli::parseArguments, the usage texts, the shell completion scripts,
/// and the CSV and JSON output.
/// Covers option values, aliases, subcommands, the combinations the CLI rejects, and the machine-readable record
/// fields, the option matching and UTF-8 decoding of process arguments fixed in the JUCE fork, and the errors for
/// inputs that produced no result.
/// Also runs the analysis workflow with a cache in a temporary folder and checks its exit codes.

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "cli/AudioAnalysisCli.h"
#include "cli/AudioAnalysisCliOutput.h"
#include "cli/CliOptions.h"
#include "cli/PluginListCli.h"
#include "cli/ShellCompletion.h"
#include "normalization/AudioNormalization.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <concepts>
#include <expected>
#include <memory>
#include <variant>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Parses the given arguments as if they followed the executable name on the command line.
std::expected<CliCommand, CliParseError> parseArguments(const juce::StringArray& arguments)
{
    return cli::parseArguments(juce::ArgumentList("audiobatch", arguments));
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
    template<std::default_initializable Command>
    Command expectCommand(const juce::StringArray& arguments)
    {
        const auto command = parseArguments(arguments);
        expect(command.has_value(), command.has_value() ? juce::String() : command.error().message);

        const auto* parsed = command.has_value() ? std::get_if<Command>(&*command) : nullptr;
        expect(!command.has_value() || parsed != nullptr, "The arguments parsed into another command");
        return parsed != nullptr ? *parsed : Command {};
    }

    /// Parses arguments that are expected to be rejected, and checks why they were.
    /// Returns the error message, for the few checks where the text itself is the behaviour under test.
    juce::String expectError(const juce::StringArray& arguments, const CliParseErrorKind kind)
    {
        const auto command = parseArguments(arguments);
        const auto commandLine = arguments.joinIntoString(" ");
        expect(!command.has_value(), utils::format("{} was accepted", commandLine));
        if (command.has_value()) {
            return {};
        }

        expect(command.error().kind == kind, utils::format("{}: {}", commandLine, command.error().message));
        expect(command.error().message.isNotEmpty(), commandLine);
        return command.error().message;
    }

    /// Runs the CLI parsing checks.
    void runTest() override
    {
        beginTest("Separate short options are parsed");
        {
            const auto command = expectCommand<AnalyzeCommand>({"-r", "-f", "music"});
            expect(command.recursive);
            expect(command.refresh);
            expect(!command.reverse);
            expect(!command.normalization.has_value());
            expectEquals(command.inputPaths.size(), 1);
        }

        beginTest("Combined short options are split, and an option that takes a value takes the rest");
        {
            auto command = expectCommand<AnalyzeCommand>({"-rfS", "music"});
            expect(command.recursive);
            expect(command.refresh);
            expect(command.reverse);
            expectEquals(command.inputPaths.size(), 1);

            command = expectCommand<AnalyzeCommand>({"-rj4", "-ojson"});
            expect(command.recursive);
            expectEquals(command.workerCount, 4);
            expect(command.outputFormat == AudioAnalysisOutputFormat::json);

            command = expectCommand<AnalyzeCommand>({"-o=csv", "-rj", "3"});
            expect(command.outputFormat == AudioAnalysisOutputFormat::csv);
            expectEquals(command.workerCount, 3);

            const auto listing = expectCommand<ListPluginsCommand>({"list", "-Rgtype"});
            expect(listing.rescan);
            expect(listing.grouping == PluginListGrouping::type);

            const auto message = expectError({"-rx"}, CliParseErrorKind::unknownOption);
            expectEquals(message, juce::String("Unknown option: -x"));
        }

        beginTest("A value that contains the letter of the help or version flag is not taken for the flag");
        {
            for (const auto* attached : {"-spath", "-s=path"}) {
                const auto expanded
                    = cli::expandShortOptions(juce::ArgumentList("audiobatch", juce::StringArray {attached, "music"}));
                expect(!expanded.containsOption("--help|-h"), attached);
                expect(!expanded.containsOption("--version|-V"), attached);
                expect(expectCommand<AnalyzeCommand>({attached, "music"}).sortMode == AudioAnalysisSortMode::path);
            }
            const auto combined = cli::expandShortOptions(juce::ArgumentList("audiobatch", juce::StringArray {"-rh"}));
            expect(combined.containsOption("--help|-h"));
        }

        beginTest("Negative numbers never match short options");
        {
            expectError({"-j", "-1"}, CliParseErrorKind::invalidValue);
            expectError({"-12", "song.wav"}, CliParseErrorKind::unexpectedNumber);

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
                expectError({option, "-14", "song.wav"}, CliParseErrorKind::unexpectedNumber);
            }
        }

        beginTest("Option values are accepted after an equals sign or as the next argument");
        {
            expectEquals(expectCommand<AnalyzeCommand>({"--jobs=4"}).workerCount, 4);
            expectEquals(expectCommand<AnalyzeCommand>({"--jobs", "5"}).workerCount, 5);
            expectEquals(expectCommand<AnalyzeCommand>({"-j", "3"}).workerCount, 3);

            expectError({"-j", "0"}, CliParseErrorKind::valueOutOfRange);
            expectError({"-j", "4x"}, CliParseErrorKind::invalidValue);
            expectError({"-j", "2.5"}, CliParseErrorKind::invalidValue);
            expectError({"-j", "100000"}, CliParseErrorKind::valueOutOfRange);
            expectEquals(expectCommand<AnalyzeCommand>({"-j", "0003"}).workerCount, 3);

            // A number too long for 64 bits must not wrap around into zero or a negative count.
            const auto message = expectError({"-j", "18446744073709551615"}, CliParseErrorKind::valueOutOfRange);
            expect(message.contains("at most"), message);
        }

        beginTest("Sort modes and the loudness alias are recognised");
        {
            expect(expectCommand<AnalyzeCommand>({"--sort", "loudness"}).sortMode == AudioAnalysisSortMode::loudness);
            expect(expectCommand<AnalyzeCommand>({"--sort=LUFS"}).sortMode == AudioAnalysisSortMode::loudness);
            expect(expectCommand<AnalyzeCommand>({"-s", "name"}).sortMode == AudioAnalysisSortMode::name);
            expect(expectCommand<AnalyzeCommand>({"-s", "path"}).sortMode == AudioAnalysisSortMode::path);

            // The error lists the names from the same table the parser reads, so the two cannot drift apart.
            const auto message = expectError({"--sort", "bogus"}, CliParseErrorKind::invalidValue);
            expect(message.contains("peak, lufs, loudness, name, path"), message);
        }

        beginTest("Plugin listing options require the list command and exclude analysis options");
        {
            expectError({"-g", "type"}, CliParseErrorKind::missingRequiredOption);
            expectError({"-R"}, CliParseErrorKind::missingRequiredOption);
            expectError({"list", "-r"}, CliParseErrorKind::conflictingOptions);
            expectError({"list", "music"}, CliParseErrorKind::unexpectedInput);
            expectError({"list", "--bogus"}, CliParseErrorKind::unknownOption);
            expectError({"list", "-g", "size"}, CliParseErrorKind::invalidValue);
            expectError({"--list-plugins"}, CliParseErrorKind::unknownOption);

            auto command = expectCommand<ListPluginsCommand>({"list"});
            expect(command.grouping == PluginListGrouping::name);
            expect(!command.rescan);

            command = expectCommand<ListPluginsCommand>({"list", "-g", "manufacturer"});
            expect(command.grouping == PluginListGrouping::manufacturer);
            expect(!command.rescan);

            command = expectCommand<ListPluginsCommand>({"list", "--group-by", "type", "-R"});
            expect(command.grouping == PluginListGrouping::type);
            expect(command.rescan);
        }

        beginTest("Subcommands are only recognised as the first argument");
        {
            const auto subcommandOf = [](const juce::StringArray& arguments) {
                return cli::findSubcommand(juce::ArgumentList("audiobatch", arguments));
            };
            expect(subcommandOf({"list", "-R"}) == CliSubcommand::list);
            expect(subcommandOf({"completion", "zsh"}) == CliSubcommand::completion);
            expect(subcommandOf({"-r", "list"}) == CliSubcommand::none);
            expect(subcommandOf({"./list"}) == CliSubcommand::none);
            expect(subcommandOf({}) == CliSubcommand::none);

            const auto command = expectCommand<AnalyzeCommand>({"-r", "list"});
            expect(command.recursive);
            expectEquals(command.inputPaths.size(), 1);
            expectEquals(command.inputPaths[0].getFileName(), juce::String("list"));
        }

        beginTest("The completion command takes one shell and its own options");
        {
            auto command = expectCommand<CompletionCommand>({"completion", "zsh"});
            expect(command.shell == CompletionShell::zsh);
            expect(!command.install);
            expect(!command.verbose);

            command = expectCommand<CompletionCommand>({"completion", "PowerShell", "-I", "--verbose"});
            expect(command.shell == CompletionShell::powershell);
            expect(command.install);
            expect(command.verbose);

            expectError({"completion"}, CliParseErrorKind::missingArgument);
            expectError({"completion", "tcsh"}, CliParseErrorKind::invalidValue);
            expectError({"completion", "zsh", "bash"}, CliParseErrorKind::unexpectedInput);
            expectError({"completion", "zsh", "-r"}, CliParseErrorKind::unknownOption);
        }

        beginTest("Usage texts list the options of each command");
        {
            const auto usage = cli::buildUsage("audiobatch");
            expect(usage.contains("  -j, --jobs <count>      Override worker count"), usage);
            expect(usage.contains("      --peak <dBFS>       Normalize to this sample peak"), usage);
            expect(usage.contains("  completion              "), usage);
            expect(!usage.contains("--group-by"), usage);

            const auto listUsage = cli::buildUsage("audiobatch", CliSubcommand::list);
            expect(listUsage.contains("Usage: audiobatch list [options]"), listUsage);
            expect(listUsage.contains("  -g, --group-by <mode>   Group the plugin list"), listUsage);
            expect(!listUsage.contains("--recurse"), listUsage);

            const auto completionUsage = cli::buildUsage("audiobatch", CliSubcommand::completion);
            expect(completionUsage.contains("Usage: audiobatch completion <shell> [options]"), completionUsage);
            expect(completionUsage.contains("zsh, bash, fish, powershell"), completionUsage);

            const auto helpUsage = cli::buildUsage("audiobatch", CliSubcommand::help);
            expect(helpUsage.contains("Usage: audiobatch help [command] [options]"), helpUsage);
        }

        beginTest("The help command takes at most one command to describe");
        {
            expect(expectCommand<HelpCommand>({"help"}).subcommand == CliSubcommand::none);
            expect(expectCommand<HelpCommand>({"help", "list"}).subcommand == CliSubcommand::list);
            expect(expectCommand<HelpCommand>({"help", "completion"}).subcommand == CliSubcommand::completion);
            expect(expectCommand<HelpCommand>({"help", "help"}).subcommand == CliSubcommand::help);

            expectError({"help", "analyze"}, CliParseErrorKind::invalidValue);
            expectError({"help", "list", "completion"}, CliParseErrorKind::unexpectedInput);
            expectError({"help", "-r"}, CliParseErrorKind::unknownOption);
        }

        beginTest("Force requires normalize");
        {
            expectError({"--force"}, CliParseErrorKind::missingRequiredOption);

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

            expectError({"-o", "xml"}, CliParseErrorKind::invalidValue);
            expectError({"list", "-o", "json"}, CliParseErrorKind::conflictingOptions);
        }

        beginTest("Normalization targets are parsed and validated");
        {
            const auto normalizationOf = [this](const juce::StringArray& arguments) {
                const auto normalization = expectCommand<AnalyzeCommand>(arguments).normalization;
                expect(normalization.has_value());
                return normalization.value_or(AudioNormalizationOptions {});
            };

            // The targets differ from the defaults, so an option that is ignored fails the check.
            auto normalization = normalizationOf({"-n", "--lufs", "-15"});
            expect(normalization.mode == AudioNormalizationMode::loudness);
            expectEquals(normalization.targetLufs, -15.0);
            expectEquals(normalization.targetPeakDbfs, AudioNormalizationOptions::defaultTargetPeakDbfs);

            normalization = normalizationOf({"-n", "--lufs=-16.5"});
            expectEquals(normalization.targetLufs, -16.5);

            normalization = normalizationOf({"-n", "--peak", "-1"});
            expect(normalization.mode == AudioNormalizationMode::peak);
            expectEquals(normalization.targetPeakDbfs, -1.0);
            expectEquals(normalization.targetLufs, AudioNormalizationOptions::defaultTargetLufs);

            normalization = normalizationOf({"-n"});
            expect(normalization.mode == AudioNormalizationMode::peak);
            expectEquals(normalization.targetPeakDbfs, AudioNormalizationOptions::defaultTargetPeakDbfs);

            expectError({"-n", "--lufs", "-30"}, CliParseErrorKind::valueOutOfRange);
            expectError({"-n", "--peak", "1"}, CliParseErrorKind::valueOutOfRange);
            expectError({"-n", "--lufs", "loud"}, CliParseErrorKind::invalidValue);
            expectError({"-n", "--peak", "-1", "--lufs", "-15"}, CliParseErrorKind::conflictingOptions);
            expectError({"--lufs", "-15"}, CliParseErrorKind::missingRequiredOption);
        }

        beginTest("Unknown options are rejected");
        {
            expectError({"--bogus"}, CliParseErrorKind::unknownOption);
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
        runAnalysisExitCodeTests();
        runCompletionTests();
    }

private:
    /// Checks that every completion script offers the options, values, and subcommands of the parser,
    /// and that each shell's script is installed to its per-user directory.
    void runCompletionTests()
    {
        beginTest("Completion scripts offer every option, value, and subcommand");
        {
            for (const auto& shellName : cli::completionShellNames) {
                const auto script = cli::buildCompletionScript(shellName.value, "audiobatch");
                const auto label = juce::String(shellName.name);
                expect(!script.containsChar('\r'), label);
                expect(!script.contains("@COMMAND@") && !script.contains("@ID@"), label + " has an unfilled token");

                // Fish names long options without their dashes, as in "-l recurse".
                const auto expectOption = [&](const CliOption& option) {
                    const auto name = juce::String(option.longName).trimCharactersAtStart("-");
                    expect(script.contains(name), label + " misses " + option.longName);
                };
                std::ranges::for_each(cli::analysisOptions, expectOption);
                std::ranges::for_each(cli::listOptions, expectOption);
                std::ranges::for_each(cli::completionOptions, expectOption);
                for (const auto* value : cli::sortModeValues) {
                    expect(script.contains(value), label + " misses the sort mode " + value);
                }
                for (const auto& subcommand : cli::subcommands) {
                    expect(script.contains(subcommand.name), label + " misses " + subcommand.name);
                }
            }

            const auto zsh = cli::buildCompletionScript(CompletionShell::zsh, "audiobatch");
            expect(zsh.startsWith("#compdef audiobatch\n"), zsh);
            expect(zsh.contains(R"('(-s --sort)'{-s,--sort=}'[Sort by peak, lufs, name, or path]:mode:()"), zsh);
            expect(zsh.contains(R"(shell'\''s completion directory)"), zsh);

            const auto bash = cli::buildCompletionScript(CompletionShell::bash, "audio-batch");
            expect(bash.contains("complete -o filenames -F _audio_batch audio-batch"), bash);

            const auto fish = cli::buildCompletionScript(CompletionShell::fish, "audiobatch");
            expect(fish.contains("-s o -l output -x -a 'table csv json'"), fish);
            expect(fish.contains(R"(shell\'s completion directory)"), fish);

            const auto powerShell = cli::buildCompletionScript(CompletionShell::powershell, "audiobatch");
            expect(powerShell.contains("-CommandName 'audiobatch'"), powerShell);
            expect(powerShell.contains("shell''s completion directory"), powerShell);
        }

        beginTest("Completion scripts are installed to the per-user directory of each shell");
        {
            const TemporaryDirectory home;
            const auto homeDirectory = home.getChildFile({});
            const auto installPath = [&homeDirectory](const CompletionShell shell) {
                return cli::findCompletionFile(shell, "audiobatch", homeDirectory).getFullPathName();
            };

            expectEquals(
                installPath(CompletionShell::zsh),
                homeDirectory.getChildFile(".zsh")
                    .getChildFile("completions")
                    .getChildFile("_audiobatch")
                    .getFullPathName()
            );
            expectEquals(
                installPath(CompletionShell::bash),
                homeDirectory.getChildFile(".bash_completion.d").getChildFile("audiobatch.bash").getFullPathName()
            );
            expect(installPath(CompletionShell::fish).endsWith("audiobatch.fish"));
            expect(installPath(CompletionShell::powershell).endsWith("_audiobatch.ps1"));

            // With oh-my-zsh, the script becomes a custom plugin named after the command.
            const auto plugins
                = homeDirectory.getChildFile(".oh-my-zsh").getChildFile("custom").getChildFile("plugins");
            expect(plugins.createDirectory().wasOk());
            expectEquals(
                installPath(CompletionShell::zsh),
                plugins.getChildFile("audiobatch").getChildFile("_audiobatch").getFullPathName()
            );
        }
    }

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
        quoted.formatName = "WAV";
        quoted.sampleRate = 44'100;
        quoted.channels = 2;
        quoted.bitsPerSample = 24;
        quoted.durationSeconds = 1.5;
        // Every level differs from the others, so a field that reports the wrong measurement fails the check.
        quoted.overallPeak = 0.5f;
        quoted.overallTruePeak = 0.8;
        quoted.integratedLufs = -14.25;
        quoted.maxShortTermLufs = -11.5;
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
            const juce::String header {"path,name,format,sampleRate,channels,bitsPerSample,durationSeconds,"
                                       "peakDbfs,truePeakDbtp,integratedLufs,maxShortTermLufs,status,error"};
            const auto emptyLines = juce::StringArray::fromLines(cli::formatRecordsAsCsv({}).trimEnd());
            expectEquals(emptyLines.size(), 1);
            expectEquals(emptyLines[0], header);

            const auto csv = cli::formatRecordsAsCsv(records);
            expect(!csv.containsChar('\r'));
            const auto lines = juce::StringArray::fromLines(csv.trimEnd());
            expectEquals(lines.size(), 4);
            expectEquals(lines[0], header);
            expectEquals(
                lines[1],
                juce::String(
                    R"("/music/a, ""b"".wav","a, ""b"".wav",WAV,44100,2,24,1.5,-6.02,-1.94,-14.25,-11.5,analyzed,)"
                )
            );
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

            expectEquals(parsed[0]["path"].toString(), quoted.fullPath);
            expectEquals(parsed[0]["name"].toString(), quoted.fileName);
            expectEquals(parsed[0]["format"].toString(), juce::String("WAV"));
            expectEquals(static_cast<int>(parsed[0]["sampleRate"]), 44'100);
            expectEquals(static_cast<int>(parsed[0]["channels"]), 2);
            expectEquals(static_cast<int>(parsed[0]["bitsPerSample"]), 24);
            expectEquals(static_cast<double>(parsed[0]["durationSeconds"]), 1.5);
            expectEquals(static_cast<double>(parsed[0]["peakDbfs"]), -6.02);
            expectEquals(static_cast<double>(parsed[0]["truePeakDbtp"]), -1.94);
            expectEquals(static_cast<double>(parsed[0]["integratedLufs"]), -14.25);
            expectEquals(static_cast<double>(parsed[0]["maxShortTermLufs"]), -11.5);
            expectEquals(parsed[0]["status"].toString(), juce::String("analyzed"));

            // The levels are written as rounded, without the binary rounding error of the double that holds them.
            expect(json.contains("\"peakDbfs\": -6.02,"), json);
            expect(json.contains("\"truePeakDbtp\": -1.94,"), json);

            expect(parsed[1]["peakDbfs"].isVoid());
            expect(parsed[1]["integratedLufs"].isVoid());
            expectEquals(parsed[2]["status"].toString(), juce::String("failed"));
            expectEquals(parsed[2]["error"].toString(), failed.errorMessage);
        }
    }

    /// Runs the analysis workflow of the CLI with a cache in a temporary folder and checks its exit codes:
    /// 0 when every input was analyzed and normalized, and 2 when any input was skipped or failed.
    void runAnalysisExitCodeTests()
    {
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("cli_cache.db"));
        const auto makeCommand = [](const juce::Array<juce::File>& inputs) {
            AnalyzeCommand command;
            command.inputPaths = inputs;
            command.workerCount = 2;
            command.outputFormat = AudioAnalysisOutputFormat::json;
            return command;
        };

        beginTest("The analysis exits with 0 when every input is analyzed, and stores the results in the given cache");
        {
            const auto first = directory.copyFixture("sine_stereo_44100_16.wav", "first.wav");
            const auto second = directory.copyFixture("sine_mono_44100_16.wav", "second.wav");
            expectEquals(cli::runAnalysis(makeCommand({first, second}), cache), 0);
            expect(cache.getAnalysis(first).has_value());
            expect(cache.getAnalysis(second).has_value());

            // The second run reads both results from the cache and still succeeds.
            expectEquals(cli::runAnalysis(makeCommand({first, second}), cache), 0);
        }

        beginTest("The analysis exits with 2 for a missing, unsupported, or broken input among good ones");
        {
            const auto good = directory.copyFixture("sine_stereo_44100_16.wav", "good.wav");
            const auto text = directory.getChildFile("notes.txt");
            expect(text.replaceWithText("not audio"));
            const auto broken = directory.copyFixture("broken.wav");
            for (const auto& bad : {directory.getChildFile("missing.wav"), text, broken}) {
                expectEquals(cli::runAnalysis(makeCommand({good, bad}), cache), 2, bad.getFileName());
            }
        }

        beginTest("Normalizing in place exits with 0, and a refused or conflicting file exits with 2");
        {
            const auto inPlace = directory.copyFixture("sine_stereo_44100_16.aif", "normalized.aif");
            auto command = makeCommand({inPlace});
            command.normalization = testNormalizationOptions();
            command.normalization->targetPeakDbfs = -1.0;
            expectEquals(cli::runAnalysis(command, cache), 0);
            expectWithinAbsoluteError(peakDecibels(analyzeFile(inPlace)), -1.0, 0.01);

            const auto silent = directory.copyFixture("silence_stereo_44100_16.wav", "silent.wav");
            command.inputPaths = {silent};
            expectEquals(cli::runAnalysis(command, cache), 2);
            expect(silent.existsAsFile());

            const auto winner = directory.copyFixture("sine_stereo_44100_16.aif", "conflict.aif");
            const auto loser = directory.copyFixture("sine_stereo_44100_16.wav", "conflict.wav");
            command.inputPaths = {winner, loser};
            expectEquals(cli::runAnalysis(command, cache), 2);
            expect(loser.existsAsFile());
        }

        beginTest("Normalizing several files on several workers converts each one and caches its output");
        {
            const auto first = directory.copyFixture("sine_stereo_44100_16.wav", "parallel_first.wav");
            const auto second = directory.copyFixture("sine_mono_44100_16.wav", "parallel_second.wav");
            const auto third = directory.copyFixture("sine_stereo_44100_16.flac", "parallel_third.flac");
            auto command = makeCommand({first, second, third});
            command.workerCount = 3;
            command.normalization = testNormalizationOptions();
            command.normalization->targetPeakDbfs = -1.0;
            expectEquals(cli::runAnalysis(command, cache), 0);
            for (const auto& source : {first, second, third}) {
                const auto output = source.withFileExtension("aif");
                expect(output.existsAsFile(), output.getFileName());
                expect(!source.existsAsFile(), source.getFileName());
                expectWithinAbsoluteError(peakDecibels(analyzeFile(output)), -1.0, 0.01);
                expect(cache.getAnalysis(output).has_value(), output.getFileName());
            }
        }

        beginTest("The analysis still succeeds when the cache cannot be opened");
        {
            const auto notDatabase = directory.getChildFile("not_a_database.db");
            expect(notDatabase.replaceWithText("This text is not an SQLite database."));
            AnalysisCache brokenCache(notDatabase);
            const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "uncached.wav");
            expectEquals(cli::runAnalysis(makeCommand({file}), brokenCache), 0);
            expect(!brokenCache.getAnalysis(file).has_value());
        }
    }
};

std::unique_ptr<juce::UnitTest> createCliTests()
{
    return std::make_unique<CliTests>();
}
}  // namespace audiobatch::tests
