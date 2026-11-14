/// Implementation of the CLI argument handling: the usage texts,
/// and the parsing of the arguments into a validated AnalyzeCommand,
/// or the ListPluginsCommand or CompletionCommand of the subcommand named by the first argument.
/// The options, their usage lines, and the names each enum option accepts come from the tables in CliOptions.h.

#include "cli/AudioAnalysisCli.h"
#include "cli/CliOptions.h"
#include "normalization/AudioNormalization.h"
#include "utils/FileUtils.h"
#include "utils/OptionalRef.h"
#include "utils/StringFormat.h"
#include "utils/version.h"
#include <fmt/format.h>
#include <string_view>
#include <type_traits>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
/// Lowest upper limit for the worker count, used on machines with few cores.
constexpr int MINIMUM_WORKER_LIMIT {64};
/// Longest worker count text that is parsed, which keeps the number far from the 64-bit limit.
constexpr int MAXIMUM_WORKER_COUNT_DIGITS {9};
/// Lowercase AUDIOBATCH_DELETE values that enable permanent deletion, the same set clap accepts as true.
constexpr std::array<std::string_view, 6> DELETE_TRUE_VALUES {"y", "yes", "t", "true", "on", "1"};
/// Lowercase AUDIOBATCH_DELETE values that keep trash disposal, the same set clap accepts as false.
constexpr std::array<std::string_view, 6> DELETE_FALSE_VALUES {"n", "no", "f", "false", "off", "0"};

/// The command line split at the first bare "--", which ends the options like it does for clap.
struct SeparatedArguments {
    /// The arguments before the separator, which may hold options.
    juce::ArgumentList options;
    /// The arguments after the separator, taken literally as positional arguments even when they start with "-".
    juce::StringArray literals;
};

/// Where the first help or version flag of a command line is, and which of the two it is.
struct HelpOrVersionFlag {
    /// Index of the flag in the expanded arguments before any "--".
    int index {0};
    /// True for --version or -V, false for --help or -h.
    bool version {false};
};

/// Returns a rejected command line, ready to return from a function whose result is a std::expected.
std::unexpected<CliParseError> parseError(const CliParseErrorKind kind, juce::String message)
{
    return std::unexpected(CliParseError {.kind = kind, .message = std::move(message)});
}

/// Returns true when the argument is a plain number such as "-14" or "0.5",
/// which may start with a minus sign but is a value and not an option.
bool isNumericArgument(const juce::String& text)
{
    const auto digits = text.startsWithChar('-') || text.startsWithChar('+') ? text.substring(1) : text;
    return digits.isNotEmpty() && digits.containsOnly("0123456789.") && digits.containsAnyOf("0123456789")
        && digits.indexOfChar('.') == digits.lastIndexOfChar('.');
}

/// Removes an option and its value from the arguments.
/// Returns nothing when the option is absent, and an empty string when it is present without a value,
/// which juce::ArgumentList::removeValueForOption does not tell apart.
std::optional<juce::String> takeOptionValue(juce::ArgumentList& arguments, const CliOption& option)
{
    const auto pattern = option.pattern();

    if (!arguments.containsOption(pattern)) {
        return std::nullopt;
    }

    return arguments.removeValueForOption(pattern);
}

/// Returns the value whose name matches the text, ignoring case but not surrounding space,
/// or nothing for an unknown name.
template<typename Enum, std::size_t count>
    requires std::is_enum_v<Enum>
std::optional<Enum> parseName(const std::array<OptionName<Enum>, count>& names, const juce::String& text)
{
    const auto token = text.toLowerCase();
    const auto match
        = std::ranges::find_if(names, [&token](const OptionName<Enum>& entry) { return token == entry.name; });

    return match == names.end() ? std::nullopt : std::optional(match->value);
}

/// Joins the accepted names for an error message, such as "table, csv, json".
template<typename Enum, std::size_t count>
    requires std::is_enum_v<Enum>
juce::String joinNames(const std::array<OptionName<Enum>, count>& names)
{
    juce::StringArray texts;

    for (const auto& entry : names) {
        texts.add(entry.name);
    }

    return texts.joinIntoString(", ");
}

/// Removes an option that takes one of the given names, and returns the value for it.
/// Returns nothing when the option is absent, and an error naming the accepted values for an unknown name.
template<typename Enum, std::size_t count>
    requires std::is_enum_v<Enum>
