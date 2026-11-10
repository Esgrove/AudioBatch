/// Declares processor-owned local parameter history, independent of the host's automation undo manager.
/// Only explicit message-thread edits are recorded. Host restore invalidates them with an atomic generation.

#pragma once

#include "ProcessingState.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace esgrove::image_control
{
/// Private implementation retains all mutable command data outside the public adapter interface.
class ParameterHistoryImpl;

/// Explicit, nested local transactions containing only parameters touched before their local writes.
/// All methods except invalidate and generation belong to the message thread.
class ParameterHistory final
{
public:
    explicit ParameterHistory(juce::AudioProcessorValueTreeState& parameters);

    ~ParameterHistory();

    ParameterHistory(const ParameterHistory&) = delete;
    ParameterHistory(ParameterHistory&&) = delete;
    ParameterHistory& operator=(const ParameterHistory&) = delete;
    ParameterHistory& operator=(ParameterHistory&&) = delete;

    [[nodiscard]] juce::String redoDescription() const;
    [[nodiscard]] juce::String undoDescription() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool isEditing() const;
    [[nodiscard]] bool isReplaying() const;

    [[nodiscard]] bool redo();
    [[nodiscard]] bool undo();
    [[nodiscard]] std::uint64_t generation() const noexcept;
    void beginEdit(const juce::String& name);
    void endEdit();
    void invalidate() noexcept;
    void touch(const juce::String& parameterId);

    void setMetadataChange(std::function<void(bool)> change);
    void setMetadataChangedCallback(std::function<void()> callback);
    void setReplayCallback(std::function<void(const ProcessingSnapshot&)> callback);
    void setStateMutex(std::recursive_mutex& mutex);

private:
    /// Owns local command storage and atomic invalidation independently of any editor lifetime.
    std::unique_ptr<ParameterHistoryImpl> implementation;
};
}  // namespace esgrove::image_control
