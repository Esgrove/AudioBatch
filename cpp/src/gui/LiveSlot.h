/// One processor-owning slot shared by the live plugin chain and its editor.
/// Owns the existing processor lifecycle and the stable identity used by chain editor rows.

#pragma once

#include "plugins/PluginChainPresets.h"

#include <JuceHeader.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace audiobatch
{
/// Identifies a slot for its lifetime, independently of its position in the chain.
enum class PluginSlotId : std::uint64_t {};

/// Owns the processor used for editing and live playback.
/// Teardown closes the editor, unregisters its listener, releases preparation, and destroys the instance.
/// Moves transfer ownership and leave the source slot empty.
struct LiveSlot {
    LiveSlot() = default;
    explicit LiveSlot(SavedChainSlot savedSlot);

    ~LiveSlot();

    LiveSlot(const LiveSlot&) = delete;
    LiveSlot& operator=(const LiveSlot&) = delete;
    LiveSlot(LiveSlot&& other) noexcept;
    LiveSlot& operator=(LiveSlot&& other) noexcept;

    void attachInstance(
        std::unique_ptr<juce::AudioPluginInstance> newInstance,
        juce::AudioProcessorListener& newListener
    );
    void closeEditor();
    void destroyProcessor();

    /// Stable id assigned when the chain adds the slot.
    PluginSlotId id {};

    /// Listener owned by the chain, null exactly when the slot has no instance.
    juce::AudioProcessorListener* listener {nullptr};
    /// Editor window, which hides when closed, or null when not open.
    juce::Component::SafePointer<juce::DialogWindow> editorWindow;
    /// Plugin descriptor, including plugins that are not installed.
    juce::PluginDescription description;
    /// Last captured or loaded processor state.
    juce::MemoryBlock state;
    /// Creation or preparation error, empty after success.
    juce::String preparationError;

    /// Latency at the preview rate, empty while unprepared.
    std::optional<juce::int64> latencySamples;
    /// Shared editing and playback processor, created on first use.
    std::unique_ptr<juce::AudioPluginInstance> instance;

    /// True when the slot participates in processing and preview.
    bool enabled {true};
    /// True while the processor is prepared at the preview rate and block size.
    bool prepared {false};
    /// True until the closed editor window has been reaped.
    bool wasEditorOpen {false};
};
}  // namespace audiobatch