std::expected<std::optional<Enum>, CliParseError> takeNamedOption(
    juce::ArgumentList& arguments,
    const CliOption& option,
    const juce::String& description,
    const std::array<OptionName<Enum>, count>& names
)
{
    const auto text = takeOptionValue(arguments, option);

    if (!text.has_value()) {
        return std::nullopt;
    }

    if (const auto value = parseName(names, *text); value.has_value()) {
        return value;
    }

    return parseError(
        CliParseErrorKind::invalidValue, utils::format("{} must be one of: {}", description, joinNames(names))
    );
}

/// Borrows the option whose short form is the given letter from the process-lifetime command tables.
utils::OptionalRef<const CliOption> findShortOption(const juce::juce_wchar letter)
{
    const auto matches = [letter](const CliOption& option) {
        return option.hasShortName() && juce::String(option.shortName).getLastCharacter() == letter;
    };

    for (const CliOption& option : {std::cref(cli::HELP_OPTION), std::cref(cli::VERSION_OPTION)}) {
        if (matches(option)) {
            return option;
        }
    }

    for (const auto options :
         {
             CliOptionList(cli::ANALYSIS_OPTIONS),
             CliOptionList(cli::PROCESS_OPTIONS),
             CliOptionList(cli::LIST_OPTIONS),
             CliOptionList(cli::COMPLETION_OPTIONS),
         })
    {
        for (const CliOption& option : options) {
            if (matches(option)) {
                return option;
            }
        }
    }

    return std::nullopt;
}

/// Splits one argument of combined short options, such as "-rf" or "-j4", into separate arguments,
/// the way common command line parsers accept them.
/// A letter of an option that takes a value takes the rest of the argument as its value,
/// so "-j4" and "-o=json" become "-j 4" and "-o json".
/// A letter of no option becomes an option of its own, which the parser then reports as unknown.
juce::StringArray splitCombinedShortOptions(const juce::String& text)
{
    juce::StringArray split;

    for (int index = 1; index < text.length(); ++index) {
        const auto option = findShortOption(text[index]);
        if (!option) {
            split.add(utils::format("-{}", text.substring(index, index + 1)));
            continue;
        }

        split.add(option->get().shortName);

        if (option->get().takesValue()) {
            if (const auto value = text.substring(index + 1).trimCharactersAtStart("="); value.isNotEmpty()) {
                split.add(value);
            }
            break;
        }
    }

    return split;
}

/// Expands every argument of combined short options into separate arguments.
/// juce::ArgumentList matches a short option by searching for its letter anywhere in the argument,
/// and removes the whole argument on the first match, so "-rf" would otherwise set only one of the flags.
/// Negative numbers such as "-14" are values, and stay as they are.
/// The arguments are added as they are, since building the list from strings would unquote them a second time.
juce::ArgumentList expandCombinedShortOptions(const juce::ArgumentList& arguments)
{
    juce::ArgumentList expanded {arguments.executableName, juce::StringArray {}};

    for (const auto& argument : arguments.arguments) {
        if (argument.isShortOption() && argument.text.length() > 2 && !isNumericArgument(argument.text)) {
            for (const auto& text : splitCombinedShortOptions(argument.text)) {
                expanded.arguments.add({.text = text});
            }
        } else {
            expanded.arguments.add(argument);
        }
    }

    return expanded;
}

/// Splits the arguments at the first bare "--".
/// Combined short options are expanded only before it, so a literal such as "-rf.wav" stays one path.
SeparatedArguments separateArguments(const juce::ArgumentList& commandLine)
{
    SeparatedArguments separated {.options = {commandLine.executableName, juce::StringArray {}}, .literals = {}};
    bool literal = false;
    for (const auto& argument : commandLine.arguments) {
        if (literal) {
            separated.literals.add(argument.text);
        } else if (argument.text == "--") {
            literal = true;
        } else {
            separated.options.arguments.add(argument);
        }
    }
    separated.options = expandCombinedShortOptions(separated.options);
    return separated;
}

/// Reads AUDIOBATCH_DELETE without treating an empty value as an absent variable.
std::optional<juce::String> readDeleteEnvironmentValue()
{
#if JUCE_MSVC
    std::size_t length = 0;
    if (_wgetenv_s(&length, nullptr, 0, L"AUDIOBATCH_DELETE") != 0 || length == 0) {
        return std::nullopt;
    }
    return juce::SystemStats::getEnvironmentVariable("AUDIOBATCH_DELETE", {});
#else
    return juce::SystemStats::getEnvironmentVariable("AUDIOBATCH_DELETE", "false");
#endif
}

