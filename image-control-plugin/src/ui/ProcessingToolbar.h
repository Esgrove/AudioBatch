/// Declares ImageControl's compact processing toolbar for local history, A/B comparison, and user presets.
/// File operations run on a serialized worker. All controls, dialogs, and sound changes belong to the message thread.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace esgrove::image_control
{
class ImageControlProcessor;
class ProcessingToolbarContent;

/// Height of the processing row, including its vertical control insets, in logical pixels.
inline constexpr int PROCESSING_TOOLBAR_HEIGHT {32};

/// A processor-backed toolbar whose refresh is driven by the editor's existing VBlank callback.
/// The processor outlives this component. Destruction joins only independent filesystem work, never a UI callback.
class ProcessingToolbar final : public juce::Component
{
public:
    explicit ProcessingToolbar(ImageControlProcessor& processor);
    ProcessingToolbar(ImageControlProcessor& processor, const juce::File& presetDirectory);

    ~ProcessingToolbar() override;

    ProcessingToolbar(const ProcessingToolbar&) = delete;
    ProcessingToolbar(ProcessingToolbar&&) = delete;
    ProcessingToolbar& operator=(const ProcessingToolbar&) = delete;
    ProcessingToolbar& operator=(ProcessingToolbar&&) = delete;

    void refresh();

    void resized() override;

private:
    /// Implementation owns controls, async dialogs, and the disk worker in their dependency-safe order.
    std::unique_ptr<ProcessingToolbarContent> content;
};
}  // namespace esgrove::image_control
