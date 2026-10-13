/// Implementation of the CLI plugin listing.
/// Covers the optional rescan of the plugin folders through the out-of-process scanner,
/// with crash and failure reporting and removal of plugins that no longer exist,
/// and the aligned console output for the alphabetical table and the grouped listings.

#include "PluginListCli.h"

#include "AppInfo.h"
#include "ConsolePrint.h"
#include "Log.h"
#include "PluginScanner.h"
#include "StringFormat.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace audiobatch
{
namespace
{
constexpr auto unknownLabel = "Unknown";

/// One column of the plugin listing.
enum class Column : std::uint8_t { name, manufacturer, type, format, version };

/// Returns the short display name of a plugin format, for example AU instead of AudioUnit.
static juce::String formatLabel(const juce::String& formatName)
{
    return formatName == "AudioUnit" ? juce::String("AU") : formatName;
}

/// Returns the processing type of a plugin, such as Dynamics, EQ, Reverb, or Instrument.
/// VST3 categories are lists like "Fx|Dynamics", where the generic Fx
/// and Instrument entries are skipped in favour of the first specific one.
/// Audio Unit categories, such as Effect or Synth, are used as they are.
static juce::String typeLabel(const juce::PluginDescription& description)
{
    auto tokens = juce::StringArray::fromTokens(description.category, "|", "");
    tokens.trim();
    tokens.removeEmptyStrings();

    for (const auto& token : tokens) {
        if (!token.equalsIgnoreCase("Fx") && !token.equalsIgnoreCase("Instrument")) {
            return token;
        }
    }

    return description.isInstrument ? "Instrument" : "Effect";
}

/// Returns the text shown for a plugin in one column.
static juce::String columnValue(const juce::PluginDescription& description, const Column column)
{
    switch (column) {
        case Column::name:
            return description.name;
        case Column::manufacturer:
            return description.manufacturerName.isNotEmpty() ? description.manufacturerName
                                                             : juce::String(unknownLabel);
        case Column::type:
            return typeLabel(description);
        case Column::format:
            return formatLabel(description.pluginFormatName);
        case Column::version:
            return description.version;
    }

    return {};
}

/// Returns the header text of a column.
static juce::String columnHeader(const Column column)
{
    switch (column) {
        case Column::name:
            return "NAME";
        case Column::manufacturer:
            return "MANUFACTURER";
        case Column::type:
            return "TYPE";
        case Column::format:
            return "FORMAT";
        case Column::version:
            return "VERSION";
    }

    return {};
}

/// Returns the group a plugin belongs to for a grouped listing.
static juce::String groupLabel(const juce::PluginDescription& description, const PluginListGrouping grouping)
{
    switch (grouping) {
        case PluginListGrouping::manufacturer:
            return columnValue(description, Column::manufacturer);
        case PluginListGrouping::type:
            return typeLabel(description);
        case PluginListGrouping::format:
            return formatLabel(description.pluginFormatName);
        case PluginListGrouping::name:
            break;
    }

    return {};
}

/// Returns the column that a grouping already shows in its group headers, so the rows can leave it out.
static std::optional<Column> groupedColumn(const PluginListGrouping grouping)
{
    switch (grouping) {
        case PluginListGrouping::manufacturer:
            return Column::manufacturer;
        case PluginListGrouping::type:
            return Column::type;
        case PluginListGrouping::format:
            return Column::format;
        case PluginListGrouping::name:
            break;
    }

    return std::nullopt;
}

/// Sorts plugins by name, then manufacturer, then format, using natural case-insensitive order.
static void sortPlugins(std::vector<juce::PluginDescription>& plugins)
{
    std::ranges::sort(plugins, [](const juce::PluginDescription& lhs, const juce::PluginDescription& rhs) {
        if (const auto byName = lhs.name.compareNatural(rhs.name); byName != 0) {
            return byName < 0;
        }

        if (const auto byManufacturer = lhs.manufacturerName.compareNatural(rhs.manufacturerName); byManufacturer != 0)
        {
            return byManufacturer < 0;
        }

        return lhs.pluginFormatName.compareNatural(rhs.pluginFormatName) < 0;
    });
}

/// Computes the display width of each column from its header and every value, so rows line up across all groups.
static std::vector<int> columnWidths(
    const std::vector<juce::PluginDescription>& plugins,
    const std::vector<Column>& columns,
    const bool includeHeaders
)
{
    std::vector<int> widths;
    widths.reserve(columns.size());

    for (const auto column : columns) {
        int width = includeHeaders ? console::displayWidth(columnHeader(column)) : 0;

        for (const auto& plugin : plugins) {
            width = std::max(width, console::displayWidth(columnValue(plugin, column)));
        }

        widths.push_back(width);
    }

    return widths;
}

/// Formats one listing line from the given cell texts.
/// Every column except the last is padded to its width, so lines do not end in spaces.
static std::string formatLine(
    const std::string_view indent,
    const std::vector<juce::String>& cells,
    const std::vector<int>& widths
)
{
    std::string line(indent);

    for (std::size_t index = 0; index < cells.size(); ++index) {
        if (index + 1 < cells.size()) {
            line += fmt::format("{:<{}}  ", cells[index], widths[index]);
        } else {
            line += fmt::format("{}", cells[index]);
        }
    }

    return line;
}

/// Prints the plugins as aligned rows with the given columns.
static void printRows(
    const std::vector<juce::PluginDescription>& plugins,
    const std::vector<Column>& columns,
    const std::vector<int>& widths,
    const std::string_view indent
)
{
    for (const auto& plugin : plugins) {
        std::vector<juce::String> cells;
        cells.reserve(columns.size());

        for (const auto column : columns) {
            cells.push_back(columnValue(plugin, column));
        }

        fmt::print("{}\n", formatLine(indent, cells, widths));
    }
}

/// Prints all plugins as one alphabetical table with a header line.
static void printTable(const std::vector<juce::PluginDescription>& plugins)
{
    const std::vector columns {Column::name, Column::manufacturer, Column::type, Column::format, Column::version};
    const auto widths = columnWidths(plugins, columns, true);

    std::vector<juce::String> headers;
    headers.reserve(columns.size());
    for (const auto column : columns) {
        headers.push_back(columnHeader(column));
    }

    console::printBold("{}\n", formatLine("", headers, widths));
    printRows(plugins, columns, widths, "");
}

namespace
{
/// The plugins of one group, with the label as first seen.
struct Group {
    juce::String label;
    std::vector<juce::PluginDescription> plugins;
};
}  // namespace

/// Prints the plugins in sections, one per group, sorted by group name with Unknown last.
/// Groups are matched case-insensitively, so differently capitalized manufacturer names end up together.
static void printGroups(const std::vector<juce::PluginDescription>& plugins, const PluginListGrouping grouping)
{
    std::map<juce::String, Group> groupsByKey;
    for (const auto& plugin : plugins) {
        const auto label = groupLabel(plugin, grouping);
        auto& group = groupsByKey[label.toLowerCase()];

        if (group.label.isEmpty()) {
            group.label = label;
        }

        group.plugins.push_back(plugin);
    }

    std::vector<Group> groups;
    groups.reserve(groupsByKey.size());
    for (auto& [key, group] : groupsByKey) {
        groups.push_back(std::move(group));
    }

    std::ranges::sort(groups, [](const Group& lhs, const Group& rhs) {
        const auto lhsUnknown = lhs.label.equalsIgnoreCase(unknownLabel);
        const auto rhsUnknown = rhs.label.equalsIgnoreCase(unknownLabel);

        if (lhsUnknown != rhsUnknown) {
            return rhsUnknown;
        }

        return lhs.label.compareNatural(rhs.label) < 0;
    });

    std::vector<Column> columns;
    for (const auto column : {Column::name, Column::manufacturer, Column::type, Column::format, Column::version}) {
        if (column != groupedColumn(grouping)) {
            columns.push_back(column);
        }
    }

    // Use the same column widths in every group, so the whole listing lines up.
    const auto widths = columnWidths(plugins, columns, false);

    for (std::size_t index = 0; index < groups.size(); ++index) {
        const auto& group = groups[index];

        if (index > 0) {
            fmt::print("\n");
        }

        console::printColorBold(fmt::terminal_color::cyan, "{}", group.label);
        fmt::print(" ({})\n", group.plugins.size());
        printRows(group.plugins, columns, widths, "  ");
    }
}

/// Returns the registered format with the given name, or nullptr.
static juce::AudioPluginFormat* findFormat(
    const juce::AudioPluginFormatManager& formatManager,
    const juce::String& name
)
{
    for (auto* format : formatManager.getFormats()) {
        if (format->getName() == name) {
            return format;
        }
    }

    return nullptr;
}

/// Scans the plugin folders for new or changed plugins, one plugin at a time in a worker process,
/// and removes plugins whose files no longer exist.
/// Each format uses the same search folders as the last GUI scan, or its default folders.
/// Progress goes to stderr, and only to an interactive terminal.
static void rescanPlugins(
    juce::PropertiesFile& settings,
    juce::KnownPluginList& knownPluginList,
    const juce::AudioPluginFormatManager& formatManager
)
{
    knownPluginList.setCustomScanner(std::make_unique<OutOfProcessPluginScanner>());

    const auto blacklistedBefore = knownPluginList.getBlacklistedFiles();
    const auto showProgress = console::isColorEnabled(stderr);
    juce::StringArray failedFiles;

    for (auto* format : formatManager.getFormats()) {
        const auto searchPath = juce::PluginListComponent::getLastSearchPath(settings, *format);
        console::printStyled(
            stderr,
            fmt::emphasis::bold,
            "Scanning {} plugins in {}\n",
            formatLabel(format->getName()),
            searchPath.toString()
        );

        // Plugins that are already listed and unchanged are not loaded again.
        constexpr bool recursive = true;
        constexpr bool allowAsynchronousInstantiation = false;
        juce::PluginDirectoryScanner scanner(
            knownPluginList, *format, searchPath, recursive, getPluginScanCrashLogFile(), allowAsynchronousInstantiation
        );

        juce::String pluginName;
        for (bool morePlugins = true; morePlugins;) {
            morePlugins = scanner.scanNextFile(true, pluginName);

            if (showProgress) {
                // Rewrite one progress line in place.
                fmt::print(stderr, "\r\033[K{:3.0f}%  {}", scanner.getProgress() * 100.0f, pluginName);
            }
        }

        if (showProgress) {
            fmt::print(stderr, "\r\033[K");
        }

        failedFiles.addArray(scanner.getFailedFiles());
    }

    knownPluginList.scanFinished();

    int removedCount = 0;
    for (const auto& type : knownPluginList.getTypes()) {
        if (auto* format = findFormat(formatManager, type.pluginFormatName);
            format != nullptr && !format->doesPluginStillExist(type))
        {
            knownPluginList.removeType(type);
            ++removedCount;
        }
    }

    if (removedCount > 0) {
        utils::logInfo("Removed {} plugins that no longer exist", removedCount);
    }

    for (const auto& file : knownPluginList.getBlacklistedFiles()) {
        if (!blacklistedBefore.contains(file)) {
            console::printWarn(utils::format("Plugin crashed during the scan and was blacklisted: {}", file));
        }
    }

    for (const auto& file : failedFiles) {
        console::printWarn(utils::format("No plugins found in {}", file));
    }
}
}  // namespace

/// Parses a --group-by value. Returns an empty optional for an unknown value.
std::optional<PluginListGrouping> PluginListCli::parseGrouping(const juce::String& value)
{
    const auto normalized = utils::normalizedToken(value);

    if (normalized == "name") {
        return PluginListGrouping::name;
    }

    if (normalized == "manufacturer") {
        return PluginListGrouping::manufacturer;
    }

    if (normalized == "type") {
        return PluginListGrouping::type;
    }

    if (normalized == "format") {
        return PluginListGrouping::format;
    }

    return std::nullopt;
}

/// Prints the known plugins, grouped as requested, and returns the process exit code.
/// With rescan set, scans the plugin folders for new or changed plugins first
/// and saves the updated list for the GUI too.
/// Each plugin is loaded in a worker process launched from this executable,
/// so a plugin that crashes is blacklisted instead of ending the scan.
int PluginListCli::run(const bool rescan, const PluginListGrouping grouping)
{
    // Plugin hosting needs a message thread, which this initialiser sets up on the calling thread.
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::ApplicationProperties properties;
    properties.setStorageParameters(utils::createApplicationSettingsOptions());

    auto* settings = properties.getUserSettings();
    if (settings == nullptr) {
        console::printError("Could not open the AudioBatch settings file");
        return 1;
    }

    // Hold the plugin scan lock from loading the list to saving it,
    // so a scan in the GUI cannot overwrite this rescan's results, or the other way around.
    PluginScanLock pluginScanLock;
    if (rescan && !pluginScanLock.tryAcquire()) {
        console::printError(
            "Another plugin scan is running, for example in the AudioBatch plugin manager. "
            "Try again once it has finished."
        );
        return 1;
    }

    juce::KnownPluginList knownPluginList;
    loadKnownPluginList(*settings, knownPluginList);

    // The settings file is only read here, never saved, so a running GUI keeps its own settings.
    // The rescan result goes to its own KnownPlugins.xml file instead.
    int exitCode = 0;
    if (rescan) {
        juce::AudioPluginFormatManager formatManager;
        juce::addDefaultFormatsToManager(formatManager);
        rescanPlugins(*settings, knownPluginList, formatManager);

        if (!saveKnownPluginList(knownPluginList)) {
            console::printError(
                utils::format("Could not save the plugin list to {}", getKnownPluginListFile().getFullPathName())
            );
            exitCode = 1;
        }
    }

    const auto types = knownPluginList.getTypes();
    if (types.isEmpty()) {
        console::printWarn("No plugins found. Run with --rescan, or scan for plugins in the GUI.");
        return exitCode;
    }

    std::vector plugins(types.begin(), types.end());
    sortPlugins(plugins);

    if (grouping == PluginListGrouping::name) {
        printTable(plugins);
    } else {
        printGroups(plugins, grouping);
    }

    console::printStyled(stderr, fmt::emphasis::faint, "{} plugins\n", plugins.size());
    return exitCode;
}
}  // namespace audiobatch