/// Resolves --delete ahead of AUDIOBATCH_DELETE, accepting the same case-insensitive boolish values as clap:
/// y, yes, t, true, on, and 1, or n, no, f, false, off, and 0, without surrounding whitespace.
/// An absent variable keeps trash disposal, while an empty or unrecognized value is rejected.
std::expected<bool, CliParseError> parseDeleteOriginals(
    const bool explicitDelete,
    const std::optional<juce::String>& environmentValue
)
{
    if (explicitDelete) {
        return true;
    }

    if (!environmentValue.has_value()) {
        return false;
    }

    const auto value = environmentValue->toLowerCase().toStdString();
    if (std::ranges::contains(DELETE_FALSE_VALUES, value)) {
        return false;
    }

    if (std::ranges::contains(DELETE_TRUE_VALUES, value)) {
        return true;
    }

    return parseError(
        CliParseErrorKind::invalidValue,
        "AUDIOBATCH_DELETE must be one of: y, yes, t, true, on, 1, n, no, f, false, off, 0"
    );
}

/// Parses the worker count of --jobs.
/// Returns the count, or an error when the value is not a whole number in the supported range.
std::expected<int, CliParseError> parseWorkerCount(const juce::String& value)
{
    // The thread pool creates every worker up front, so the count is capped well above any useful value.
    const auto maximumWorkerCount = juce::jmax(MINIMUM_WORKER_LIMIT, juce::SystemStats::getNumCpus() * 4);
    const auto workerCountText = value.trim();

    if (workerCountText.isEmpty() || !workerCountText.containsOnly("0123456789")) {
        return parseError(CliParseErrorKind::invalidValue, "Worker count must be a whole number greater than zero");
    }

    const auto tooLarge = [maximumWorkerCount] {
        return parseError(
            CliParseErrorKind::valueOutOfRange, utils::format("Worker count must be at most {}", maximumWorkerCount)
        );
    };

    // The digit count is checked before parsing, since a longer number would wrap around while it is parsed.
    if (workerCountText.trimCharactersAtStart("0").length() > MAXIMUM_WORKER_COUNT_DIGITS) {
        return tooLarge();
    }

    const auto workerCount = workerCountText.getLargeIntValue();

    if (workerCount <= 0) {
        return parseError(CliParseErrorKind::valueOutOfRange, "Worker count must be greater than zero");
    }

    if (workerCount > maximumWorkerCount) {
        return tooLarge();
    }

    return static_cast<int>(workerCount);
}

/// Parses a target level in the given range for --peak or --lufs.
/// Returns the level, or an error when the value is not a number in the range.
std::expected<double, CliParseError> parseTargetLevel(
    const juce::String& option,
    const juce::String& value,
    const double minimum,
    const double maximum
)
{
    const auto trimmedValue = value.trim();

    if (!isNumericArgument(trimmedValue)) {
        return parseError(
            CliParseErrorKind::invalidValue,
            utils::format("{} needs a number, for example {} {}", option, option, minimum)
        );
    }

    const auto target = trimmedValue.getDoubleValue();

    if (target < minimum || target > maximum) {
        return parseError(
            CliParseErrorKind::valueOutOfRange, utils::format("{} must be between {} and {}", option, minimum, maximum)
        );
    }

    return target;
}

/// Builds the normalization options of --normalize from the values of --peak and --lufs,
/// keeping the default 0 dBFS sample peak target when neither is given.
/// Returns an error when both are given or a value is invalid.
std::expected<AudioNormalizationOptions, CliParseError> parseNormalization(
    const std::optional<juce::String>& peakValue,
    const std::optional<juce::String>& loudnessValue
)
{
    AudioNormalizationOptions normalization;

    if (peakValue.has_value() && loudnessValue.has_value()) {
        return parseError(CliParseErrorKind::conflictingOptions, "--peak and --lufs cannot be used together");
    }

    if (peakValue.has_value()) {
        const auto target = parseTargetLevel(
            "--peak",
            *peakValue,
            AudioNormalizationOptions::MINIMUM_TARGET_PEAK_DBFS,
            AudioNormalizationOptions::MAXIMUM_TARGET_PEAK_DBFS
        );

        if (!target) {
            return std::unexpected(target.error());
        }

        normalization.mode = AudioNormalizationMode::peak;
        normalization.targetPeakDbfs = *target;
    }

    if (loudnessValue.has_value()) {
        const auto target = parseTargetLevel(
            "--lufs",
            *loudnessValue,
            AudioNormalizationOptions::MINIMUM_TARGET_LUFS,
            AudioNormalizationOptions::MAXIMUM_TARGET_LUFS
        );

        if (!target) {
            return std::unexpected(target.error());
        }

        normalization.mode = AudioNormalizationMode::loudness;
        normalization.targetLufs = *target;
    }

    return normalization;
}

