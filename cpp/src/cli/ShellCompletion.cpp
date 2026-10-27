/// Implementation of the shell completion scripts and the completion subcommand.
/// Each script is a fixed template for its shell, filled in with the options, value names,
/// and subcommands from the tables in CliOptions.h, so the completions offer exactly what the parser accepts.
/// A subcommand is only recognised as the first argument, the same as in the parser.
/// The install locations are the per-user directories audiobatch-rs uses too, so both CLIs install alike.

#include "cli/ShellCompletion.h"

#include "cli/CliOptions.h"
#include "utils/ConsolePrint.h"
#include "utils/StringFormat.h"
#include <fmt/ranges.h>
#include <string_view>

#include <cstdio>
#include <span>
#include <string>
#include <vector>

#if JUCE_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

namespace audiobatch
{
namespace
{
/// The options and the argument completions of one command, the analysis run or a subcommand.
struct CompletionScope {
    /// The subcommand name, or empty for the analysis run, which completes file paths as its arguments.
    std::string_view name;
    /// One line description of the subcommand.
    std::string_view description;
    /// The options of the command, starting with --help.
    std::vector<CliOption> options;
    /// Placeholder of the subcommand's argument, such as "shell", or empty when it takes none.
    std::string_view argumentName;
    /// Values offered for the subcommand's argument, such as the shell names.
    std::span<const char* const> argumentValues;
};

/// Returns the completion scopes of the analysis run and of every subcommand, in the order of the usage text.
std::vector<CompletionScope> completionScopes()
{
    std::vector<CompletionScope> scopes;

    CompletionScope analysis {
        .name = {},
        .description = {},
        .options = {cli::helpOption, cli::versionOption},
        .argumentName = {},
        .argumentValues = {},
    };
    for (const CliOption& option : cli::analysisOptions) {
        analysis.options.push_back(option);
    }
    scopes.push_back(std::move(analysis));

    for (const auto& subcommand : cli::subcommands) {
        CompletionScope scope {
            .name = subcommand.name,
            .description = subcommand.description,
            .options = {cli::helpOption},
            .argumentName = subcommand.argumentName,
            .argumentValues = subcommand.argumentValues,
        };
        for (const CliOption& option : subcommand.options) {
            scope.options.push_back(option);
        }
        scopes.push_back(std::move(scope));
    }

    return scopes;
}

/// Returns the text with every occurrence of the token replaced, for filling in the script templates.
std::string replaceAll(std::string text, const std::string_view token, const std::string_view value)
{
    for (auto position = text.find(token); position != std::string::npos;
         position = text.find(token, position + value.size()))
    {
        text.replace(position, token.size(), value);
    }

    return text;
}

/// Returns the text without its last line feed, for filling a template line with several generated lines.
std::string withoutTrailingNewline(std::string text)
{
    if (text.ends_with('\n')) {
        text.pop_back();
    }

    return text;
}

/// Returns the text with every character in the set preceded by the escape text,
/// such as a backslash before the brackets of a zsh option description.
std::string escapeCharacters(
    const std::string_view text,
    const std::string_view characters,
    const std::string_view escape
)
{
    std::string result;

    for (const char character : text) {
        if (characters.contains(character)) {
            result += escape;
        }
        result += character;
    }

    return result;
}

/// Returns the text for a single quoted string in a POSIX shell, with each quote closed, escaped, and reopened.
std::string escapeSingleQuotes(const std::string_view text)
{
    return replaceAll(std::string(text), "'", R"('\'')");
}

/// Returns the command name as a shell function name, with every character other than a letter or digit as "_".
std::string functionName(const juce::String& commandName)
{
    std::string name;

    for (const auto character : commandName.toStdString()) {
        const auto isWordCharacter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9');
        name += isWordCharacter ? character : '_';
    }

    return name;
}

/// Joins words with the separator, such as the values of an option for a completion list.
std::string joinWords(const std::span<const char* const> words, const std::string_view separator)
{
    std::string result;

    for (const auto* word : words) {
        if (!result.empty()) {
            result += separator;
        }
        result += word;
    }

    return result;
}

/// Returns the names an option is typed as, the short form first when it has one.
std::vector<std::string_view> optionNames(const CliOption& option)
{
    std::vector<std::string_view> names;

    if (option.hasShortName()) {
        names.emplace_back(option.shortName);
    }
    names.emplace_back(option.longName);

    return names;
}

/// Joins the names of the options with the separator, such as "-h --help -V --version".
std::string joinOptionNames(const std::vector<CliOption>& options, const std::string_view separator)
{
    std::string result;

    for (const auto& option : options) {
        for (const auto name : optionNames(option)) {
            if (!result.empty()) {
                result += separator;
            }
            result += name;
        }
    }

    return result;
}

/// Returns the subcommand names separated by spaces.
std::string subcommandNames()
{
    std::string result;

    for (const auto& subcommand : cli::subcommands) {
        if (!result.empty()) {
            result += ' ';
        }
        result += subcommand.name;
    }

    return result;
}

/// Returns the zsh _arguments spec of an option, such as "'(-s --sort)'{-s,--sort=}'[Sort by]:mode:(peak name)'".
std::string zshOptionSpec(const CliOption& option)
{
    const auto description = escapeSingleQuotes(escapeCharacters(option.description, "\\[]:", "\\"));
    std::string action;

    if (option.takesValue()) {
        action = option.values.empty() ? fmt::format(":{}: ", option.valueName)
                                       : fmt::format(":{}:({})", option.valueName, joinWords(option.values, " "));
    }

    const auto longForm = option.takesValue() ? fmt::format("{}=", option.longName) : std::string(option.longName);

    if (!option.hasShortName()) {
        return fmt::format("'{}[{}]{}'", longForm, description, action);
    }

    return fmt::format(
        "'({} {})'{{{},{}}}'[{}]{}'", option.shortName, option.longName, option.shortName, longForm, description, action
    );
}

/// Returns the zsh _arguments call that completes the options and the arguments of a scope.
std::string zshArguments(const CompletionScope& scope, const std::string_view indent)
{
    std::vector<std::string> specs;
    specs.reserve(scope.options.size() + 1);

    for (const auto& option : scope.options) {
        specs.push_back(zshOptionSpec(option));
    }

    if (scope.name.empty()) {
        specs.emplace_back("'*:path:_files'");
    } else if (!scope.argumentValues.empty()) {
        specs.push_back(fmt::format("'1:{}:({})'", scope.argumentName, joinWords(scope.argumentValues, " ")));
    }

    std::string result {"_arguments -S"};

    for (const auto& spec : specs) {
        result += fmt::format(" \\\n{}    {}", indent, spec);
    }

    return result;
}

/// Builds the zsh completion script, which works both from a directory in fpath and when sourced.
std::string buildZshScript(const std::string_view commandName, const std::string_view identifier)
{
    constexpr std::string_view scriptTemplate {R"(#compdef @COMMAND@

# Shell completion for @COMMAND@, generated by "@COMMAND@ completion zsh".

_@ID@() {
    local -a subcommands=(
@SUBCOMMANDS@
    )

    if (( CURRENT > 2 )); then
        case ${words[2]} in
@SUBCOMMAND_CASES@
        esac
    fi

    if (( CURRENT == 2 )) && [[ $PREFIX != -* ]]; then
        _describe -t commands command subcommands
    fi

    @ANALYSIS@
}

if [[ $zsh_eval_context[-1] == loadautofunc ]]; then
    _@ID@ "$@"
else
    compdef _@ID@ @COMMAND@
fi
)"};

    std::string descriptions;
    std::string cases;

    for (const auto& scope : completionScopes()) {
        if (scope.name.empty()) {
            continue;
        }

        descriptions += fmt::format("        '{}:{}'\n", scope.name, escapeSingleQuotes(scope.description));
        cases += fmt::format(
            "            ({})\n"
            "                {}\n"
            "                return\n"
            "                ;;\n",
            scope.name,
            zshArguments(scope, "                ")
        );
    }

    auto script = replaceAll(std::string(scriptTemplate), "@SUBCOMMANDS@", withoutTrailingNewline(descriptions));
    script = replaceAll(std::move(script), "@SUBCOMMAND_CASES@", withoutTrailingNewline(cases));
    script = replaceAll(std::move(script), "@ANALYSIS@", zshArguments(completionScopes().front(), "    "));
    script = replaceAll(std::move(script), "@ID@", identifier);
    return replaceAll(std::move(script), "@COMMAND@", commandName);
}

/// Returns the bash case branch that completes the options and the arguments of a scope.
std::string bashScopeCase(const CompletionScope& scope)
{
    std::string valueCases;
    std::vector<std::string_view> freeValueNames;

    for (const auto& option : scope.options) {
        if (!option.takesValue()) {
            continue;
        }

        const auto names = optionNames(option);

        if (option.values.empty()) {
            freeValueNames.insert(freeValueNames.end(), names.begin(), names.end());
            continue;
        }

        valueCases += fmt::format(
            "                {})\n"
            "                    _@ID@_add_words \"$current\" {}\n"
            "                    return\n"
            "                    ;;\n",
            fmt::join(names, " | "),
            joinWords(option.values, " ")
        );
    }

    if (!freeValueNames.empty()) {
        valueCases += fmt::format(
            "                {})\n"
            "                    return\n"
            "                    ;;\n",
            fmt::join(freeValueNames, " | ")
        );
    }

    std::string result = fmt::format("        {})\n", scope.name.empty() ? "*" : scope.name);

    if (!valueCases.empty()) {
        result += fmt::format("            case \"$previous\" in\n{}            esac\n", valueCases);
    }

    result += fmt::format(
        "            if [[ $current == -* ]]; then\n"
        "                _@ID@_add_words \"$current\" {}\n"
        "                return\n"
        "            fi\n",
        joinOptionNames(scope.options, " ")
    );

    if (scope.name.empty()) {
        result += fmt::format(
            "            if [[ $COMP_CWORD -eq 1 ]]; then\n"
            "                _@ID@_add_words \"$current\" {}\n"
            "            fi\n"
            "            _@ID@_add_files \"$current\"\n",
            subcommandNames()
        );
    } else if (!scope.argumentValues.empty()) {
        result += fmt::format("            _@ID@_add_words \"$current\" {}\n", joinWords(scope.argumentValues, " "));
    }

    result += "            ;;\n";
    return result;
}

/// Builds the bash completion script, which file names with spaces pass through whole.
std::string buildBashScript(const std::string_view commandName, const std::string_view identifier)
{
    constexpr std::string_view scriptTemplate {
        R"(# Shell completion for @COMMAND@, generated by "@COMMAND@ completion bash".

_@ID@_add_words() {
    local current="$1"
    shift
    local word
    for word in "$@"; do
        if [[ $word == "$current"* ]]; then
            COMPREPLY+=("$word")
        fi
    done
}

_@ID@_add_files() {
    local IFS=$'\n'
    # shellcheck disable=SC2207
    COMPREPLY+=($(compgen -f -- "$1"))
}

_@ID@() {
    COMPREPLY=()
    local current="${COMP_WORDS[COMP_CWORD]}"
    local previous="${COMP_WORDS[COMP_CWORD - 1]}"
    local command=""
    if [[ $COMP_CWORD -gt 1 ]]; then
        command="${COMP_WORDS[1]}"
    fi

    case "$command" in
@CASES@
    esac
}

complete -o filenames -F _@ID@ @COMMAND@
)"
    };

