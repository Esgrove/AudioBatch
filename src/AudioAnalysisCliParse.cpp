/// Implementation of the CLI argument handling: the usage text, the option parsing and validation,
/// and the workarounds for juce::ArgumentList matching of negative numbers and combined short options.

#include "AudioAnalysisCli.h"
#include "StringFormat.h"
#include "version.h"

#include <algorithm>
#include <map>

/// Helpers for parsing and validating the CLI options.
namespace audiobatch::cli
{
/// Lowest upper limit for the worker count, used on machines with few cores.
constexpr int minimumWorkerLimit = 64;
/// Longest worker count text that is parsed, which keeps the number far from the 64-bit limit.
constexpr int maximumWorkerCountDigits = 9;

/// Returns true when the argument is a plain number such as "-14" or "0.5",
/// which may start with a minus sign but is a value and not an option.
static bool isNumericArgument(const juce::String& text)
{
    const auto digits = text.startsWithChar('-') || text.startsWithChar('+') ? text.substring(1) : text;
    return digits.isNotEmpty() && digits.containsOnly("0123456789.") && digits.containsAnyOf("0123456789")
        && digits.indexOfChar('.') == digits.lastIndexOfChar('.');
}

/// Removes an option and its value from the arguments.
/// Accepts the value as the next argument for both the short and the long form, like "-g type" and "--group-by type",
/// and also as "--group-by=type".
/// juce::ArgumentList::removeValueForOption only takes the next argument for the short form.
/// Returns an empty optional when the option is not present, and an empty string when it is present without a value.
static std::optional<juce::String> removeOptionWithValue(juce::ArgumentList& arguments, const juce::StringRef option)
{
    for (int index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments.arguments.getReference(index);

        if (argument != option) {
            continue;
        }

        if (argument.isLongOption() && argument.text.containsChar('=')) {
            auto value = argument.getLongOptionValue();
            arguments.arguments.remove(index);
            return value;
        }

        if (index + 1 < arguments.size() && !arguments[index + 1].isOption()) {
            auto value = arguments[index + 1].text;
            arguments.arguments.removeRange(index, 2);
            return value;
        }

        arguments.arguments.remove(index);
        return juce::String();
    }

    return std::nullopt;
}

/// Returns an error message for the first argument that combines several short options, such as "-rf",
/// or that is a negative number no option took as its value, such as "-12", or an empty string when there is none.
/// juce::ArgumentList matches a short option by searching for its letter anywhere in the argument,
/// and removes the whole argument on the first match, so "-rf" would silently set only one of the flags.
/// It also converts the option letter to a number string first, so "-12" would match "-r" and several other flags.
static juce::String findShortOptionError(const juce::ArgumentList& arguments)
{
    for (const auto& argument : arguments.arguments) {
        if (!argument.isShortOption()) {
            continue;
        }

        if (isNumericArgument(argument.text)) {
            return utils::format(
                "Unexpected number {}. Pass a negative value right after the option that takes it.", argument.text
            );
        }

        if (argument.text.length() > 2) {
            return utils::format(
                "Combined short options are not supported: {}. Pass each option separately.", argument.text
            );
        }
    }

    return {};
}

/// Parses a sort mode name, accepting "loudness" as an alias for "lufs".
/// Returns an empty optional for an unknown name.
static std::optional<AudioAnalysisSortMode> parseSortMode(const juce::String& value)
{
    const auto normalizedSort = utils::normalizedToken(value);

    if (normalizedSort == "peak") {
        return AudioAnalysisSortMode::peak;
    }

    if (normalizedSort == "loudness" || normalizedSort == "lufs") {
        return AudioAnalysisSortMode::loudness;
    }

    if (normalizedSort == "name") {
        return AudioAnalysisSortMode::name;
    }

    if (normalizedSort == "path") {
        return AudioAnalysisSortMode::path;
    }

    return std::nullopt;
}

/// Parses an output format name.
/// Returns an empty optional for an unknown name.
static std::optional<AudioAnalysisOutputFormat> parseOutputFormat(const juce::String& value)
{
    const auto normalizedFormat = utils::normalizedToken(value);

    if (normalizedFormat == "table") {
        return AudioAnalysisOutputFormat::table;
    }

    if (normalizedFormat == "csv") {
        return AudioAnalysisOutputFormat::csv;
    }

    if (normalizedFormat == "json") {
        return AudioAnalysisOutputFormat::json;
    }

    return std::nullopt;
}

/// Parses a target level in the given range for --peak or --lufs into the normalization options.
/// Returns an error message, or an empty string when the value is valid.
static juce::String parseTargetLevel(
    const juce::String& option,
    const juce::String& value,
    const double minimum,
    const double maximum,
    double& target
)
{
    const auto trimmedValue = value.trim();

    if (!isNumericArgument(trimmedValue)) {
        return utils::format("{} needs a number, for example {} {}", option, option, minimum);
    }

    target = trimmedValue.getDoubleValue();

    if (target < minimum || target > maximum) {
        return utils::format("{} must be between {} and {}", option, minimum, maximum);
    }

    return {};
}

/// Removes --peak and --lufs from the arguments and sets the normalization mode and target.
/// Returns an error message, or an empty string when the options are valid or absent.
static juce::String parseNormalizationTarget(juce::ArgumentList& arguments, AudioAnalysisCliOptions& options)
{
    const auto peakValue = removeOptionWithValue(arguments, "--peak");
    const auto loudnessValue = removeOptionWithValue(arguments, "--lufs");

    if (peakValue.has_value() && loudnessValue.has_value()) {
        return "--peak and --lufs cannot be used together";
    }

    if (peakValue.has_value()) {
        options.hasNormalizationTarget = true;
        options.normalization.mode = AudioNormalizationMode::peak;
        return parseTargetLevel(
            "--peak",
            *peakValue,
            AudioNormalizationOptions::minimumTargetPeakDbfs,
            AudioNormalizationOptions::maximumTargetPeakDbfs,
            options.normalization.targetPeakDbfs
        );
    }

    if (loudnessValue.has_value()) {
        options.hasNormalizationTarget = true;
        options.normalization.mode = AudioNormalizationMode::loudness;
        return parseTargetLevel(
            "--lufs",
            *loudnessValue,
            AudioNormalizationOptions::minimumTargetLufs,
            AudioNormalizationOptions::maximumTargetLufs,
            options.normalization.targetLufs
        );
    }

    return {};
}

/// Returns an error message for options that cannot be used together, or an empty string when they can.
/// The plugin listing takes no input paths and no analysis options,
/// its grouping and rescan options need --list-plugins, and --force, --peak, and --lufs need --normalize.
static juce::String validateOptionCombinations(
    const AudioAnalysisCliOptions& options,
    const bool hasGrouping,
    const bool hasAnalysisValueOption
)
{
    if ((hasGrouping || options.rescanPlugins) && !options.listPlugins) {
        return "--group-by and --rescan can only be used with --list-plugins";
    }

    if (options.listPlugins && !options.inputPaths.isEmpty()) {
        return "--list-plugins does not take input paths";
    }

    const bool hasAnalysisOption = hasAnalysisValueOption || options.recursive || options.refresh || options.normalize
        || options.force || options.reverse || options.hasNormalizationTarget;

    if (options.listPlugins && hasAnalysisOption) {
        return "--list-plugins cannot be combined with analysis options";
    }

    if (options.force && !options.normalize) {
        return "--force can only be used with --normalize";
    }

    if (options.hasNormalizationTarget && !options.normalize) {
        return "--peak and --lufs can only be used with --normalize";
    }

    return {};
}
}  // namespace audiobatch::cli