/// Returns an error for the first option left in the arguments, which no option of the command took.
std::expected<void, CliParseError> rejectRemainingOptions(const juce::ArgumentList& arguments)
{
    for (const auto& argument : arguments.arguments) {
        if (argument.isOption()) {
            return parseError(CliParseErrorKind::unknownOption, utils::format("Unknown option: {}", argument.text));
        }
    }

    return {};
}

/// Parses the arguments that follow the list subcommand into a plugin listing,
/// which takes only its grouping and rescan options, without input paths or analysis options.
std::expected<CliCommand, CliParseError> parseListPluginsCommand(juce::ArgumentList& arguments)
{
    ListPluginsCommand command;
    command.rescan = arguments.removeOptionIfFound(cli::RESCAN_OPTION.pattern());

    const auto grouping
        = takeNamedOption(arguments, cli::GROUP_BY_OPTION, "Plugin grouping", cli::PLUGIN_GROUPING_NAMES);
    if (!grouping) {
        return std::unexpected(grouping.error());
    }
    command.grouping = grouping->value_or(command.grouping);

    if (std::ranges::any_of(
            cli::ANALYSIS_OPTIONS,
            [&arguments](const CliOption& option) { return arguments.containsOption(option.pattern()); }
        ))
    {
        return parseError(CliParseErrorKind::unknownOption, "The list command does not take analysis options");
    }

    if (auto remaining = rejectRemainingOptions(arguments); !remaining) {
        return std::unexpected(std::move(remaining).error());
    }

    if (arguments.size() > 0) {
        return parseError(CliParseErrorKind::unexpectedInput, "The list command does not take input paths");
    }

    return command;
}

/// Parses the arguments that follow the completion subcommand into a completion request,
/// which takes exactly one shell name and the install and verbose options.
std::expected<CliCommand, CliParseError> parseCompletionCommand(juce::ArgumentList& arguments)
{
    CompletionCommand command;
    command.install = arguments.removeOptionIfFound(cli::INSTALL_OPTION.pattern());
    command.verbose = arguments.removeOptionIfFound(cli::VERBOSE_OPTION.pattern());

    if (auto remaining = rejectRemainingOptions(arguments); !remaining) {
        return std::unexpected(std::move(remaining).error());
    }

    const auto shellNames = joinNames(cli::COMPLETION_SHELL_NAMES);

    if (arguments.size() == 0) {
        return parseError(
            CliParseErrorKind::missingArgument, utils::format("The completion command needs a shell: {}", shellNames)
        );
    }

    if (arguments.size() > 1) {
        return parseError(CliParseErrorKind::unexpectedInput, "The completion command takes only one shell");
    }

    const auto shell = parseName(cli::COMPLETION_SHELL_NAMES, arguments[0].text);
    if (!shell.has_value()) {
        return parseError(CliParseErrorKind::invalidValue, utils::format("Shell must be one of: {}", shellNames));
    }

    command.shell = *shell;
    return command;
}

/// Parses the arguments of the help subcommand, which names at most one command to describe.
std::expected<CliCommand, CliParseError> parseHelpCommand(const juce::ArgumentList& arguments)
{
    if (auto remaining = rejectRemainingOptions(arguments); !remaining) {
        return std::unexpected(std::move(remaining).error());
    }

    if (arguments.size() > 1) {
        return parseError(CliParseErrorKind::unexpectedInput, "The help command takes only one command");
    }

    if (arguments.size() == 0) {
        return HelpCommand {};
    }

    const auto subcommand = cli::findSubcommand(arguments);
    if (subcommand == CliSubcommand::none) {
        juce::StringArray names;
        for (const auto* name : cli::HELP_COMMAND_VALUES) {
            names.add(name);
        }
        return parseError(
            CliParseErrorKind::invalidValue,
            utils::format("Unknown command: {}, expected one of: {}", arguments[0].text, names.joinIntoString(", "))
        );
    }

    return HelpCommand {.subcommand = subcommand};
}