    std::string cases;
    const auto scopes = completionScopes();

    // The analysis run is the catch-all branch, so it goes after the subcommands.
    for (const auto& scope : std::span(scopes).subspan(1)) {
        cases += bashScopeCase(scope);
    }
    cases += bashScopeCase(scopes.front());

    auto script = replaceAll(std::string(scriptTemplate), "@CASES@", withoutTrailingNewline(cases));
    script = replaceAll(std::move(script), "@ID@", identifier);
    return replaceAll(std::move(script), "@COMMAND@", commandName);
}

/// Returns the text for a single quoted fish string.
std::string escapeFish(const std::string_view text)
{
    return escapeCharacters(text, "\\'", "\\");
}

/// Builds the fish completion script, with one complete command per option, subcommand, and argument value.
std::string buildFishScript(const std::string_view commandName, const std::string_view identifier)
{
    constexpr std::string_view scriptTemplate {
        R"(# Shell completion for @COMMAND@, generated by "@COMMAND@ completion fish".

function __@ID@_using_command --argument-names name
    set -l tokens (commandline -opc)
    set -l command ''
    if test (count $tokens) -ge 2; and contains -- $tokens[2] @SUBCOMMANDS@
        set command $tokens[2]
    end
    test "$command" = "$name"
end

function __@ID@_at_first_argument
    test (count (commandline -opc)) -eq 1
end

)"
    };

