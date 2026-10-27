/// Shell completion scripts for the CLI executable.
/// Declares the cli functions
/// that generate the zsh, bash, fish, and PowerShell completion scripts from the option tables in CliOptions.h,
/// find the per-user directory each shell loads them from,
/// and run the completion subcommand, which prints a script or installs it.

#pragma once

#include "cli/AudioAnalysisCli.h"

namespace audiobatch::cli
{
[[nodiscard]] juce::String buildCompletionScript(CompletionShell shell, const juce::String& commandName);

[[nodiscard]] juce::File findCompletionFile(
    CompletionShell shell,
    const juce::String& commandName,
    const juce::File& homeDirectory
);

[[nodiscard]] int runCompletion(const CompletionCommand& command, const juce::String& commandName);
}  // namespace audiobatch::cli