/// Validates the input paths left after taking every supported analysis option,
/// followed by the literal paths after a bare "--", which are never options or stray values.
std::expected<juce::Array<juce::File>, CliParseError> parseInputPaths(
    const juce::ArgumentList& arguments,
    const juce::StringArray& literals
)
{
    juce::Array<juce::File> paths;
    for (const auto& argument : arguments.arguments) {
        if (argument.isOption()) {
            return parseError(CliParseErrorKind::unknownOption, utils::format("Unknown option: {}", argument.text));
        }

        // A negative number is a value, so one that no option took was most likely meant for the option before it.
        if (isNumericArgument(argument.text) && argument.text.startsWithChar('-')) {
            return parseError(
                CliParseErrorKind::unexpectedNumber,
                utils::format(
                    "Unexpected number {}. Pass a negative value right after the option that takes it.", argument.text
                )
            );
        }

        paths.add(argument.resolveAsFile());
    }
    for (const auto& literal : literals) {
        paths.add(juce::ArgumentList::Argument {.text = literal}.resolveAsFile());
    }
    return paths;
}

/// Parses the arguments into an analysis run, with the remaining arguments as its input paths.
/// The plugin listing options are rejected, and --force, --delete, --peak, and --lufs need --normalize.
/// An optional caller-owned processing command receives the parsed settings during this call only.
std::expected<CliCommand, CliParseError> parseAnalyzeCommand(
    juce::ArgumentList& arguments,
    const juce::StringArray& literals,
    const std::optional<juce::String>& deleteEnvironmentValue,
    const utils::OptionalRef<ProcessCommand> processing = std::nullopt
)
{
    if (arguments.containsOption(cli::GROUP_BY_OPTION.pattern())
        || arguments.containsOption(cli::RESCAN_OPTION.pattern()))
    {
        return parseError(
            CliParseErrorKind::unknownOption, "--group-by and --rescan can only be used with the list command"
        );
    }

    AnalyzeCommand command;
    command.recursive = arguments.removeOptionIfFound(cli::RECURSE_OPTION.pattern());
    command.refresh = arguments.removeOptionIfFound(cli::REFRESH_OPTION.pattern());
    command.reverse = arguments.removeOptionIfFound(cli::REVERSE_OPTION.pattern());
    const bool normalize = arguments.removeOptionIfFound(cli::NORMALIZE_OPTION.pattern());
    const bool force = arguments.removeOptionIfFound(cli::FORCE_OPTION.pattern());
    const auto deleteOriginals
        = parseDeleteOriginals(arguments.removeOptionIfFound(cli::DELETE_OPTION.pattern()), deleteEnvironmentValue);
    if (!deleteOriginals) {
        return std::unexpected(deleteOriginals.error());
    }

    if (const auto workerCountValue = takeOptionValue(arguments, cli::JOBS_OPTION); workerCountValue.has_value()) {
        const auto workerCount = parseWorkerCount(*workerCountValue);

        if (!workerCount) {
            return std::unexpected(workerCount.error());
        }

        command.workerCount = *workerCount;
    }

    const auto sortMode = takeNamedOption(arguments, cli::SORT_OPTION, "Sort mode", cli::SORT_MODE_NAMES);
    if (!sortMode) {
        return std::unexpected(sortMode.error());
    }
    command.sortMode = sortMode->value_or(command.sortMode);

    const auto outputFormat = takeNamedOption(arguments, cli::OUTPUT_OPTION, "Output format", cli::OUTPUT_FORMAT_NAMES);
    if (!outputFormat) {
        return std::unexpected(outputFormat.error());
    }
    command.outputFormat = outputFormat->value_or(command.outputFormat);

    const auto peakValue = takeOptionValue(arguments, cli::PEAK_OPTION);
    const auto loudnessValue = takeOptionValue(arguments, cli::LUFS_OPTION);

    if (force && !normalize && !processing) {
        return parseError(CliParseErrorKind::missingRequiredOption, "--force can only be used with --normalize");
    }

    if (*deleteOriginals && !normalize && !processing) {
        return parseError(CliParseErrorKind::missingRequiredOption, "--delete can only be used with --normalize");
    }

    if ((peakValue.has_value() || loudnessValue.has_value()) && !normalize) {
        return parseError(
            CliParseErrorKind::missingRequiredOption, "--peak and --lufs can only be used with --normalize"
        );
    }

    if (normalize) {
        auto normalization = parseNormalization(peakValue, loudnessValue);

        // Target levels are validated after every argument was read, so a later help flag still wins.
        if (!normalization) {
            auto error = normalization.error();
            error.deferred = true;
            return std::unexpected(std::move(error));
        }

        normalization->overwriteExisting = force;
        normalization->disposeFile = *deleteOriginals ? utils::deleteFile : utils::trashFile;
        command.normalization = std::move(*normalization);
    }

    auto paths = parseInputPaths(arguments, literals);
    if (!paths) {
        return std::unexpected(paths.error());
    }
    command.inputPaths = std::move(*paths);

    if (processing) {
        auto& processCommand = processing->get();
        processCommand.analysis = std::move(command);
        processCommand.options.overwriteExisting = force;
        processCommand.options.disposeFile = *deleteOriginals ? utils::deleteFile : utils::trashFile;
        processCommand.options.normalizeBeforePlugin = processCommand.analysis.normalization;
        return processCommand;
    }
    return command;
}