    std::string completions;

    for (const auto& scope : completionScopes()) {
        const auto condition = fmt::format("'__@ID@_using_command \"{}\"'", scope.name);

        if (!scope.name.empty()) {
            completions += fmt::format("complete -c @COMMAND@ -n {} -f\n", condition);
        }

        for (const auto& option : scope.options) {
            std::string line = fmt::format("complete -c @COMMAND@ -n {}", condition);

            if (option.hasShortName()) {
                line += fmt::format(" -s {}", std::string_view(option.shortName).substr(1));
            }
            line += fmt::format(" -l {}", std::string_view(option.longName).substr(2));

            if (option.takesValue()) {
                line += " -x";
                if (!option.values.empty()) {
                    line += fmt::format(" -a '{}'", joinWords(option.values, " "));
                }
            }

            completions += fmt::format("{} -d '{}'\n", line, escapeFish(option.description));
        }

        if (!scope.argumentValues.empty()) {
            completions += fmt::format(
                "complete -c @COMMAND@ -n {} -a '{}'\n", condition, joinWords(scope.argumentValues, " ")
            );
        }
    }

    for (const auto& subcommand : cli::subcommands) {
        completions += fmt::format(
            "complete -c @COMMAND@ -n __@ID@_at_first_argument -a {} -d '{}'\n",
            subcommand.name,
            escapeFish(subcommand.description)
        );
    }

