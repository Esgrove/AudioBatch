/// Declares ImageControl's A/B and preset row, with history commands shared by the editor's header and shortcuts.
/// File operations run on a serialized worker. All controls, dialogs, and sound changes belong to the message thread.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <memory>

namespace esgrove::image_control
{
class ImageControlProcessor;
class ProcessingToolbarContent;

/// Controls alert presentation per instance.
/// Non-rendering dialogs retain contents and actions without desktop peers or modality.
enum class PresetDialogPresentation : std::uint8_t { visible, nonRendering };

/// Height of the processing row, including its vertical control insets, in logical pixels.
inline constexpr int PROCESSING_TOOLBAR_HEIGHT {32};
/// Shared action widths and gaps keep editor packing identical to the toolbar's own layout.
inline constexpr int PROCESSING_TOOLBAR_SLOT_WIDTH {32};
inline constexpr int PROCESSING_TOOLBAR_COPY_WIDTH {72};
inline constexpr int PROCESSING_TOOLBAR_MENU_WIDTH {72};
inline constexpr int PROCESSING_TOOLBAR_CONTROL_GAP {4};
inline constexpr int PROCESSING_TOOLBAR_GROUP_GAP {12};
/// Preset names retain a usable minimum and stay bounded in either placement.
inline constexpr int PROCESSING_TOOLBAR_MIN_SELECTOR_WIDTH {100};
inline constexpr int PROCESSING_TOOLBAR_MAX_SELECTOR_WIDTH {320};
/// Fixed actions and their adjacent gaps, excluding the flexible preset selector.
inline constexpr int PROCESSING_TOOLBAR_FIXED_WIDTH {
    PROCESSING_TOOLBAR_SLOT_WIDTH * 2 + PROCESSING_TOOLBAR_CONTROL_GAP + PROCESSING_TOOLBAR_COPY_WIDTH
        + PROCESSING_TOOLBAR_GROUP_GAP + PROCESSING_TOOLBAR_CONTROL_GAP + PROCESSING_TOOLBAR_MENU_WIDTH,
};
/// Smallest inline allocation that preserves every action and a readable preset name.
inline constexpr int PROCESSING_TOOLBAR_INLINE_MIN_WIDTH {
    PROCESSING_TOOLBAR_FIXED_WIDTH + PROCESSING_TOOLBAR_MIN_SELECTOR_WIDTH,
};
/// Maximum allocation bounds the preset name field even in very wide headers.
inline constexpr int PROCESSING_TOOLBAR_MAX_WIDTH {
    PROCESSING_TOOLBAR_FIXED_WIDTH + PROCESSING_TOOLBAR_MAX_SELECTOR_WIDTH,
};

/// A processor-backed toolbar whose refresh is driven by the editor's existing VBlank callback.
/// The processor outlives this component. Destruction joins only independent filesystem work, never a UI callback.
class ProcessingToolbar final : public juce::Component
{
public:
    explicit ProcessingToolbar(ImageControlProcessor& processor);
    ProcessingToolbar(
        ImageControlProcessor& processor,
        const juce::File& presetDirectory,
        PresetDialogPresentation dialogs = PresetDialogPresentation::visible
    );

    ~ProcessingToolbar() override;

    ProcessingToolbar(const ProcessingToolbar&) = delete;
    ProcessingToolbar(ProcessingToolbar&&) = delete;
    ProcessingToolbar& operator=(const ProcessingToolbar&) = delete;
    ProcessingToolbar& operator=(ProcessingToolbar&&) = delete;

    void focusControls();
    void redo();
    void refresh();
    void undo();

    void resized() override;

private:
    /// Implementation owns controls, async dialogs, and the disk worker in their dependency-safe order.
    std::unique_ptr<ProcessingToolbarContent> content;
};
}  // namespace esgrove::image_control
