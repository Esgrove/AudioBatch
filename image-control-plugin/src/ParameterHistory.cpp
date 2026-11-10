/// Implements bounded local undo transactions using explicit touched-parameter deltas.
/// No listeners observe automation, and host restore invalidation performs only an atomic increment.

#include "ParameterHistory.h"

#include "Parameters.h"
#include "ProcessingState.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Bounds local history independently of automation traffic and editor lifetime.
constexpr std::size_t MAX_HISTORY_ENTRIES {128};

/// One chronological local command. Metadata is an optional processor-owned A/B patch.
struct HistoryEntry final {
    /// User-facing description retained for the undo and redo controls.
    juce::String name;
    /// Plain-unit values after the command, containing only IDs that actually changed.
    ProcessingSnapshot after;
    /// Plain-unit values before the command, with exactly the same IDs as after.
    ProcessingSnapshot before;
    /// Optional processor metadata patch, invoked with true for undo and false for redo.
    std::function<void(bool)> metadata;
};
}  // namespace

/// Message-thread-owned containers with a separately atomic host-state generation.
class ParameterHistoryImpl final
{
public:
    /// Retains the processor-owned APVTS without registering any parameter listeners.
    explicit ParameterHistoryImpl(juce::AudioProcessorValueTreeState& state) : parameters(state) {}

    /// Releases retained local commands only when the owning processor is destroyed on its owner thread.
    ~ParameterHistoryImpl() = default;

    ParameterHistoryImpl(const ParameterHistoryImpl&) = delete;
    ParameterHistoryImpl(ParameterHistoryImpl&&) = delete;
    ParameterHistoryImpl& operator=(const ParameterHistoryImpl&) = delete;
    ParameterHistoryImpl& operator=(ParameterHistoryImpl&&) = delete;

    /// Acquires the processor's host-state mutex, or the standalone history's own recursive mutex.
    [[nodiscard]] std::unique_lock<std::recursive_mutex> lock() const
    {
        return std::unique_lock<std::recursive_mutex>(stateMutex.get());
    }

    /// Defers command destruction and gesture cancellation until local message-thread use.
    void synchronize()
    {
        const auto current = invalidation.load(std::memory_order_acquire);
        if (observedGeneration != current) {
            entries.clear();
            pending = {};
            position = 0;
            depth = 0;
            observedGeneration = current;
        }
    }

    /// Replays only changed parameters, stopping immediately if a synchronous host callback restores state.
    bool replay(const bool undo)
    {
        const auto stateLock = lock();
        synchronize();
        if (applying || depth != 0 || (undo ? position == 0 : position == entries.size())) {
            return false;
        }
        const juce::ScopedValueSetter<bool> guard(applying, true);
        // Copy the command because callbacks may invalidate and cause deferred containers to be cleared.
        const auto command = entries.at(undo ? position - 1 : position);
        const auto expectedGeneration = observedGeneration;
        if (command.metadata) {
            command.metadata(undo);
        }
        if (generationChanged(expectedGeneration)) {
            return false;
        }
        const auto& values = undo ? command.before : command.after;
        if (beforeReplay) {
            beforeReplay(values);
        }
        for (const auto& [parameterId, value] : values) {
            if (generationChanged(expectedGeneration)) {
                return false;
            }
            auto* parameter = parameters.getParameter(parameterId);
            if (parameter == nullptr || juce::exactlyEqual(plainValue(*parameter), value)) {
                continue;
            }
            parameter->beginChangeGesture();
            if (!generationChanged(expectedGeneration)) {
                parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
            }
            parameter->endChangeGesture();
        }
        if (generationChanged(expectedGeneration)) {
            return false;
        }
        // Keep replay targets paired with their exact readback, since skewed normalization is not idempotent.
        // Only replayed IDs are refreshed, and only after the complete generation-checked command succeeds.
        auto& recorded = entries.at(undo ? position - 1 : position);
        auto& recalled = undo ? recorded.before : recorded.after;
        for (auto& [parameterId, value] : recalled) {
            value = parameters.getRawParameterValue(parameterId)->load(std::memory_order_relaxed);
        }
        if (undo) {
            --position;
        } else {
            ++position;
        }
        if (command.metadata && metadataChanged) {
            metadataChanged();
        }
        return !generationChanged(expectedGeneration);
    }

