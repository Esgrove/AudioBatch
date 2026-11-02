/// Processor ownership transfers and ordered teardown for a live plugin chain slot.

#include "gui/LiveSlot.h"

#include "plugins/PluginChainPresets.h"

#include <memory>
#include <optional>
#include <utility>

namespace audiobatch
{
/// Builds a slot without a processor from its stored form.
LiveSlot::LiveSlot(SavedChainSlot savedSlot) :
    description(std::move(savedSlot.description)),
    state(std::move(savedSlot.state)),
    enabled(savedSlot.enabled)
{}

/// Tears the live processor down, so erasing or clearing slots never skips the teardown order.
LiveSlot::~LiveSlot()
{
    destroyProcessor();
}

/// Transfers the processor and editor window, leaving the source without either.
LiveSlot::LiveSlot(LiveSlot&& other) noexcept :
    id(other.id),
    listener(std::exchange(other.listener, nullptr)),
    editorWindow(std::exchange(other.editorWindow, nullptr)),
    description(std::move(other.description)),
    state(std::move(other.state)),
    preparationError(std::move(other.preparationError)),
    latencySamples(std::exchange(other.latencySamples, std::nullopt)),
    instance(std::move(other.instance)),
    enabled(other.enabled),
    prepared(std::exchange(other.prepared, false)),
    wasEditorOpen(std::exchange(other.wasEditorOpen, false))
{}

/// Tears down the current processor before taking over the source processor and editor window.
LiveSlot& LiveSlot::operator=(LiveSlot&& other) noexcept
{
    if (this != &other) {
        destroyProcessor();
        id = other.id;
        description = std::move(other.description);
        state = std::move(other.state);
        enabled = other.enabled;
        instance = std::move(other.instance);
        listener = std::exchange(other.listener, nullptr);
        editorWindow = std::exchange(other.editorWindow, nullptr);
        preparationError = std::move(other.preparationError);
        latencySamples = std::exchange(other.latencySamples, std::nullopt);
        prepared = std::exchange(other.prepared, false);
        wasEditorOpen = std::exchange(other.wasEditorOpen, false);
    }
    return *this;
}

/// Registers the chain-owned listener on a new processor. The listener must outlive the slot.
void LiveSlot::attachInstance(
    std::unique_ptr<juce::AudioPluginInstance> newInstance,
    juce::AudioProcessorListener& newListener
)
{
    destroyProcessor();
    instance = std::move(newInstance);
    if (instance != nullptr) {
        instance->addListener(&newListener);
        listener = &newListener;
    }
}

/// Deletes only the editor window, retaining the processor.
void LiveSlot::closeEditor()
{
    if (editorWindow != nullptr) {
        editorWindow.deleteAndZero();
    }
    wasEditorOpen = false;
}

/// Closes the editor before unregistering the listener, releasing resources, and destroying the processor.
void LiveSlot::destroyProcessor()
{
    closeEditor();
    if (instance != nullptr) {
        if (listener != nullptr) {
            instance->removeListener(listener);
        }
        if (prepared) {
            instance->releaseResources();
        }
        instance = nullptr;
    }
    listener = nullptr;
    prepared = false;
    latencySamples.reset();
}
}  // namespace audiobatch