/// Returns the subcommand table entry for a subcommand, or nothing for an analysis run.
std::optional<CliSubcommandInfo> findSubcommandInfo(const CliSubcommand subcommand)
{
    for (const auto& info : cli::SUBCOMMANDS) {
        if (info.subcommand == subcommand) {
            return info;
        }
    }

    return std::nullopt;
}

/// Formats one line of a usage list, with the description in a column after the flags or the name.
juce::String usageRow(const std::string_view flags, const std::string_view description)
{
    return utils::format("  {:<22}  {}", flags, description);
}

/// Formats the usage line of an option, such as "  -j, --jobs <count>      Override worker count".
/// An option without a short form is indented to keep its long form in line with the others.
juce::String usageRow(const CliOption& option)
{
    std::string flags = option.hasShortName() ? fmt::format("{}, {}", option.shortName, option.longName)
                                              : fmt::format("    {}", option.longName);

    if (option.takesValue()) {
        flags += fmt::format(" <{}>", option.valueName);
    }

    return usageRow(flags, option.description);
}

/// Returns the placeholder of a subcommand's argument, such as "<shell>" or "[command]",
/// or an empty string when the subcommand takes no argument.
std::string argumentPlaceholder(const CliSubcommandInfo& subcommand)
{
    if (std::string_view(subcommand.argumentName).empty()) {
        return {};
    }
    return subcommand.argumentRequired ? fmt::format("<{}>", subcommand.argumentName)
                                       : fmt::format("[{}]", subcommand.argumentName);
}

/// Returns the subcommand followed by its argument placeholder, such as "completion <shell>".
std::string subcommandWithArgument(const CliSubcommandInfo& subcommand)
{
    const auto placeholder = argumentPlaceholder(subcommand);
    return placeholder.empty() ? std::string(subcommand.name) : fmt::format("{} {}", subcommand.name, placeholder);
}

/// Returns the usage line of a subcommand, such as "Usage: audiobatch completion <shell> [options]".
juce::String subcommandUsageLine(const juce::String& executableName, const CliSubcommandInfo& subcommand)
{
    return utils::format("Usage: {} {} [options]", executableName, subcommandWithArgument(subcommand));
}

/// Builds the help text of a subcommand, with its argument and its options.
juce::StringArray buildSubcommandUsage(const juce::String& executableName, const CliSubcommandInfo& subcommand)
{
    const bool takesArgument = !std::string_view(subcommand.argumentName).empty();

    juce::StringArray lines {
        utils::format("{} {}", executableName, version::VERSION_INFO),
        subcommand.description,
        "",
        subcommandUsageLine(executableName, subcommand),
    };

    if (takesArgument) {
        juce::StringArray values;
        for (const auto* value : subcommand.argumentValues) {
            values.add(value);
        }

        lines.add("");
        lines.add("Arguments:");
        lines.add(usageRow(
            argumentPlaceholder(subcommand),
            values.isEmpty() ? std::string(subcommand.argumentDescription)
                             : fmt::format("{}: {}", subcommand.argumentDescription, values.joinIntoString(", "))
        ));
    }

    lines.add("");
    lines.add("Options:");
    lines.add(usageRow(cli::HELP_OPTION));

    for (const CliOption& option : subcommand.options) {
        lines.add(usageRow(option));
    }

    return lines;
}
/// Returns the arguments of the literal paths after "--", which are never options.
juce::Array<juce::ArgumentList::Argument> literalArguments(const juce::StringArray& literals)
{
    juce::Array<juce::ArgumentList::Argument> arguments;
    for (const auto& literal : literals) {
        arguments.add({.text = literal});
    }
    return arguments;
}