    /// Rejects stale work without destroying data on the host's restore thread.
    [[nodiscard]] bool generationChanged(const std::uint64_t expected) const
    {
        return invalidation.load(std::memory_order_acquire) != expected;
    }

    /// Processor-owned parameters, retained without registering automation listeners.
    juce::AudioProcessorValueTreeState& parameters;
    /// Outer transaction under construction, containing only explicitly touched IDs.
    HistoryEntry pending;
    /// Commands retained on the local chronological undo/redo branch.
    std::vector<HistoryEntry> entries;
    /// Processor notification sent after a complete metadata command or replay.
    std::function<void()> metadataChanged;
    /// Processor hook runs after slot metadata changes and before replay emits parameter notifications.
    std::function<void(const ProcessingSnapshot&)> beforeReplay;
    /// Fallback synchronization when this history is used without a processor state mutex.
    mutable std::recursive_mutex standaloneMutex;
    /// Active processor or fallback mutex reference, installed before history becomes externally accessible.
    std::reference_wrapper<std::recursive_mutex> stateMutex {standaloneMutex};
    /// Host-restore token. Only this atomic is modified by invalidate on a foreign thread.
    std::atomic<std::uint64_t> invalidation {0};
    /// Generation represented by the message-thread-owned command containers.
    std::uint64_t observedGeneration {0};
    /// Number of nested open edits. Only the outermost end commits a command.
    std::size_t depth {0};
    /// Index immediately after the newest applied command.
    std::size_t position {0};
    /// Reentrancy guard while replaying deltas or delivering metadata completion notifications.
    bool applying {false};
};

/// Creates explicit local history with no APVTS UndoManager or parameter listener.
ParameterHistory::ParameterHistory(juce::AudioProcessorValueTreeState& parameters) :
    implementation(std::make_unique<ParameterHistoryImpl>(parameters))
{}

/// Destroys callbacks on the owner's thread, after the processor has finished all in-flight host callbacks.
ParameterHistory::~ParameterHistory() = default;

/// Returns the label of the next undo command, or no label while editing or after a host restore.
juce::String ParameterHistory::undoDescription() const
{
    const auto stateLock = implementation->lock();
    return canUndo() ? implementation->entries.at(implementation->position - 1).name : juce::String();
}

/// Returns the label of the next redo command without observing or recording host automation.
juce::String ParameterHistory::redoDescription() const
{
    const auto stateLock = implementation->lock();
    return canRedo() ? implementation->entries.at(implementation->position).name : juce::String();
}

/// Reports availability without deleting old transactions on a foreign thread.
bool ParameterHistory::canUndo() const
{
    const auto stateLock = implementation->lock();
    return !implementation->generationChanged(implementation->observedGeneration) && !implementation->applying
        && implementation->depth == 0 && implementation->position > 0;
}

/// Reports redo availability only outside an explicit local edit and in the current host-state generation.
bool ParameterHistory::canRedo() const
{
    const auto stateLock = implementation->lock();
    return !implementation->generationChanged(implementation->observedGeneration) && !implementation->applying
        && implementation->depth == 0 && implementation->position < implementation->entries.size();
}

/// A host restore logically cancels even an edit whose stale end callback has not arrived yet.
bool ParameterHistory::isEditing() const
{
    const auto stateLock = implementation->lock();
    return !implementation->generationChanged(implementation->observedGeneration) && implementation->depth != 0;
}

/// Reports a synchronous replay so processor commands cannot nest inside its parameter notifications.
bool ParameterHistory::isReplaying() const
{
    const auto stateLock = implementation->lock();
    return implementation->applying;
}

/// Applies the previous local delta with balanced host gestures, leaving all unrelated parameters alone.
bool ParameterHistory::undo()
{
    return implementation->replay(true);
}

/// Applies the next local delta in the same chronological stream as processor metadata commands.
bool ParameterHistory::redo()
{
    return implementation->replay(false);
}

/// Returns the atomic host-restore token for processor commands that notify reentrant host listeners.
std::uint64_t ParameterHistory::generation() const noexcept
{
    return implementation->invalidation.load(std::memory_order_acquire);
}

