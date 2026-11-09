/// Declares ImageControl's plugin-owned, versioned sound preset library.
/// Operations are synchronous explicit filesystem work for a caller-owned worker,
/// never an audio or host-state callback. Construction does not touch disk.
/// The immutable APVTS parameter layout must outlive the library and its operations.

#pragma once

#include "ProcessingState.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <expected>
#include <vector>

namespace esgrove::image_control
{
/// A detached library selection and sound snapshot.
/// Keep the complete value from list/load/save for optimistic conflict checks, not just its filename.
/// The opaque fingerprint identifies the exact bytes read, including a revision changed on every mutation.
struct UserPreset {
    /// Absolute library file path, used only by explicit preset operations.
    juce::File file;
    /// Opaque exact-byte revision token for stale-selection detection.
    juce::String fingerprint;
    /// Stable preset UUID, retained when updating or renaming the preset.
    juce::String id;
    /// Validated portable display name, independent of the immutable UUID filename.
    juce::String name;
    /// Detached sound parameters, without editor state or audition controls.
    ProcessingSnapshot snapshot;
};

/// A file rejected while listing, retained so the caller can report malformed or unsupported presets.
struct UserPresetIssue {
    /// Rejected library entry that the caller may identify in a diagnostic.
    juce::File file;
    /// Human-readable validation or read failure, without modifying the rejected file.
    juce::String message;
};

/// A listing includes only validated presets, with separate diagnostics for rejected XML files.
struct UserPresetListing {
    /// Validated selections sorted case-insensitively by display name.
    std::vector<UserPreset> presets;
    /// Invalid or ambiguous XML entries omitted from the selectable presets.
    std::vector<UserPresetIssue> rejectedFiles;
};

/// Standalone sound-preset storage, independent of host state, editor settings, A/B slots, and undo history.
/// Library files use immutable UUID basenames, so a rename atomically replaces metadata without moving the file.
/// Mutations serialize across library instances and processes, then recheck selections before committing.
/// These locks coordinate ImageControl writers. Uncooperative external writers can still race filesystem operations.
/// All operations may block, allocate, and fail. The UI owns scheduling, lifetime, and destructive-action confirmation.
class UserPresets final
{
public:
    explicit UserPresets(const juce::AudioProcessorValueTreeState& parameterState);
    UserPresets(const juce::AudioProcessorValueTreeState& parameterState, juce::File presetDirectory);

    ~UserPresets();

    UserPresets(const UserPresets&) = delete;
    UserPresets& operator=(const UserPresets&) = delete;
    UserPresets(UserPresets&&) = delete;
    UserPresets& operator=(UserPresets&&) = delete;

    static juce::File defaultDirectory();
    static juce::Result validateName(const juce::String& name);

    [[nodiscard]] juce::File getDirectory() const;
    [[nodiscard]] juce::Result exportPreset(const UserPreset& selection, const juce::File& destination) const;
    [[nodiscard]] juce::Result remove(const UserPreset& selection) const;

    [[nodiscard]] std::expected<UserPresetListing, juce::String> list() const;

    [[nodiscard]] std::expected<UserPreset, juce::String> importPreset(
        const juce::File& source,
        const juce::String& name = {}
    ) const;
    [[nodiscard]] std::expected<UserPreset, juce::String> load(const UserPreset& selection) const;
    [[nodiscard]] std::expected<UserPreset, juce::String> rename(
        const UserPreset& selection,
        const juce::String& name
    ) const;
    [[nodiscard]] std::expected<UserPreset, juce::String> saveAs(
        const juce::String& name,
        const ProcessingSnapshot& snapshot
    ) const;
    [[nodiscard]] std::expected<UserPreset, juce::String> update(
        const UserPreset& selection,
        const ProcessingSnapshot& snapshot
    ) const;

private:
    /// Immutable parameter schema owned by the processor, which outlives every library operation.
    const juce::AudioProcessorValueTreeState& parameters;
    /// Configured storage path, created lazily by an explicit successful save or import.
    juce::File directory;
};
}  // namespace esgrove::image_control