/// Returns the first help or version flag, skipping the value of an option that takes one.
/// Version is only a flag of the analysis run, since subcommands do not take it.
/// The help subcommand never has a flag, since it takes only command names.
std::optional<HelpOrVersionFlag> findHelpOrVersionFlag(
    const juce::ArgumentList& arguments,
    const CliSubcommand subcommand
)
{
    // The help subcommand takes only command names, so a flag after it is reported as an unknown option.
    if (subcommand == CliSubcommand::help) {
        return std::nullopt;
    }
    const auto valueOptions = [subcommand] {
        std::vector<std::reference_wrapper<const CliOption>> options;
        if (subcommand == CliSubcommand::process) {
            options.assign(cli::PROCESS_OPTIONS.begin(), cli::PROCESS_OPTIONS.end());
        } else if (subcommand == CliSubcommand::list) {
            options.assign(cli::LIST_OPTIONS.begin(), cli::LIST_OPTIONS.end());
        } else if (subcommand == CliSubcommand::none) {
            options.assign(cli::ANALYSIS_OPTIONS.begin(), cli::ANALYSIS_OPTIONS.end());
        }
        return options;
    }();
    const auto takesValue = [&valueOptions](const juce::ArgumentList::Argument& argument) {
        return std::ranges::any_of(valueOptions, [&argument](const CliOption& option) {
            return option.takesValue() && argument == option.pattern() && !argument.text.containsChar('=');
        });
    };

    const auto first = subcommand == CliSubcommand::none ? 0 : 1;
    for (int index = first; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == cli::HELP_OPTION.pattern()) {
            return HelpOrVersionFlag {.index = index, .version = false};
        }
        if (subcommand == CliSubcommand::none && argument == cli::VERSION_OPTION.pattern()) {
            return HelpOrVersionFlag {.index = index, .version = true};
        }
        if (takesValue(argument) && index + 1 < arguments.size()) {
            ++index;
        }
    }
    return std::nullopt;
}

/// Returns true for an error that relates several options or needs every argument,
/// which a help or version flag later on the command line still overrides, as it does in clap.
bool isDeferredError(const CliParseError& error)
{
    if (error.deferred) {
        return true;
    }
    switch (error.kind) {
        case CliParseErrorKind::missingRequiredOption:
        case CliParseErrorKind::conflictingOptions:
        case CliParseErrorKind::missingArgument:
            return true;
        case CliParseErrorKind::unknownOption:
        case CliParseErrorKind::invalidValue:
        case CliParseErrorKind::valueOutOfRange:
        case CliParseErrorKind::unexpectedNumber:
        case CliParseErrorKind::unexpectedInput:
            return false;
    }
    return false;
}

/// Parses arguments already split at "--", with the subcommand found in their first argument.
std::expected<CliCommand, CliParseError> parseSeparatedArguments(
    juce::ArgumentList arguments,
    const juce::StringArray& literals,
    const CliSubcommand subcommand,
    const std::optional<juce::String>& deleteEnvironmentValue
)
{
    if (subcommand != CliSubcommand::none) {
        arguments.arguments.remove(0);
    }

    // Root environment values are validated even for subcommands, except the help command.
    if (subcommand == CliSubcommand::list || subcommand == CliSubcommand::completion) {
        if (const auto deleteOriginals = parseDeleteOriginals(false, deleteEnvironmentValue); !deleteOriginals) {
            return std::unexpected(deleteOriginals.error());
        }
    }

    switch (subcommand) {
        case CliSubcommand::process: {
            ProcessCommand processing;
            const auto preset = takeOptionValue(arguments, cli::PRESET_OPTION);
            if (!preset.has_value() || preset->trim().isEmpty()) {
                return parseError(
                    CliParseErrorKind::missingRequiredOption, "The process command needs --preset <preset>"
                );
            }
            processing.preset = *preset;
            return parseAnalyzeCommand(arguments, literals, deleteEnvironmentValue, processing);
        }
        case CliSubcommand::list:
            arguments.arguments.addArray(literalArguments(literals));
            return parseListPluginsCommand(arguments);
        case CliSubcommand::completion:
            arguments.arguments.addArray(literalArguments(literals));
            return parseCompletionCommand(arguments);
        case CliSubcommand::help:
            arguments.arguments.addArray(literalArguments(literals));
            return parseHelpCommand(arguments);
        case CliSubcommand::none:
            break;
    }

    return parseAnalyzeCommand(arguments, literals, deleteEnvironmentValue);
}
}  // namespace

/// Returns the usage line of an analysis run or of a subcommand, which an argument error prints with a hint.
juce::String cli::buildUsageLine(const juce::String& executableName, const CliSubcommand subcommand)
{
    if (const auto info = findSubcommandInfo(subcommand); info.has_value()) {
        return subcommandUsageLine(executableName, *info);
    }
    return utils::format("Usage: {} [options] <paths...>", executableName);
}