/// Begins an explicitly named gesture. Nested edits join the outer command and retain its name.
void ParameterHistory::beginEdit(const juce::String& name)
{
    const auto stateLock = implementation->lock();
    implementation->synchronize();
    if (implementation->applying) {
        return;
    }
    if (implementation->depth == 0) {
        implementation->pending = {};
        implementation->pending.name = name;
    }
    ++implementation->depth;
}

/// Records the current atomic value once, immediately before the adapter's first local write to this ID.
void ParameterHistory::touch(const juce::String& parameterId)
{
    const auto stateLock = implementation->lock();
    implementation->synchronize();
    if (implementation->applying || implementation->depth == 0) {
        return;
    }
    if (const auto* value = implementation->parameters.getRawParameterValue(parameterId)) {
        implementation->pending.before.try_emplace(parameterId, value->load(std::memory_order_relaxed));
    }
}

/// Commits only changed touched IDs when the outer gesture closes. A no-op keeps the existing redo branch.
void ParameterHistory::endEdit()
{
    const auto stateLock = implementation->lock();
    implementation->synchronize();
    if (implementation->applying || implementation->depth == 0) {
        return;
    }
    --implementation->depth;
    if (implementation->depth != 0) {
        return;
    }
    auto command = std::exchange(implementation->pending, {});
    for (auto iterator = command.before.begin(); iterator != command.before.end();) {
        const auto* value = implementation->parameters.getRawParameterValue(iterator->first);
        if (value == nullptr) {
            iterator = command.before.erase(iterator);
            continue;
        }
        const auto current = value->load(std::memory_order_relaxed);
        if (juce::exactlyEqual(iterator->second, current)) {
            iterator = command.before.erase(iterator);
        } else {
            command.after.emplace(iterator->first, current);
            ++iterator;
        }
    }
    if (command.before.empty() && !command.metadata) {
        return;
    }
    implementation->entries.resize(implementation->position);
    if (implementation->entries.size() == MAX_HISTORY_ENTRIES) {
        implementation->entries.erase(implementation->entries.begin());
    }
    implementation->entries.push_back(std::move(command));
    implementation->position = implementation->entries.size();
    if (implementation->entries.back().metadata && implementation->metadataChanged) {
        const juce::ScopedValueSetter<bool> guard(implementation->applying, true);
        implementation->metadataChanged();
    }
}

/// Invalidates all local history with no allocation, locks, listener callbacks, or command destruction.
void ParameterHistory::invalidate() noexcept
{
    implementation->invalidation.fetch_add(1, std::memory_order_acq_rel);
}

/// Installs processor synchronization during construction, before local adapters or host state callbacks run.
void ParameterHistory::setStateMutex(std::recursive_mutex& mutex)
{
    implementation->stateMutex = std::ref(mutex);
}

/// Adds a metadata patch to the outer transaction, chaining nested patches in reverse order for undo.
void ParameterHistory::setMetadataChange(std::function<void(bool)> change)
{
    const auto stateLock = implementation->lock();
    implementation->synchronize();
    if (implementation->applying || implementation->depth == 0 || !change) {
        return;
    }
    auto previousChange = std::move(implementation->pending.metadata);
    implementation->pending.metadata
        = [previous = std::move(previousChange), next = std::move(change)](const bool undo) {
              if (undo) {
                  next(true);
                  if (previous) {
                      previous(true);
                  }
              } else {
                  if (previous) {
                      previous(false);
                  }
                  next(false);
              }
          };
}

/// Installs a processor-owned pre-replay hook under the same state lock as local commands and host restores.
/// The hook receives only replayed IDs, after slot metadata is applied and before parameter notifications.
void ParameterHistory::setReplayCallback(std::function<void(const ProcessingSnapshot&)> callback)
{
    const auto stateLock = implementation->lock();
    implementation->beforeReplay = std::move(callback);
}

/// Installs the processor notification sent only after a complete metadata command has been committed or replayed.
void ParameterHistory::setMetadataChangedCallback(std::function<void()> callback)
{
    const auto stateLock = implementation->lock();
    implementation->metadataChanged = std::move(callback);
}
}  // namespace esgrove::image_control