    auto script = replaceAll(std::string(scriptTemplate), "@SUBCOMMANDS@", subcommandNames()) + completions;
    script = replaceAll(std::move(script), "@ID@", identifier);
    return replaceAll(std::move(script), "@COMMAND@", commandName);
}

/// Returns the user's home directory, from HOME when it is set outside Windows,
/// the same as the shells that load the scripts.
/// JUCE asks the system instead on macOS, which ignores HOME.
juce::File findHomeDirectory()
{
#if !JUCE_WINDOWS
    if (const auto home = juce::SystemStats::getEnvironmentVariable("HOME", {}); juce::File::isAbsolutePath(home)) {
        return {home};
    }
#endif
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory);
}

/// Returns the text for a single quoted PowerShell string, with each quote doubled.
std::string escapePowerShell(const std::string_view text)
{
    return replaceAll(std::string(text), "'", "''");
}

/// Returns a PowerShell array literal of quoted words, such as "@('peak', 'name')".
std::string powerShellArray(const std::span<const std::string_view> words)
{
    std::string result {"@("};

    for (const auto word : words) {
        if (result.size() > 2) {
            result += ", ";
        }
        result += fmt::format("'{}'", escapePowerShell(word));
    }

    return result + ")";
}

/// Returns a PowerShell array literal of the quoted values of an option or a subcommand argument.
std::string powerShellArray(const std::span<const char* const> values)
{
    const std::vector<std::string_view> words(values.begin(), values.end());
    return powerShellArray(words);
}