/// Builds the help text shown by the CLI executable for an analysis run, or for one of its subcommands.
juce::String cli::buildUsage(const juce::String& executableName, const CliSubcommand subcommand)
{
    juce::StringArray lines;

    if (const auto info = findSubcommandInfo(subcommand); info.has_value()) {
        lines = buildSubcommandUsage(executableName, *info);
    } else {
        lines.add(utils::format("{} {}", executableName, version::VERSION_INFO));
        lines.add(buildUsageLine(executableName, subcommand));

        for (const auto& command : SUBCOMMANDS) {
            lines.add(utils::format("       {} {} [options]", executableName, subcommandWithArgument(command)));
        }

        lines.add("");
        lines.add("Commands:");
        for (const auto& command : SUBCOMMANDS) {
            lines.add(usageRow(command.name, command.description));
        }

        lines.add("");
        lines.add("Options:");
        lines.add(usageRow(HELP_OPTION));
        lines.add(usageRow(VERSION_OPTION));
        for (const CliOption& option : ANALYSIS_OPTIONS) {
            lines.add(usageRow(option));
        }

        lines.add("");
        lines.add(utils::format("Run {} help <command> for the options of a command.", executableName));
    }

    // Lines are joined with juce::newLine outside the format strings,
    // so the text uses the platform line ending when printed.
    return lines.joinIntoString(juce::newLine) + juce::newLine;
}

/// Returns the subcommand named by the first argument, or none for an analysis run.
/// Only the first argument counts, so a subcommand name after an option is an input path,
/// and a file named like a subcommand can still be analyzed by passing it as "./list".
CliSubcommand cli::findSubcommand(const juce::ArgumentList& arguments)
{
    if (arguments.size() == 0) {
        return CliSubcommand::none;
    }

    for (const auto& command : SUBCOMMANDS) {
        if (arguments[0].text == command.name) {
            return command.subcommand;
        }
    }

    return CliSubcommand::none;
}

/// Joins the decoded arguments, without the executable name, into one space separated command line.
/// Used for the plugin scan worker command line,
/// so it carries the same decoded text as the arguments instead of decoding argv a second time.
juce::String cli::joinArguments(const juce::ArgumentList& arguments)
{
    juce::StringArray texts;

    for (const auto& argument : arguments.arguments) {
        texts.add(argument.text);
    }

    return texts.joinIntoString(" ");
}

/// Returns the arguments with every argument of combined short options expanded into separate arguments,
/// so a short option can be matched exactly, and the value in "-spath" is not taken for the -h flag.
juce::ArgumentList cli::expandShortOptions(const juce::ArgumentList& arguments)
{
    return expandCombinedShortOptions(arguments);
}

/// Parses command-line arguments into a validated analysis, plugin listing, completion, help, or version command,
/// or returns why the arguments were rejected, with the message to show the user.
std::expected<CliCommand, CliParseError> cli::parseArguments(const juce::ArgumentList& commandLine)
{
    return parseArguments(commandLine, readDeleteEnvironmentValue());
}

/// Parses arguments with an injected AUDIOBATCH_DELETE value, without reading or changing the process environment.
/// An absent value keeps the default trash disposal, and an explicit --delete overrides the injected value.
std::expected<CliCommand, CliParseError> cli::parseArguments(
    const juce::ArgumentList& commandLine,
    const std::optional<juce::String>& deleteEnvironmentValue
)
{
    const auto separated = separateArguments(commandLine);
    const auto subcommand = findSubcommand(separated.options);

    // Help and version are handled in argument order, the way clap handles them:
    // an error in an argument before the flag wins, while later arguments are not checked.
    // Rules that relate several options, such as --force needing --normalize, are checked only after the flags.
    if (const auto flag = findHelpOrVersionFlag(separated.options, subcommand); flag.has_value()) {
        auto prefix = separated.options;
        prefix.arguments.removeRange(flag->index, prefix.arguments.size() - flag->index);
        const auto parsedPrefix = parseSeparatedArguments(prefix, {}, subcommand, deleteEnvironmentValue);
        if (!parsedPrefix && !isDeferredError(parsedPrefix.error())) {
            return parsedPrefix;
        }
        if (flag->version) {
            return VersionCommand {};
        }
        return HelpCommand {.subcommand = subcommand};
    }

    return parseSeparatedArguments(separated.options, separated.literals, subcommand, deleteEnvironmentValue);
}
}  // namespace audiobatch
