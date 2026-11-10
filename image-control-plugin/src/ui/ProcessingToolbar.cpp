/// Implements A/B and user-preset controls, plus history commands shared by the editor's header and shortcuts.
/// A single disk worker publishes detached results for refresh to consume without any message-thread rendezvous.
/// Dialogs and file choosers are owned by the toolbar, and asynchronous callbacks check component SafePointers.

#include "ui/ProcessingToolbar.h"

#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "ProcessingState.h"
#include "ui/ImageControlLookAndFeel.h"
#include "UserPresets.h"
#include <initializer_list>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Compact control geometry fits inside the editor's 760-pixel minimum width.
constexpr int CONTROL_INSET {4};
constexpr int CONTROL_GAP {4};
constexpr int GROUP_GAP {12};
constexpr int SLOT_WIDTH {32};
constexpr int COPY_WIDTH {72};
constexpr int MENU_WIDTH {72};
constexpr int STATUS_WIDTH {76};
/// Status text remains legible without competing with the compact preset selector.
constexpr float STATUS_FONT_HEIGHT {12.0F};
/// Stable left-to-right keyboard traversal, independent of member declaration order.
constexpr int SLOT_A_FOCUS_ORDER {1};
constexpr int SLOT_B_FOCUS_ORDER {2};
constexpr int COPY_FOCUS_ORDER {3};
constexpr int PRESET_SELECTOR_FOCUS_ORDER {4};
constexpr int PRESET_MENU_FOCUS_ORDER {5};
/// Dialog result IDs are explicit, including Cancel as the non-destructive default for confirmations.
constexpr int ACCEPT_RESULT {1};
constexpr int CANCEL_RESULT {0};
constexpr auto NAME_FIELD {"presetName"};
/// Maximum number of rejected file diagnostics displayed in one error dialog.
constexpr std::size_t MAX_VISIBLE_ISSUES {8};

/// Disk operations never access mutable processor state or components.
enum class PresetOperation : std::uint8_t { list, load, saveAs, update, rename, remove, importPreset, exportPreset };
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
};

/// A complete request owns all values used by the worker, including optimistic library identity checks.
struct PresetRequest final {
    PresetOperation operation {PresetOperation::list};
    std::optional<UserPreset> selection;
    ProcessingSnapshot sound;
    juce::File file;
    juce::String name;
    std::uint64_t generation {0};
    int comparisonSlot {0};
};

/// A detached completion is consumed only by the message thread, never called from the disk worker.
struct PresetCompletion final {
    PresetRequest request;
    std::optional<UserPreset> preset;
    std::optional<UserPresetListing> listing;
    juce::String error;
    juce::String listingError;
};

/// Shared only until the independent worker has published its final detached result.
struct PresetMailbox final {
    std::mutex mutex;
    std::optional<PresetCompletion> completion;
};

/// Moves a validated library result into the worker completion without losing its conflict-check fingerprint.
void capturePresetResult(PresetCompletion& completion, std::expected<UserPreset, juce::String> result)
{
    if (result.has_value()) {
        completion.preset = std::move(*result);
    } else {
        completion.error = result.error();
    }
}

/// Executes commands requiring an existing revision only after the caller has checked the optional selection.
void performSelectedPresetRequest(const UserPresets& library, const UserPreset& selection, PresetCompletion& completion)
{
    const auto& command = completion.request;
    switch (command.operation) {
        case PresetOperation::load:
            capturePresetResult(completion, library.load(selection));
            break;
        case PresetOperation::update:
            capturePresetResult(completion, library.update(selection, command.sound));
            break;
        case PresetOperation::rename:
            capturePresetResult(completion, library.rename(selection, command.name));
            break;
        case PresetOperation::remove: {
            const auto result = library.remove(selection);
            if (result.failed()) {
                completion.error = result.getErrorMessage();
            }
            break;
        }
        case PresetOperation::exportPreset: {
            const auto result = library.exportPreset(selection, command.file);
            if (result.failed()) {
                completion.error = result.getErrorMessage();
            }
            break;
        }
        case PresetOperation::list:
        case PresetOperation::saveAs:
        case PresetOperation::importPreset:
            break;
    }
}