/// Builds the PowerShell completion script, a native argument completer driven by tables of the options.
std::string buildPowerShellScript(const std::string_view commandName)
{
    constexpr std::string_view scriptTemplate {
        R"(# Shell completion for @COMMAND@, generated by "@COMMAND@ completion powershell".

Register-ArgumentCompleter -Native -CommandName '@COMMAND@' -ScriptBlock {
    param($wordToComplete, $commandAst, $cursorPosition)

    $subcommands = [ordered]@{
@SUBCOMMANDS@
    }
    $options = @{
@OPTIONS@
    }
    $argumentValues = @{
@ARGUMENTS@
    }

    $previous = @($commandAst.CommandElements | Select-Object -Skip 1 |
        Where-Object { $_.Extent.EndOffset -lt $cursorPosition } |
        ForEach-Object { $_.ToString() })
    $command = ''
    if ($previous.Count -gt 0 -and $subcommands.Contains($previous[0])) {
        $command = $previous[0]
    }
    $last = if ($previous.Count -gt 0) { $previous[-1] } else { '' }
    $startsWord = { param($text) $text.StartsWith($wordToComplete, [System.StringComparison]::Ordinal) }

    $option = $options[$command] | Where-Object { $_.Names -ccontains $last } | Select-Object -First 1
    if ($option -and $option.TakesValue) {
        foreach ($value in $option.Values) {
            if (& $startsWord $value) {
                [System.Management.Automation.CompletionResult]::new($value, $value, 'ParameterValue', $value)
            }
        }
        return
    }

    if ($wordToComplete.StartsWith('-')) {
        foreach ($entry in $options[$command]) {
            foreach ($name in $entry.Names) {
                if (& $startsWord $name) {
                    [System.Management.Automation.CompletionResult]::new($name, $name, 'ParameterName', $entry.Description)
                }
            }
        }
        return
    }

    if ($previous.Count -eq 0) {
        foreach ($name in $subcommands.Keys) {
            if (& $startsWord $name) {
                [System.Management.Automation.CompletionResult]::new($name, $name, 'Command', $subcommands[$name])
            }
        }
    }

    if ($argumentValues.ContainsKey($command)) {
        foreach ($value in $argumentValues[$command]) {
            if (& $startsWord $value) {
                [System.Management.Automation.CompletionResult]::new($value, $value, 'ParameterValue', $value)
            }
        }
    } elseif ($command -eq '') {
        [System.Management.Automation.CompletionCompleters]::CompleteFilename($wordToComplete)
    }
}
)"
    };

    std::string subcommandEntries;
    std::string optionEntries;
    std::string argumentEntries;

    for (const auto& subcommand : cli::subcommands) {
        subcommandEntries
            += fmt::format("        '{}' = '{}'\n", subcommand.name, escapePowerShell(subcommand.description));
    }

    for (const auto& scope : completionScopes()) {
        optionEntries += fmt::format("        '{}' = @(\n", scope.name);

        for (const auto& option : scope.options) {
            optionEntries += fmt::format(
                "            @{{ Names = {}; Description = '{}'; TakesValue = ${}; Values = {} }}\n",
                powerShellArray(optionNames(option)),
                escapePowerShell(option.description),
                option.takesValue() ? "true" : "false",
                powerShellArray(option.values)
            );
        }

        optionEntries += "        )\n";

        if (!scope.argumentValues.empty()) {
            argumentEntries += fmt::format("        '{}' = {}\n", scope.name, powerShellArray(scope.argumentValues));
        }
    }

    auto script = replaceAll(std::string(scriptTemplate), "@SUBCOMMANDS@", withoutTrailingNewline(subcommandEntries));
    script = replaceAll(std::move(script), "@OPTIONS@", withoutTrailingNewline(optionEntries));
    script = replaceAll(std::move(script), "@ARGUMENTS@", withoutTrailingNewline(argumentEntries));
    return replaceAll(std::move(script), "@COMMAND@", commandName);
}
}  // namespace