using namespace audiobatch::cli;

/// Builds the help text shown by the CLI executable.
juce::String AudioAnalysisCli::buildUsage(const juce::String& executableName)
{
    // Lines are joined with juce::newLine outside the format strings,
    // so the text uses the platform line ending when printed.
    const juce::StringArray lines {
        utils::format("{} {}", executableName, version::VERSION_INFO),
        utils::format("Usage: {} [options] <paths...>", executableName),
        "",
        "Options:",
        "  -h, --help              Print usage and exit",
        "  -V, --version           Print version and exit",
        "  -r, --recurse           Recurse into directories",
        "  -f, --refresh           Ignore cached analysis and re-analyze files",
        "  -n, --normalize         Normalize analyzed files, then report the re-analyzed output peaks",
        "  -F, --force             Let --normalize replace an existing output file",
        "      --peak <dBFS>       Normalize to this sample peak, -12 to 0 (default 0)",
        "      --lufs <LUFS>       Normalize to this integrated loudness, -24 to -6, with a -0.3 dBTP ceiling",
        "  -j, --jobs <count>      Override worker count",
        "  -s, --sort <mode>       Sort by peak, lufs, name, or path",
        "      --reverse           Sort in descending order",
        "  -o, --output <format>   Print results as table (default), csv, or json",
        "  -l, --list-plugins      List the available audio plugins and exit",
        "  -g, --group-by <mode>   Group the plugin list by name (alphabetical), manufacturer, type, or format",
        "  -R, --rescan            Scan for new or changed plugins before listing them",
        "",
        "Short options cannot be combined, so pass -r -f instead of -rf.",
    };

    return lines.joinIntoString(juce::newLine) + juce::newLine;
}

/// Joins an option that takes a value and a following negative number into one "--option=value" argument,
/// for example "--lufs -14" into "--lufs=-14" and "-j -1" into "--jobs=-1".
/// Must run before any juce::ArgumentList option matching:
/// its short option check converts the option letter to a number string, so "-14" would match "-h" or "-n".
void AudioAnalysisCli::attachNegativeOptionValues(juce::ArgumentList& arguments)
{
    // Short options cannot carry a value after an equals sign, so the ones that take a value use their long form.
    const std::map<juce::String, juce::String> longFormOfShortValueOptions {
        {"-j", "--jobs"},
        {"-s", "--sort"},
        {"-o", "--output"},
        {"-g", "--group-by"},
    };

    for (int index = 0; index + 1 < arguments.size(); ++index) {
        const auto& argument = arguments.arguments.getReference(index);
        const auto& [text] = arguments.arguments.getReference(index + 1);

        if (!text.startsWithChar('-') || !isNumericArgument(text)) {
            continue;
        }

        auto optionText = argument.text;

        if (const auto longForm = longFormOfShortValueOptions.find(argument.text);
            longForm != longFormOfShortValueOptions.end())
        {
            optionText = longForm->second;
        } else if (!argument.isLongOption() || argument.text.containsChar('=')) {
            continue;
        }

        if (optionText != "--jobs" && optionText != "--peak" && optionText != "--lufs") {
            continue;
        }

        arguments.arguments.set(index, {utils::format("{}={}", optionText, text)});
        arguments.arguments.remove(index + 1);
    }
}