/// Runs one library command and refreshes its listing on the same serialized worker.
PresetCompletion performPresetRequest(const UserPresets& library, PresetRequest request)
{
    PresetCompletion completion;
    completion.request = std::move(request);
    const auto& command = completion.request;
    try {
        switch (command.operation) {
            case PresetOperation::list:
                break;
            case PresetOperation::saveAs:
                capturePresetResult(completion, library.saveAs(command.name, command.sound));
                break;
            case PresetOperation::importPreset:
                capturePresetResult(completion, library.importPreset(command.file));
                break;
            case PresetOperation::load:
            case PresetOperation::update:
            case PresetOperation::rename:
            case PresetOperation::remove:
            case PresetOperation::exportPreset:
                if (command.selection.has_value()) {
                    performSelectedPresetRequest(library, *command.selection, completion);
                } else {
                    completion.error = "Select a user preset before choosing this action.";
                }
                break;
        }
        auto listing = library.list();
        if (listing.has_value()) {
            completion.listing = std::move(*listing);
        } else {
            completion.listingError = listing.error();
        }
    } catch (const std::exception& exception) {
        completion.error = juce::String("Preset operation failed: ") + exception.what();
    } catch (...) {
        completion.error = "Preset operation failed unexpectedly.";
    }
    return completion;
}

/// Describes malformed or unsupported files without flooding the dialog with an unbounded directory listing.
juce::String describeIssues(const std::vector<UserPresetIssue>& issues)
{
    juce::String description;
    const auto count = std::min(issues.size(), MAX_VISIBLE_ISSUES);
    for (std::size_t index = 0; index < count; ++index) {
        if (description.isNotEmpty()) {
            description += juce::newLine;
        }
        description += issues.at(index).file.getFileName() + ": " + issues.at(index).message;
    }
    if (issues.size() > count) {
        description += juce::newLine + juce::String(static_cast<int>(issues.size() - count)) + " more rejected files.";
    }
    return description;
}

/// Assigns a stable automation-test ID and a screen-reader name without changing visible button copy.
void identifyControl(
    juce::Component& control,
    const juce::String& identifier,
    const juce::String& title,
    const int focusOrder
)
{
    control.setComponentID(identifier);
    control.setTitle(title);
    control.setDescription(title);
    control.setWantsKeyboardFocus(true);
    control.setExplicitFocusOrder(focusOrder);
}
}  // namespace

/// Owns the toolbar's UI and serial worker. The processor is never accessed from the worker.
class ProcessingToolbarContent final : public juce::Component
{
public:
    ProcessingToolbarContent(ImageControlProcessor& owner, const juce::File& presetDirectory);

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
    [[nodiscard]] bool canInteract() const;
    [[nodiscard]] PresetRequest makeRequest(PresetOperation operation) const;

    void acceptCompletion(PresetCompletion completion);
    void acceptPresetResult(const PresetRequest& request, UserPreset preset);
    void applyListing(UserPresetListing listing);
    void chooseExport();
    void chooseImport();
    void confirmDelete();
    void handleMenuAction(int action);
    void loadSelectedPreset();
    void observePendingLoad();
    void promptName(bool renaming);
    void queueRequest(PresetRequest request);
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
    /// Lists validated presets and presents the active slot's modified baseline.
    juce::ComboBox presetSelector;
    /// Read-only progress, library-error, or modified-state summary.
    juce::Label presetStatus;
    /// Opens explicit library commands without changing processing.
    juce::TextButton presetMenuButton {"Presets"};