/// Builds the completion script of the shell for the command name, with line feed line endings on every platform,
/// since bash cannot read a script with carriage returns.
juce::String cli::buildCompletionScript(const CompletionShell shell, const juce::String& commandName)
{
    const auto command = commandName.toStdString();
    const auto identifier = functionName(commandName);
    std::string script;

    switch (shell) {
        case CompletionShell::zsh:
            script = buildZshScript(command, identifier);
            break;
        case CompletionShell::bash:
            script = buildBashScript(command, identifier);
            break;
        case CompletionShell::fish:
            script = buildFishScript(command, identifier);
            break;
        case CompletionShell::powershell:
            script = buildPowerShellScript(command);
            break;
    }

    return juce::String::fromUTF8(script.c_str(), static_cast<int>(script.size()));
}

/// Returns the file the completion script of the shell is installed to, in a per-user directory under the home folder.
/// With oh-my-zsh installed, the zsh script goes into a custom plugin named after the command,
/// which is then enabled in the plugins list of .zshrc.
/// The system wide directories are not used, because writing them needs administrator rights.
juce::File cli::findCompletionFile(
    const CompletionShell shell,
    const juce::String& commandName,
    const juce::File& homeDirectory
)
{
    switch (shell) {
        case CompletionShell::zsh: {
            const auto ohMyZshPlugins
                = homeDirectory.getChildFile(".oh-my-zsh").getChildFile("custom").getChildFile("plugins");
            const auto directory = ohMyZshPlugins.isDirectory()
                ? ohMyZshPlugins.getChildFile(commandName)
                : homeDirectory.getChildFile(".zsh").getChildFile("completions");
            return directory.getChildFile(utils::format("_{}", commandName));
        }
        case CompletionShell::bash:
            return homeDirectory.getChildFile(".bash_completion.d").getChildFile(utils::format("{}.bash", commandName));
        case CompletionShell::fish:
            return homeDirectory.getChildFile(".config")
                .getChildFile("fish")
                .getChildFile("completions")
                .getChildFile(utils::format("{}.fish", commandName));
        case CompletionShell::powershell:
            break;
    }

#if JUCE_WINDOWS
    const auto powerShellDirectory = homeDirectory.getChildFile("Documents").getChildFile("PowerShell");
#else
    const auto powerShellDirectory = homeDirectory.getChildFile(".config").getChildFile("powershell");
#endif
    return powerShellDirectory.getChildFile("completions").getChildFile(utils::format("_{}.ps1", commandName));
}

/// Runs the completion subcommand and returns the process exit code.
/// Prints the script to stdout, or writes it into the shell's completion directory with --install,
/// creating the directory when missing.
int cli::runCompletion(const CompletionCommand& command, const juce::String& commandName)
{
    const auto script = buildCompletionScript(command.shell, commandName);

    if (!command.install) {
#if JUCE_WINDOWS
        // Text mode would turn the line feeds into carriage return and line feed pairs, which bash cannot read.
        std::fflush(stdout);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        fmt::print("{}", script);
        return 0;
    }

    const auto file = findCompletionFile(command.shell, commandName, findHomeDirectory());
    const auto directory = file.getParentDirectory();

    if (const auto result = directory.createDirectory(); result.failed()) {
        console::printError(
            utils::format(
                "Failed to create the completion directory {}: {}",
                directory.getFullPathName().quoted(),
                result.getErrorMessage()
            )
        );
        return 1;
    }

    if (!file.replaceWithText(script, false, false, "\n")) {
        console::printError(
            utils::format("Failed to write the completion script to {}", file.getFullPathName().quoted())
        );
        return 1;
    }

    if (command.verbose) {
        fmt::print("Completion file generated to: {}\n", file.getFullPathName());
    }

    return 0;
}
}  // namespace audiobatch