/// Returns true when an argument is exactly one of the given option names, such as "--help" or "-h".
/// Unlike juce::ArgumentList::containsOption, a number such as "-1" never matches a short option.
bool AudioAnalysisCli::containsExactOption(const juce::ArgumentList& arguments, const juce::StringArray& optionNames)
{
    return std::ranges::any_of(arguments.arguments, [&optionNames](const juce::ArgumentList::Argument& argument) {
        return optionNames.contains(argument.text);
    });
}

/// Parses command-line arguments into a validated options object.
/// The help and version flags are handled by the caller before parsing.
std::optional<AudioAnalysisCliOptions> AudioAnalysisCli::parse(juce::ArgumentList arguments, juce::String& errorMessage)
{
    AudioAnalysisCliOptions options;
    attachNegativeOptionValues(arguments);

    if (errorMessage = findShortOptionError(arguments); errorMessage.isNotEmpty()) {
        return std::nullopt;
    }

    options.recursive = arguments.removeOptionIfFound("--recurse|-r");
    options.refresh = arguments.removeOptionIfFound("--refresh|-f");
    options.normalize = arguments.removeOptionIfFound("--normalize|-n");
    options.force = arguments.removeOptionIfFound("--force|-F");
    options.reverse = arguments.removeOptionIfFound("--reverse");
    options.listPlugins = arguments.removeOptionIfFound("--list-plugins|-l");
    options.rescanPlugins = arguments.removeOptionIfFound("--rescan|-R");

    const auto workerCountValue = removeOptionWithValue(arguments, "--jobs|-j");
    if (workerCountValue.has_value()) {
        // The thread pool creates every worker up front, so the count is capped well above any useful value.
        const auto maximumWorkerCount = juce::jmax(minimumWorkerLimit, juce::SystemStats::getNumCpus() * 4);
        const auto workerCountText = workerCountValue->trim();

        if (workerCountText.isEmpty() || !workerCountText.containsOnly("0123456789")) {
            errorMessage = "Worker count must be a whole number greater than zero";
            return std::nullopt;
        }

        // Parsed as a 64-bit value, so a very long number cannot overflow into a small one.
        const auto workerCount = workerCountText.getLargeIntValue();

        if (workerCount <= 0) {
            errorMessage = "Worker count must be greater than zero";
            return std::nullopt;
        }

        if (workerCount > maximumWorkerCount || workerCountText.length() > maximumWorkerCountDigits) {
            errorMessage = utils::format("Worker count must be at most {}", maximumWorkerCount);
            return std::nullopt;
        }

        options.workerCount = static_cast<int>(workerCount);
    }

    const auto sortValue = removeOptionWithValue(arguments, "--sort|-s");
    if (sortValue.has_value()) {
        const auto sortMode = parseSortMode(*sortValue);

        if (!sortMode.has_value()) {
            errorMessage = "Sort mode must be one of: peak, lufs, name, path";
            return std::nullopt;
        }

        options.sortMode = *sortMode;
    }

    if (errorMessage = parseNormalizationTarget(arguments, options); errorMessage.isNotEmpty()) {
        return std::nullopt;
    }

    const auto outputValue = removeOptionWithValue(arguments, "--output|-o");
    if (outputValue.has_value()) {
        const auto outputFormat = parseOutputFormat(*outputValue);

        if (!outputFormat.has_value()) {
            errorMessage = "Output format must be one of: table, csv, json";
            return std::nullopt;
        }

        options.outputFormat = *outputFormat;
    }

    const auto groupingValue = removeOptionWithValue(arguments, "--group-by|-g");
    if (groupingValue.has_value()) {
        const auto grouping = PluginListCli::parseGrouping(*groupingValue);

        if (!grouping.has_value()) {
            errorMessage = "Plugin grouping must be one of: name, manufacturer, type, format";
            return std::nullopt;
        }

        options.pluginGrouping = *grouping;
    }

    for (const auto& argument : arguments.arguments) {
        if (argument.isOption()) {
            errorMessage = utils::format("Unknown option: {}", argument.text);
            return std::nullopt;
        }

        options.inputPaths.add(argument.resolveAsFile());
    }

    const bool hasAnalysisValueOption
        = workerCountValue.has_value() || sortValue.has_value() || outputValue.has_value();

    if (errorMessage = validateOptionCombinations(options, groupingValue.has_value(), hasAnalysisValueOption);
        errorMessage.isNotEmpty())
    {
        return std::nullopt;
    }

    return options;
}