    /// Modal windows keep the plugin palette even if they are not children of the editor.
    ImageControlLookAndFeel dialogLookAndFeel;
    /// Current asynchronous confirmation or name-entry window, cancelled during teardown.
    std::unique_ptr<juce::AlertWindow> dialog;
    /// Native import/export chooser owned until completion or teardown.
    std::unique_ptr<juce::FileChooser> fileChooser;
    /// Last completed library scan, indexed by the selector's item IDs.
    std::vector<UserPreset> presets;
    /// Rejected library entries available through the diagnostics menu.
    std::vector<UserPresetIssue> issues;
    /// Detached worker result shared without cross-thread component access.
    std::shared_ptr<PresetMailbox> mailbox {std::make_shared<PresetMailbox>()};
    /// Latest scan failure, presented without erasing the current sound.
    juce::String listingError;
    /// Last programmatically displayed selector text, preserving pending user selection notifications.
    std::optional<juce::String> shownPresetText;
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

/// Installs controls in left-to-right keyboard order and starts a non-blocking initial preset scan.
ProcessingToolbarContent::ProcessingToolbarContent(ImageControlProcessor& owner, const juce::File& presetDirectory) :
    processor(owner),
    library(owner.getParameters(), presetDirectory)
{
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle("A/B comparison and presets");
    identifyControl(slotAButton, "comparison-a", "Comparison slot A", SLOT_A_FOCUS_ORDER);
    identifyControl(slotBButton, "comparison-b", "Comparison slot B", SLOT_B_FOCUS_ORDER);
    identifyControl(copyButton, "comparison-copy", "Copy active comparison to the other slot", COPY_FOCUS_ORDER);
    identifyControl(presetSelector, "preset-selector", "User preset", PRESET_SELECTOR_FOCUS_ORDER);
    identifyControl(presetMenuButton, "preset-menu", "Preset actions", PRESET_MENU_FOCUS_ORDER);
    presetStatus.setComponentID("preset-status");
    presetStatus.setTitle("Preset status");
    presetStatus.setFont(textFont(STATUS_FONT_HEIGHT));
    presetStatus.setJustificationType(juce::Justification::centredRight);
    presetStatus.setInterceptsMouseClicks(false, false);
    slotAButton.setConnectedEdges(juce::Button::ConnectedOnRight);
    slotBButton.setConnectedEdges(juce::Button::ConnectedOnLeft);
    slotAButton.setToggleable(true);
    slotBButton.setToggleable(true);
    slotAButton.setTooltip("Recall processing slot A. Display settings and audition controls are unchanged");
    slotBButton.setTooltip("Recall processing slot B. Display settings and audition controls are unchanged");
    presetSelector.setEditableText(false);
    presetSelector.setTextWhenNothingSelected("Unsaved sound");
    presetSelector.setTextWhenNoChoicesAvailable("No user presets. Choose Save As from Presets");
    presetSelector.setTooltip("Load a user preset into the active comparison slot. * means the sound has changed");
    presetMenuButton.setTooltip("Save As, Update, Rename, Delete, Import, Export, and Refresh user presets");
    slotAButton.onClick = [this] {
        if (processor.selectComparisonSlot(0)) {
            pendingLoadChanged = pendingLoadSound.has_value();
            refresh();
        }
    };
    slotBButton.onClick = [this] {
        if (processor.selectComparisonSlot(1)) {
            pendingLoadChanged = pendingLoadSound.has_value();
            refresh();
        }
    };
    copyButton.onClick = [this] {
        if (processor.copyComparisonToOtherSlot()) {
            refresh();
        }
    };
    presetSelector.onChange = [this] { loadSelectedPreset(); };
    presetMenuButton.onClick = [this] { showPresetMenu(); };
    for (auto* control :
         std::initializer_list<juce::Component*> {
             &slotAButton,
             &slotBButton,
             &copyButton,
             &presetSelector,
             &presetStatus,
             &presetMenuButton,
         })
    {
        addAndMakeVisible(*control);
    }
    queueRequest(makeRequest(PresetOperation::list));
    refresh();
}

/// Cancels dialogs and joins only filesystem work. The worker never needs a message callback to finish.
ProcessingToolbarContent::~ProcessingToolbarContent()
{
    closing = true;
    fileChooser.reset();
    dialog.reset();
    diskWorker.removeAllJobs(true, -1);
}

/// Rejects overlapping disk commands and modal interactions without preventing ordinary sound editing.
bool ProcessingToolbarContent::canInteract() const
{
    return !busy && !closing && !menuOpen && dialog == nullptr && fileChooser == nullptr
        && !processor.getHistory().isEditing();
}

/// Captures all processing state and host-restore identity on the message thread before dispatching disk work.
PresetRequest ProcessingToolbarContent::makeRequest(const PresetOperation operation) const
{
    PresetRequest request;
    request.operation = operation;
    request.generation = processor.getHistory().generation();
    request.selection = processor.getSelectedUserPreset();
    request.sound = processor.getSoundSnapshot();
    request.comparisonSlot = processor.getActiveComparisonSlot();
    return request;
}

/// Lays out the 24-pixel controls with a flexible preset selector, including at the 760-pixel editor minimum.
void ProcessingToolbarContent::resized()
{
    auto area = getLocalBounds().reduced(0, CONTROL_INSET);
    slotAButton.setBounds(area.removeFromLeft(SLOT_WIDTH));
    slotBButton.setBounds(area.removeFromLeft(SLOT_WIDTH));
    area.removeFromLeft(CONTROL_GAP);
    copyButton.setBounds(area.removeFromLeft(COPY_WIDTH));
    area.removeFromLeft(GROUP_GAP);
    presetMenuButton.setBounds(area.removeFromRight(MENU_WIDTH));
    area.removeFromRight(CONTROL_GAP);
    presetStatus.setBounds(area.removeFromRight(STATUS_WIDTH));
    area.removeFromRight(CONTROL_GAP);
    presetSelector.setBounds(area);
}

/// Replays a history action and invalidates a pending preset load even if another action restores the same sound.
void ProcessingToolbarContent::undo()
{
    if (processor.getHistory().undo()) {
        pendingLoadChanged = pendingLoadSound.has_value();
    }
    refresh();
}

/// Uses the same pending-load guard for redo, independent of the row's visibility.
void ProcessingToolbarContent::redo()
{
    if (processor.getHistory().redo()) {
        pendingLoadChanged = pendingLoadSound.has_value();
    }
    refresh();
}

/// Consumes a detached worker result and refreshes A/B, preset selection, and modified status.
void ProcessingToolbarContent::refresh()
{
    observePendingLoad();
    std::optional<PresetCompletion> completion;
    {
        const std::scoped_lock lock(mailbox->mutex);
        completion = std::move(mailbox->completion);
        mailbox->completion.reset();
    }
    if (completion.has_value()) {
        busy = false;
        acceptCompletion(std::move(*completion));
        pendingLoadSound.reset();
    }
    auto const& history = processor.getHistory();
    const bool slotA = processor.getActiveComparisonSlot() == 0;
    slotAButton.setToggleState(slotA, juce::dontSendNotification);
    slotBButton.setToggleState(!slotA, juce::dontSendNotification);
    slotAButton.setEnabled(!history.isEditing());
    slotBButton.setEnabled(!history.isEditing());
    copyButton.setEnabled(!history.isEditing());
    copyButton.setButtonText(juce::String::fromUTF8(slotA ? "A \xe2\x86\x92 B" : "B \xe2\x86\x92 A"));
    copyButton.setTitle(slotA ? "Copy A to B" : "Copy B to A");
    copyButton.setTooltip(
        slotA ? "Replace slot B with the current sound in A, keeping A active"
              : "Replace slot A with the current sound in B, keeping B active"
    );
    presetSelector.setEnabled(canInteract());
    presetMenuButton.setEnabled(canInteract());
    updatePresetSelector();
}

/// Updates presentation without touching disk or changing a selected preset's conflict-check fingerprint.
void ProcessingToolbarContent::updatePresetSelector()
{
    const auto selection = processor.getSelectedUserPreset();
    const bool modified
        = selection.has_value() && !sameProcessingState(selection->snapshot, processor.getSoundSnapshot());
    const auto text = selection.has_value() ? selection->name + (modified ? " *" : "") : juce::String("Unsaved sound");
    // Do not overwrite a newly chosen ComboBox item before its asynchronous onChange callback is delivered.
    if (!shownPresetText.has_value() || *shownPresetText != text) {
        presetSelector.setText(text, juce::dontSendNotification);
        shownPresetText = text;
    }
    juce::String status;
    if (busy) {
        status = "Working...";
    } else if (listingError.isNotEmpty()) {
        status = "Scan failed";
    } else if (!issues.empty()) {
        status = juce::String(static_cast<int>(issues.size())) + " invalid";
    } else if (modified) {
        status = "Modified";
    } else if (!selection.has_value()) {
        status = "Unsaved";
    }
    presetStatus.setText(status, juce::dontSendNotification);
    presetStatus.setColour(juce::Label::textColourId, juce::Colour(busy ? palette::BLUE : palette::YELLOW));
    juce::String tooltip;
    if (listingError.isNotEmpty()) {
        tooltip = listingError;
    } else if (!issues.empty()) {
        tooltip = describeIssues(issues);
    } else if (modified) {
        tooltip = "The active sound differs from the saved preset";
    } else if (selection.has_value()) {
        tooltip = "The selected preset is unchanged";
    } else {
        tooltip = "The active sound has not been saved as a user preset";
    }
    presetStatus.setTooltip(tooltip);
    presetStatus.setDescription(status);
}

/// Serializes disk jobs and publishes only values. The worker never posts callbacks or touches SafePointers.
void ProcessingToolbarContent::queueRequest(PresetRequest request)
{
    if (busy || closing) {
        return;
    }
    if (request.generation != processor.getHistory().generation()) {
        showError("The host restored a different plugin state. Please choose the preset action again.");
        return;
    }
    busy = true;
    pendingLoadChanged = false;
    if (request.operation == PresetOperation::load) {
        pendingLoadSlot = request.comparisonSlot;
        pendingLoadSound = request.sound;
    }
    const auto sharedMailbox = mailbox;
    // The destructor joins the pool before destroying the library or its processor-owned parameter definitions.
    diskWorker.addJob([sharedMailbox, &presetLibrary = library, request] mutable {
        auto completion = performPresetRequest(presetLibrary, std::move(request));
        const std::scoped_lock lock(sharedMailbox->mutex);
        sharedMailbox->completion = std::move(completion);
    });
}

/// Rebuilds the selector from a detached scan without replacing the selected revision or its baseline.
void ProcessingToolbarContent::applyListing(UserPresetListing listing)
{
    presets = std::move(listing.presets);
    issues = std::move(listing.rejectedFiles);
    presetSelector.clear(juce::dontSendNotification);
    shownPresetText.reset();
    int identifier = 1;
    for (const auto& preset : presets) {
        presetSelector.addItem(preset.name, identifier++);
    }
}

/// Remembers observed intervening edits, even when an undo or A/B round trip later restores the captured sound.
void ProcessingToolbarContent::observePendingLoad()
{
    if (!pendingLoadSound.has_value() || pendingLoadChanged) {
        return;
    }
    pendingLoadChanged = processor.getHistory().isEditing() || processor.getHistory().isReplaying()
        || pendingLoadSlot != processor.getActiveComparisonSlot()
        || !sameProcessingState(*pendingLoadSound, processor.getSoundSnapshot());
}

/// Applies sound recall only while its original slot and sound are still current.
/// Import is library-only, and must never replace the active slot's preset identity or modified baseline.
void ProcessingToolbarContent::acceptPresetResult(const PresetRequest& request, UserPreset preset)
{
    if (request.operation == PresetOperation::importPreset) {
        return;
    }
    if (request.comparisonSlot != processor.getActiveComparisonSlot()) {
        return;
    }
    if (request.operation == PresetOperation::load) {
        const bool unchanged = sameProcessingState(request.sound, processor.getSoundSnapshot());
        if (pendingLoadChanged || !unchanged || processor.getHistory().isEditing()) {
            showError("The sound changed while the preset was being read. Select the preset again to load it.");
            return;
        }
        if (!processor.loadSoundSnapshot(preset.snapshot, "Load preset " + preset.name)) {
            showError("The preset could not be applied while a processing edit is active.");
            return;
        }
    } else if (request.operation != PresetOperation::saveAs) {
        const auto selected = processor.getSelectedUserPreset();
        if (!selected.has_value() || !request.selection.has_value()) {
            return;
        }
        if (selected->id != request.selection->id || selected->fingerprint != request.selection->fingerprint) {
            return;
        }
    }
    // Recall notifies the host synchronously. Recheck after any reentrant restore or slot change.
    if (request.generation == processor.getHistory().generation()
        && request.comparisonSlot == processor.getActiveComparisonSlot())
    {
        processor.setSelectedUserPreset(std::move(preset));
    }
}

/// Applies current-generation results. A slow load never overwrites edits, slot changes, or a host state restore.
void ProcessingToolbarContent::acceptCompletion(PresetCompletion completion)
{
    if (completion.listing.has_value()) {
        applyListing(std::move(*completion.listing));
    }
    listingError = completion.listingError;
    if (completion.error.isNotEmpty()) {
        if (completion.request.operation == PresetOperation::list) {
            // Background scans expose failures through status, never an unsolicited modal during editor creation.
            listingError = completion.error;
        } else {
            showError(completion.error);
        }
        return;
    }
    if (completion.request.generation != processor.getHistory().generation()) {
        return;
    }
    const auto operation = completion.request.operation;
    if (operation == PresetOperation::remove) {
        const bool sameSlot = completion.request.comparisonSlot == processor.getActiveComparisonSlot();
        const auto selected = processor.getSelectedUserPreset();
        const bool sameSelection = selected.has_value() && completion.request.selection.has_value()
            && selected->id == completion.request.selection->id;
        if (sameSlot && sameSelection) {
            processor.setSelectedUserPreset(std::nullopt);
        }
    } else if (completion.preset.has_value()) {
        acceptPresetResult(completion.request, std::move(*completion.preset));
    }
    if (completion.listingError.isNotEmpty() && operation != PresetOperation::list) {
        showError("The preset list could not be refreshed. " + completion.listingError);
    }
}

/// Resolves the selector's stable, displayed list before queuing a validated preset load.
void ProcessingToolbarContent::loadSelectedPreset()
{
    const int index = presetSelector.getSelectedId() - 1;
    shownPresetText.reset();
    if (!canInteract() || index < 0 || static_cast<std::size_t>(index) >= presets.size()) {
        updatePresetSelector();
        return;
    }
    auto request = makeRequest(PresetOperation::load);
    request.selection = presets.at(static_cast<std::size_t>(index));
    queueRequest(std::move(request));
    refresh();
}

/// Opens an asynchronous actions menu, capturing neither raw component pointers nor mutable library indexes.
void ProcessingToolbarContent::showPresetMenu()
{
    if (!canInteract()) {
        return;
    }
    const bool selected = processor.getSelectedUserPreset().has_value();
    juce::PopupMenu menu;
    menu.setLookAndFeel(&dialogLookAndFeel);
    menu.addItem(static_cast<int>(PresetMenuAction::saveAs), "Save As...");
    menu.addItem(static_cast<int>(PresetMenuAction::update), "Update selected preset", selected);
    menu.addSeparator();
    menu.addItem(static_cast<int>(PresetMenuAction::rename), "Rename...", selected);
    menu.addItem(static_cast<int>(PresetMenuAction::remove), "Delete...", selected);
    menu.addSeparator();
    menu.addItem(static_cast<int>(PresetMenuAction::importPreset), "Import...");
    menu.addItem(static_cast<int>(PresetMenuAction::exportPreset), "Export...", selected);
    menu.addSeparator();
    menu.addItem(static_cast<int>(PresetMenuAction::refresh), "Refresh preset list");
    if (!issues.empty() || listingError.isNotEmpty()) {
        menu.addItem(static_cast<int>(PresetMenuAction::issues), "Preset file errors...");
    }
    menuOpen = true;
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options {}.withTargetComponent(&presetMenuButton).withDeletionCheck(*this),
        [safeThis](const int result) {
            if (safeThis == nullptr || safeThis->closing) {
                return;
            }
            safeThis->menuOpen = false;
            safeThis->handleMenuAction(result);
            safeThis->refresh();
        }
    );
}

