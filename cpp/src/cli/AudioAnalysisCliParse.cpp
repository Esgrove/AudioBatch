/// Implementation of the CLI argument handling: the usage texts,
/// and the parsing of the arguments into a validated AnalyzeCommand,
/// or the ListPluginsCommand or CompletionCommand of the subcommand named by the first argument.
/// The options, their usage lines, and the names each enum option accepts come from the tables in CliOptions.h.

#include "cli/AudioAnalysisCli.h"
#include "cli/CliOptions.h"
#include "utils/StringFormat.h"
#include "utils/version.h"

#include <algorithm>
#include <array>
#include <expected>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace audiobatch
{
namespace
{
/// Lowest upper limit for the worker count, used on machines with few cores.
constexpr int minimumWorkerLimit {64};
/// Longest worker count text that is parsed, which keeps the number far from the 64-bit limit.
constexpr int maximumWorkerCountDigits {9};

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

/// Returns the value whose name matches the text, ignoring case and surrounding space,
/// or nothing for an unknown name.
template<typename Enum, std::size_t count>
    requires std::is_enum_v<Enum>
std::optional<Enum> parseName(const std::array<OptionName<Enum>, count>& names, const juce::String& text)
{
    const auto token = utils::normalizedToken(text);
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

/// Returns the option whose short form is the given letter, among the options of every command.
const CliOption* findShortOption(const juce::juce_wchar letter)
{
    const auto matches = [letter](const CliOption& option) {
        return option.hasShortName() && juce::String(option.shortName).getLastCharacter() == letter;
    };

    for (const CliOption& option : {std::cref(cli::helpOption), std::cref(cli::versionOption)}) {
        if (matches(option)) {
            return &option;
        }
    }

    for (const auto options :
         {CliOptionList(cli::analysisOptions), CliOptionList(cli::listOptions), CliOptionList(cli::completionOptions)})
    {
        for (const CliOption& option : options) {
            if (matches(option)) {
                return &option;
            }
        }
    }

    return nullptr;
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
        const auto* option = findShortOption(text[index]);
        if (option == nullptr) {
            split.add(utils::format("-{}", text.substring(index, index + 1)));
            continue;
        }

        split.add(option->shortName);

        if (option->takesValue()) {
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

/// Parses the worker count of --jobs.
/// Returns the count, or an error when the value is not a whole number in the supported range.
std::expected<int, CliParseError> parseWorkerCount(const juce::String& value)
{
    // The thread pool creates every worker up front, so the count is capped well above any useful value.
    const auto maximumWorkerCount = juce::jmax(minimumWorkerLimit, juce::SystemStats::getNumCpus() * 4);
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
    if (workerCountText.trimCharactersAtStart("0").length() > maximumWorkerCountDigits) {
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
            AudioNormalizationOptions::minimumTargetPeakDbfs,
            AudioNormalizationOptions::maximumTargetPeakDbfs
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
            AudioNormalizationOptions::minimumTargetLufs,
            AudioNormalizationOptions::maximumTargetLufs
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
    command.rescan = arguments.removeOptionIfFound(cli::rescanOption.pattern());

    const auto grouping = takeNamedOption(arguments, cli::groupByOption, "Plugin grouping", cli::pluginGroupingNames);
    if (!grouping) {
        return std::unexpected(grouping.error());
    }
    command.grouping = grouping->value_or(command.grouping);

    if (std::ranges::any_of(
            cli::analysisOptions,
            [&arguments](const CliOption& option) { return arguments.containsOption(option.pattern()); }
        ))
    {
        return parseError(CliParseErrorKind::conflictingOptions, "The list command does not take analysis options");
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
    command.install = arguments.removeOptionIfFound(cli::installOption.pattern());
    command.verbose = arguments.removeOptionIfFound(cli::verboseOption.pattern());

    if (auto remaining = rejectRemainingOptions(arguments); !remaining) {
        return std::unexpected(std::move(remaining).error());
    }

    const auto shellNames = joinNames(cli::completionShellNames);

    if (arguments.size() == 0) {
        return parseError(
            CliParseErrorKind::missingArgument, utils::format("The completion command needs a shell: {}", shellNames)
        );
    }

    if (arguments.size() > 1) {
        return parseError(CliParseErrorKind::unexpectedInput, "The completion command takes only one shell");
    }

    const auto shell = parseName(cli::completionShellNames, arguments[0].text);
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
        for (const auto* name : cli::helpCommandValues) {
            names.add(name);
        }
        return parseError(
            CliParseErrorKind::invalidValue,
            utils::format("Unknown command: {}, expected one of: {}", arguments[0].text, names.joinIntoString(", "))
        );
    }

    return HelpCommand {.subcommand = subcommand};
}

/// Parses the arguments into an analysis run, with the remaining arguments as its input paths.
/// The plugin listing options are rejected, and --force, --peak, and --lufs need --normalize.
std::expected<CliCommand, CliParseError> parseAnalyzeCommand(juce::ArgumentList& arguments)
{
    if (arguments.containsOption(cli::groupByOption.pattern()) || arguments.containsOption(cli::rescanOption.pattern()))
    {
        return parseError(
            CliParseErrorKind::missingRequiredOption, "--group-by and --rescan can only be used with the list command"
        );
    }

    AnalyzeCommand command;
    command.recursive = arguments.removeOptionIfFound(cli::recurseOption.pattern());
    command.refresh = arguments.removeOptionIfFound(cli::refreshOption.pattern());
    command.reverse = arguments.removeOptionIfFound(cli::reverseOption.pattern());
    const bool normalize = arguments.removeOptionIfFound(cli::normalizeOption.pattern());
    const bool force = arguments.removeOptionIfFound(cli::forceOption.pattern());

    if (const auto workerCountValue = takeOptionValue(arguments, cli::jobsOption); workerCountValue.has_value()) {
        const auto workerCount = parseWorkerCount(*workerCountValue);

        if (!workerCount) {
            return std::unexpected(workerCount.error());
        }

        command.workerCount = *workerCount;
    }

    const auto sortMode = takeNamedOption(arguments, cli::sortOption, "Sort mode", cli::sortModeNames);
    if (!sortMode) {
        return std::unexpected(sortMode.error());
    }
    command.sortMode = sortMode->value_or(command.sortMode);

    const auto outputFormat = takeNamedOption(arguments, cli::outputOption, "Output format", cli::outputFormatNames);
    if (!outputFormat) {
        return std::unexpected(outputFormat.error());
    }
    command.outputFormat = outputFormat->value_or(command.outputFormat);

    const auto peakValue = takeOptionValue(arguments, cli::peakOption);
    const auto loudnessValue = takeOptionValue(arguments, cli::lufsOption);

    if (force && !normalize) {
        return parseError(CliParseErrorKind::missingRequiredOption, "--force can only be used with --normalize");
    }

    if ((peakValue.has_value() || loudnessValue.has_value()) && !normalize) {
        return parseError(
            CliParseErrorKind::missingRequiredOption, "--peak and --lufs can only be used with --normalize"
        );
    }

    if (normalize) {
        auto normalization = parseNormalization(peakValue, loudnessValue);

        if (!normalization) {
            return std::unexpected(normalization.error());
        }

        normalization->overwriteExisting = force;
        command.normalization = std::move(*normalization);
    }

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

        command.inputPaths.add(argument.resolveAsFile());
    }

    return command;
}

/// Returns the subcommand table entry for a subcommand, or nothing for an analysis run.
std::optional<CliSubcommandInfo> findSubcommandInfo(const CliSubcommand subcommand)
{
    for (const auto& info : cli::subcommands) {
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
            fmt::format("{}: {}", subcommand.argumentDescription, values.joinIntoString(", "))
        ));
    }

    lines.add("");
    lines.add("Options:");
    lines.add(usageRow(cli::helpOption));

    for (const CliOption& option : subcommand.options) {
        lines.add(usageRow(option));
    }

    return lines;
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

        for (const auto& command : subcommands) {
            lines.add(utils::format("       {} {} [options]", executableName, subcommandWithArgument(command)));
        }

        lines.add("");
        lines.add("Commands:");
        for (const auto& command : subcommands) {
            lines.add(usageRow(command.name, command.description));
        }

        lines.add("");
        lines.add("Options:");
        lines.add(usageRow(helpOption));
        lines.add(usageRow(versionOption));
        for (const CliOption& option : analysisOptions) {
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

    for (const auto& command : subcommands) {
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

/// Parses command-line arguments into a validated analysis, plugin listing, completion, or help command,
/// or returns why the arguments were rejected, with the message to show the user.
/// The help and version flags are handled by the caller before parsing.
std::expected<CliCommand, CliParseError> cli::parseArguments(const juce::ArgumentList& commandLine)
{
    auto arguments = expandCombinedShortOptions(commandLine);
    const auto subcommand = findSubcommand(arguments);

    if (subcommand != CliSubcommand::none) {
        arguments.arguments.remove(0);
    }

    switch (subcommand) {
        case CliSubcommand::list:
            return parseListPluginsCommand(arguments);
        case CliSubcommand::completion:
            return parseCompletionCommand(arguments);
        case CliSubcommand::help:
            return parseHelpCommand(arguments);
        case CliSubcommand::none:
            break;
    }

    return parseAnalyzeCommand(arguments);
}
}  // namespace audiobatch
