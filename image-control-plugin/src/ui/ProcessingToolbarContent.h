/// Declares the private processing-toolbar implementation shared by its presentation and preset workflows.
/// The processor owns sound state. A serialized worker owns filesystem commands and detached completions.

#pragma once

#include "ProcessingState.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/ProcessingToolbar.h"
#include "UserPresets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace esgrove::image_control
{
namespace test
{
struct PresetWorkflowTestAccess;
}
class ImageControlProcessor;

/// Disk operations never access mutable processor state or components.
enum class PresetOperation : std::uint8_t {
    list,
    load,
    reconnect,
    saveAs,
    update,
    rename,
    remove,
    importPreset,
    exportPreset,
};
/// Stable menu IDs are distinct from the preset selector's one-based library indexes.
enum class PresetMenuAction : std::uint8_t {
    saveAs = 1,
    update,
    rename,
    remove,
    importPreset,
    exportPreset,
    refresh,
    issues,
    reconnect,
};

/// A complete request owns all values used by the worker, including optimistic library identity checks.
struct PresetRequest final {
    /// Explicit library command, never inferred from a display name.
    PresetOperation operation {PresetOperation::list};
    /// Exact target file revision captured before a confirmation or worker request.
    std::optional<UserPreset> selection;
    /// Original transient binding, distinct from a load or reconnection target.
    std::optional<UserPreset> binding;
    /// Active sound captured before an asynchronous workflow begins.
    ProcessingSnapshot sound;
    /// Source or destination for explicit import and export commands.
    juce::File file;
    /// Validated requested display name.
    juce::String name;
    /// Persistent baseline identity captured independently of the transient file binding.
    juce::String provenanceId;
    /// Persistent baseline name captured to reject superseded completions.
    juce::String provenanceName;
    /// Persistent dirty-state baseline captured before the asynchronous workflow.
    std::optional<ProcessingSnapshot> baseline;
    /// Host restore epoch at request creation.
    std::uint64_t generation {0};
    /// Comparison slot that owns the captured provenance.
    int comparisonSlot {0};
};

/// A detached completion is consumed only by the message thread, never called from the disk worker.
struct PresetCompletion final {
    /// Immutable origin used to reject stale completions.
    PresetRequest request;
    /// Validated library revision returned by a successful command.
    std::optional<UserPreset> preset;
    /// Fresh library listing, which cannot change processor provenance or binding.
    std::optional<UserPresetListing> listing;
    /// Main operation failure, separate from a subsequent listing failure.
    juce::String error;
    /// Diagnostic for an unavailable post-operation listing.
    juce::String listingError;
};

/// Shared only until the independent worker has published its final detached result.
struct PresetMailbox final {
    /// Protects the detached completion, never processor or component state.
    std::mutex mutex;
    /// At most one serialized request can be outstanding.
    std::optional<PresetCompletion> completion;
};

/// Message-thread actions belong to the containing toolbar, which outlives its selector.
struct PresetNameCallbacks final {
    /// Starts ordinary inline naming.
    std::function<void()> edit;
    /// Accepts the current inline input.
    std::function<void()> accept;
    /// Discards the current inline draft.
    std::function<void()> cancel;
    /// Updates the undecorated name draft.
    std::function<void(const juce::String&)> change;
};

/// Edits a raw preset name separately from ComboBox selection and its modified-name decoration.
class PresetNameSelector final : public juce::ComboBox
{
public:
    PresetNameSelector();

    [[nodiscard]] juce::String getNameText() const;
    [[nodiscard]] bool isNameEditing() const;
    void beginNameEdit(const juce::String& name);
    void endNameEdit();
    void setNameCallbacks(PresetNameCallbacks actions);
    void setNameFeedback(const juce::String& feedback);

    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void resized() override;

private:
    /// Overlay retains input and caret when library items or toolbar bounds change.
    juce::TextEditor nameEditor;
    /// Callbacks never outlive the containing toolbar.
    PresetNameCallbacks callbacks;
};

/// Owns the toolbar's message-thread controls and dialogs. The processor outlives this component.
class ProcessingToolbarContent final : public juce::Component
{
public:
    ProcessingToolbarContent(
        ImageControlProcessor& owner,
        const juce::File& presetDirectory,
        PresetDialogPresentation dialogs
    );

    ~ProcessingToolbarContent() override;

    ProcessingToolbarContent(const ProcessingToolbarContent&) = delete;
    ProcessingToolbarContent(ProcessingToolbarContent&&) = delete;
    ProcessingToolbarContent& operator=(const ProcessingToolbarContent&) = delete;
    ProcessingToolbarContent& operator=(ProcessingToolbarContent&&) = delete;

    void redo();
    void refresh();
    void undo();

    void resized() override;

private:
    friend struct test::PresetWorkflowTestAccess;

    [[nodiscard]] PresetRequest makeRequest(PresetOperation operation) const;
    [[nodiscard]] juce::PopupMenu buildPresetMenu() const;
    [[nodiscard]] juce::String currentPresetName() const;
    [[nodiscard]] juce::String libraryFeedback() const;
    [[nodiscard]] bool canInteract() const;
    [[nodiscard]] bool canSave() const;
    [[nodiscard]] bool requestIsCurrent(const PresetRequest& request) const;

    void acceptCompletion(PresetCompletion completion);
    void acceptPresetResult(const PresetRequest& request, UserPreset preset);
    void applyListing(UserPresetListing listing);
    void beginDialog(std::function<void(int)> completion);
    void beginNameEdit(bool saveAs);
    void cancelNameEdit();
    void chooseExport();
    void chooseImport();
    void commitNameEdit();
    void confirmDelete();
    void confirmReconnect();
    void configureDialogPresentation();
    void finishDialog(int result);
    void handleMenuAction(int action);
    void loadSelectedPreset();
    void observePendingLoad();
    void queueRequest(PresetRequest request);
    void savePreset();
    void showDialog(
        const juce::String& title,
        const juce::String& message,
        juce::MessageBoxIconType icon,
        const juce::String& acceptText,
        const std::function<void()>& accepted
    );
    void showError(const juce::String& message);
    void showPresetMenu();
    void updatePresetSelector();

    /// The owning processor outlives this component and the independent disk work joined by its destructor.
    ImageControlProcessor& processor;
    /// Immutable parameter definitions referenced by the library outlive the joined disk worker.
    UserPresets library;

    /// Selects the first persistent comparison sound.
    juce::TextButton slotAButton {"A"};
    /// Selects the second persistent comparison sound.
    juce::TextButton slotBButton {"B"};
    /// Copies the current live sound to the inactive comparison slot.
    juce::TextButton copyButton;
    /// Lists validated presets and edits the active slot's name without recalling by typed text.
    PresetNameSelector presetSelector;
    /// Opens explicit library commands without changing processing.
    juce::TextButton presetMenuButton {"Presets"};

    /// Explicit per-toolbar presentation, never a process-global or production error suppression switch.
    const PresetDialogPresentation dialogPresentation;

    /// Modal windows keep the plugin palette even if they are not children of the editor.
    ImageControlLookAndFeel dialogLookAndFeel;
    /// Current asynchronous confirmation or name-entry window, cancelled during teardown.
    std::unique_ptr<juce::AlertWindow> dialog;
    /// Peerless test presentation invokes the same completion without registering a desktop modal window.
    std::function<void(int)> dialogCompletion;
    /// Native import/export chooser owned until completion or teardown.
    std::unique_ptr<juce::FileChooser> fileChooser;
    /// Last completed library scan, indexed by the selector's item IDs.
    std::vector<UserPreset> presets;
    /// Rejected library entries available through the diagnostics menu.
    std::vector<UserPresetIssue> issues;
    /// Detached worker result shared without cross-thread component access.
    std::shared_ptr<PresetMailbox> mailbox {std::make_shared<PresetMailbox>()};
    /// Successful metadata completion retained until a local sound gesture releases processor command admission.
    std::optional<PresetCompletion> deferredCompletion;
    /// Latest scan failure, presented without erasing the current sound.
    juce::String listingError;
    /// Contextual short description of the outstanding filesystem operation.
    juce::String pendingOperation;
    /// Last programmatically displayed selector text, preserving pending user selection notifications.
    std::optional<juce::String> shownPresetText;
    /// Captured processor identity prevents drafts from leaking across recall or comparison transitions.
    std::optional<PresetRequest> nameEditContext;
    /// Raw input never contains the presentation-only modified marker.
    juce::String draftName;
    /// Reachable inline validation or save failure, retained with the draft.
    juce::String nameError;
    /// Explicit Save As intent creates a new identity rather than updating the current binding.
    bool savingAs {false};
    /// A load is cancelled logically once an observed edit or slot transition supersedes its captured sound.
    std::optional<ProcessingSnapshot> pendingLoadSound;
    /// Slot captured when the outstanding preset load began.
    int pendingLoadSlot {0};
    /// Sticky rejection flag after an observed intervening edit or comparison change.
    bool pendingLoadChanged {false};
    /// One serialized disk request is outstanding.
    bool busy {false};
    /// Prevents completion handlers from scheduling new work during destruction.
    bool closing {false};
    /// Prevents overlapping library menus or modal workflows.
    bool menuOpen {false};

    /// Declared last and joined first. Jobs never wait for callbacks or access the component or processor.
    juce::ThreadPool diskWorker {juce::ThreadPool::Options {}
                                     .withNumberOfThreads(1)
                                     .withThreadName("ImageControl presets")
                                     .withDesiredThreadPriority(juce::Thread::Priority::low)};
};
}  // namespace esgrove::image_control