/// Routes explicit user commands, never inferring a destructive action from a preset name alone.
void ProcessingToolbarContent::handleMenuAction(const int action)
{
    if (!canInteract() || action == 0) {
        return;
    }
    switch (static_cast<PresetMenuAction>(action)) {
        case PresetMenuAction::saveAs:
            promptName(false);
            break;
        case PresetMenuAction::update:
            if (processor.getSelectedUserPreset().has_value()) {
                queueRequest(makeRequest(PresetOperation::update));
            }
            break;
        case PresetMenuAction::rename:
            promptName(true);
            break;
        case PresetMenuAction::remove:
            confirmDelete();
            break;
        case PresetMenuAction::importPreset:
            chooseImport();
            break;
        case PresetMenuAction::exportPreset:
            chooseExport();
            break;
        case PresetMenuAction::refresh:
            queueRequest(makeRequest(PresetOperation::list));
            break;
        case PresetMenuAction::issues:
            showError(listingError.isNotEmpty() ? listingError : describeIssues(issues));
            break;
    }
}

/// Prompts for a validated name, capturing the sound and selection before the asynchronous dialog opens.
void ProcessingToolbarContent::promptName(const bool renaming)
{
    auto request = makeRequest(renaming ? PresetOperation::rename : PresetOperation::saveAs);
    if (renaming && !request.selection.has_value()) {
        return;
    }
    dialog = std::make_unique<juce::AlertWindow>(
        renaming ? "Rename Preset" : "Save Preset As",
        renaming ? "Choose a new name for the selected user preset."
                 : "Save the current sound as a new user preset. Existing presets are never replaced by Save As.",
        juce::MessageBoxIconType::NoIcon,
        this
    );
    dialog->setLookAndFeel(&dialogLookAndFeel);
    dialog->addTextEditor(
        NAME_FIELD, request.selection.has_value() ? request.selection->name : juce::String(), "Preset name:"
    );
    dialog->addButton(renaming ? "Rename" : "Save", ACCEPT_RESULT, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", CANCEL_RESULT, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    const juce::Component::SafePointer<juce::AlertWindow> safeDialog(dialog.get());
    dialog->enterModalState(
        true, juce::ModalCallbackFunction::create([safeThis, safeDialog, request](const int result) mutable {
            if (safeThis == nullptr || safeThis->closing || safeDialog == nullptr) {
                return;
            }
            const auto name = safeDialog->getTextEditorContents(NAME_FIELD).trim();
            safeThis->dialog.reset();
            if (result == ACCEPT_RESULT) {
                const auto validation = UserPresets::validateName(name);
                if (validation.failed()) {
                    safeThis->showError(validation.getErrorMessage());
                } else {
                    request.name = name;
                    safeThis->queueRequest(std::move(request));
                }
            }
            safeThis->refresh();
        })
    );
}

/// Shows a member-owned asynchronous dialog. Closing the editor destroys it without a dangling callback owner.
void ProcessingToolbarContent::showDialog(
    const juce::String& title,
    const juce::String& message,
    const juce::MessageBoxIconType icon,
    const juce::String& acceptText,
    const std::function<void()>& accepted
)
{
    if (closing || dialog != nullptr) {
        return;
    }
    const bool confirmation = static_cast<bool>(accepted);
    dialog = std::make_unique<juce::AlertWindow>(title, message, icon, this);
    dialog->setLookAndFeel(&dialogLookAndFeel);
    dialog->addButton(
        acceptText,
        ACCEPT_RESULT,
        confirmation ? juce::KeyPress() : juce::KeyPress(juce::KeyPress::returnKey),
        confirmation ? juce::KeyPress() : juce::KeyPress(juce::KeyPress::escapeKey)
    );
    if (confirmation) {
        dialog->addButton(
            "Cancel",
            CANCEL_RESULT,
            juce::KeyPress(juce::KeyPress::escapeKey),
            juce::KeyPress(juce::KeyPress::returnKey)
        );
    }
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    // Capture a copy. The caller-owned const reference can expire before the asynchronous dialog is dismissed.
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, accepted](const int result) {
                                if (safeThis == nullptr || safeThis->closing) {
                                    return;
                                }
                                safeThis->dialog.reset();
                                if (result == ACCEPT_RESULT && accepted) {
                                    accepted();
                                }
                                safeThis->refresh();
                            }));
}

/// Reports storage and validation failures without claiming that the user's sound was modified.
void ProcessingToolbarContent::showError(const juce::String& message)
{
    showDialog("ImageControl Presets", message, juce::MessageBoxIconType::WarningIcon, "OK", {});
}

/// Confirms deletion of the exact captured library revision, with Cancel as the Return/Escape action.
void ProcessingToolbarContent::confirmDelete()
{
    auto request = makeRequest(PresetOperation::remove);
    if (!request.selection.has_value()) {
        return;
    }
    const auto message = "Delete " + request.selection->name.quoted()
        + " from the user preset library? "
          "This cannot be undone. The current sound will not change.";
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    showDialog("Delete Preset", message, juce::MessageBoxIconType::WarningIcon, "Delete", [safeThis, request] mutable {
        if (safeThis != nullptr) {
            safeThis->queueRequest(std::move(request));
        }
    });
}

/// Lets the host's native asynchronous chooser select one XML file, then imports and validates it on the worker.
void ProcessingToolbarContent::chooseImport()
{
    auto request = makeRequest(PresetOperation::importPreset);
    fileChooser
        = std::make_unique<juce::FileChooser>("Import ImageControl Preset", juce::File(), "*.xml", true, false, this);
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    fileChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, request](const juce::FileChooser& chooser) mutable {
            if (safeThis == nullptr || safeThis->closing) {
                return;
            }
            const auto file = chooser.getResult();
            safeThis->fileChooser.reset();
            if (file != juce::File()) {
                request.file = file;
                safeThis->queueRequest(std::move(request));
            }
            safeThis->refresh();
        }
    );
}

/// Exports the captured saved revision, not unsaved edits. The library rejects existing destinations.
void ProcessingToolbarContent::chooseExport()
{
    auto request = makeRequest(PresetOperation::exportPreset);
    if (!request.selection.has_value()) {
        return;
    }
    fileChooser = std::make_unique<juce::FileChooser>(
        "Export Saved Preset to a New File", request.selection->file, "*.xml", true, false, this
    );
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    fileChooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, request](const juce::FileChooser& chooser) mutable {
            if (safeThis == nullptr || safeThis->closing) {
                return;
            }
            const auto file = chooser.getResult();
            safeThis->fileChooser.reset();
            if (file != juce::File()) {
                request.file = file;
                safeThis->queueRequest(std::move(request));
            }
            safeThis->refresh();
        }
    );
}

/// Creates the processor-backed implementation without adding another refresh timer to the editor.
ProcessingToolbar::ProcessingToolbar(ImageControlProcessor& processor) :
    ProcessingToolbar(processor, UserPresets::defaultDirectory())
{}

/// Uses explicit library storage for isolated workflows without changing the production directory.
ProcessingToolbar::ProcessingToolbar(ImageControlProcessor& processor, const juce::File& presetDirectory) :
    content(std::make_unique<ProcessingToolbarContent>(processor, presetDirectory))
{
    setComponentID("processing-toolbar");
    setTitle("Processing toolbar");
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    addAndMakeVisible(*content);
}

/// Releases dialogs and finishes independent disk operations before the processor reference can become invalid.
ProcessingToolbar::~ProcessingToolbar() = default;

/// Routes header and keyboard redo through the pending-preset-load guard on the message thread.
void ProcessingToolbar::redo()
{
    content->redo();
}

/// Routes header and keyboard undo through the pending-preset-load guard on the message thread.
void ProcessingToolbar::undo()
{
    content->undo();
}

/// Called by the editor's VBlank callback on the message thread, including while this row is collapsed.
void ProcessingToolbar::refresh()
{
    content->refresh();
}

/// Keeps the implementation aligned with the dedicated processing row.
void ProcessingToolbar::resized()
{
    content->setBounds(getLocalBounds());
}
}  // namespace esgrove::image_control
