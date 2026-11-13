/// Implements A/B controls, inline preset naming, modified markers, and library feedback.
/// Filesystem workflows live in ProcessingToolbarPresets.cpp and publish detached worker completions.

#include "ui/ProcessingToolbar.h"

#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "ProcessingState.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/ProcessingToolbarContent.h"
#include "UserPresets.h"
#include <initializer_list>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace esgrove::image_control
{
namespace
{
/// Vertical breathing room matches the compact header actions.
constexpr int CONTROL_INSET {4};
/// Keep the selector's dropdown arrow available beside its inline editor.
constexpr int PRESET_ARROW_WIDTH {30};
constexpr float PRESET_NAME_FONT_HEIGHT {13.0F};
/// Stable left-to-right traversal is independent of the controls' dependency-safe declaration order.
constexpr int SLOT_A_FOCUS_ORDER {1};
constexpr int SLOT_B_FOCUS_ORDER {2};
constexpr int COPY_FOCUS_ORDER {3};
constexpr int PRESET_SELECTOR_FOCUS_ORDER {4};
constexpr int PRESET_MENU_FOCUS_ORDER {5};

/// Assigns stable test and screen-reader identities with explicit left-to-right keyboard traversal.
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

/// Keeps naming local to the selector rather than using a modal JUCE label editor.
PresetNameSelector::PresetNameSelector()
{
    nameEditor.setComponentID("preset-name-input");
    nameEditor.setTitle("Preset name");
    nameEditor.setMultiLine(false);
    nameEditor.setFont(textFont(PRESET_NAME_FONT_HEIGHT));
    nameEditor.onTextChange = [this] {
        if (callbacks.change) {
            callbacks.change(nameEditor.getText());
        }
    };
    nameEditor.onReturnKey = [this] {
        if (callbacks.change) {
            callbacks.change(nameEditor.getText());
        }
        if (callbacks.accept) {
            callbacks.accept();
        }
    };
    nameEditor.onEscapeKey = [this] {
        if (callbacks.cancel) {
            callbacks.cancel();
        }
    };
    addChildComponent(nameEditor);
}

/// Installs the containing toolbar's lifetime-bounded message-thread actions.
void PresetNameSelector::setNameCallbacks(PresetNameCallbacks actions)
{
    callbacks = std::move(actions);
}

/// Starts or resumes raw name input without changing the ComboBox's library selection.
void PresetNameSelector::beginNameEdit(const juce::String& name)
{
    nameEditor.setText(name, false);
    nameEditor.setVisible(true);
    nameEditor.toFront(false);
    resized();
    if (isShowing()) {
        nameEditor.grabKeyboardFocus();
    }
    nameEditor.selectAll();
}

/// Ends inline input without an asynchronous ComboBox text-change notification.
void PresetNameSelector::endNameEdit()
{
    nameEditor.setVisible(false);
}

/// Makes inline validation and Save As instructions reachable while the input covers the selector.
void PresetNameSelector::setNameFeedback(const juce::String& feedback)
{
    nameEditor.setDescription(feedback);
    nameEditor.setTooltip(feedback);
}

/// Reads input synchronously so Save cannot race a pending TextEditor notification.
juce::String PresetNameSelector::getNameText() const
{
    return nameEditor.getText();
}

/// Reports whether the overlay owns the raw name draft.
bool PresetNameSelector::isNameEditing() const
{
    return nameEditor.isVisible();
}

/// F2 enters naming while arrow keys and Return retain ordinary preset-picker navigation.
bool PresetNameSelector::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::F2Key && callbacks.edit) {
        callbacks.edit();
        return true;
    }
    return juce::ComboBox::keyPressed(key);
}

/// Clicking the name edits it, while the dropdown arrow continues to open the preset list.
void PresetNameSelector::mouseDown(const juce::MouseEvent& event)
{
    const auto local = event.getEventRelativeTo(this);
    if (event.originalComponent != &nameEditor && !nameEditor.isParentOf(event.originalComponent)
        && local.x < getWidth() - PRESET_ARROW_WIDTH && event.mods.isLeftButtonDown() && callbacks.edit)
    {
        callbacks.edit();
        return;
    }
    if (event.eventComponent == this) {
        juce::ComboBox::mouseDown(event);
    }
}

/// Preserves input focus and caret while the toolbar moves between header and second row.
void PresetNameSelector::resized()
{
    juce::ComboBox::resized();
    nameEditor.setBounds(getLocalBounds().reduced(1).withTrimmedRight(PRESET_ARROW_WIDTH));
}

/// Installs controls in keyboard order and starts a non-blocking initial scan without reconnecting restored state.
ProcessingToolbarContent::ProcessingToolbarContent(
    ImageControlProcessor& owner,
    const juce::File& presetDirectory,
    const PresetDialogPresentation dialogs
) :
    processor(owner),
    library(owner.getParameters(), presetDirectory),
    dialogPresentation(dialogs)
{
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    setTitle("A/B comparison and presets");
    identifyControl(slotAButton, "comparison-a", "Comparison slot A", SLOT_A_FOCUS_ORDER);
    identifyControl(slotBButton, "comparison-b", "Comparison slot B", SLOT_B_FOCUS_ORDER);
    identifyControl(copyButton, "comparison-copy", "Copy active comparison to the other slot", COPY_FOCUS_ORDER);
    identifyControl(presetSelector, "preset-selector", "User preset", PRESET_SELECTOR_FOCUS_ORDER);
    identifyControl(presetMenuButton, "preset-menu", "Preset actions", PRESET_MENU_FOCUS_ORDER);
    slotAButton.setConnectedEdges(juce::Button::ConnectedOnRight);
    slotBButton.setConnectedEdges(juce::Button::ConnectedOnLeft);
    slotAButton.setToggleable(true);
    slotBButton.setToggleable(true);
    slotAButton.setTooltip("Recall processing slot A. Display settings and audition controls are unchanged");
    slotBButton.setTooltip("Recall processing slot B. Display settings and audition controls are unchanged");
    presetSelector.setEditableText(false);
    presetSelector.setTextWhenNothingSelected("Unsaved");
    presetSelector.setNameCallbacks({
        .edit = [this] { beginNameEdit(false); },
        .accept = [this] { commitNameEdit(); },
        .cancel = [this] { cancelNameEdit(); },
        .change =
            [this](const juce::String& name) {
                if (!nameEditContext || busy) {
                    return;
                }
                draftName = name;
                nameError.clear();
                updatePresetSelector();
            },
    });
    presetSelector.setTextWhenNoChoicesAvailable("No user presets. Choose Save As from Presets");
    presetMenuButton.setTooltip("Save, Save As, Rename, Delete, Import, Export, and Reconnect presets");
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

/// Captures sound, persistent provenance, and the transient file revision before asynchronous work begins.
PresetRequest ProcessingToolbarContent::makeRequest(const PresetOperation operation) const
{
    PresetRequest request;
    request.operation = operation;
    const auto presentation = processor.getPresetPresentation();
    request.generation = presentation.generation;
    request.selection = presentation.selection;
    request.binding = presentation.selection;
    request.sound = presentation.sound;
    request.comparisonSlot = presentation.comparisonSlot;
    if (presentation.provenance) {
        request.provenanceId = presentation.provenance->id;
        request.provenanceName = presentation.provenance->name;
        request.baseline = presentation.provenance->baseline;
    }
    return request;
}

/// Rejects a completion after host restore, slot change, or replacement of the persistent baseline or file binding.
bool ProcessingToolbarContent::requestIsCurrent(const PresetRequest& request) const
{
    return requestMatchesPresentation(request, processor.getPresetPresentation());
}

/// Checks a captured request against an already fetched active-slot presentation snapshot.
bool ProcessingToolbarContent::requestMatchesPresentation(
    const PresetRequest& request,
    const PresetPresentation& presentation
)
{
    if (request.generation != presentation.generation || request.comparisonSlot != presentation.comparisonSlot) {
        return false;
    }
    const auto& provenance = presentation.provenance;
    if (provenance.has_value() != request.baseline.has_value()) {
        return false;
    }
    if (provenance
        && (provenance->id != request.provenanceId || provenance->name != request.provenanceName
            || !sameProcessingState(provenance->baseline, *request.baseline)))
    {
        return false;
    }
    const auto& selected = presentation.selection;
    return selected.has_value() == request.binding.has_value()
        && (!selected
            || (selected->id == request.binding->id && selected->file == request.binding->file
                && selected->fingerprint == request.binding->fingerprint));
}

/// Packs the selector directly beside its actions using the same dimensions as header admission.
void ProcessingToolbarContent::resized()
{
    constexpr int controlHeight {24};
    const int inset = std::min(CONTROL_INSET, std::max(0, (getHeight() - controlHeight) / 2));
    auto area = getLocalBounds().reduced(0, inset);
    slotAButton.setBounds(area.removeFromLeft(PROCESSING_TOOLBAR_SLOT_WIDTH));
    slotBButton.setBounds(area.removeFromLeft(PROCESSING_TOOLBAR_SLOT_WIDTH));
    area.removeFromLeft(PROCESSING_TOOLBAR_CONTROL_GAP);
    copyButton.setBounds(area.removeFromLeft(PROCESSING_TOOLBAR_COPY_WIDTH));
    area.removeFromLeft(PROCESSING_TOOLBAR_GROUP_GAP);
    presetSelector.setBounds(area.removeFromLeft(
        std::clamp(
            area.getWidth() - PROCESSING_TOOLBAR_MENU_WIDTH - PROCESSING_TOOLBAR_CONTROL_GAP,
            PROCESSING_TOOLBAR_MIN_SELECTOR_WIDTH,
            PROCESSING_TOOLBAR_MAX_SELECTOR_WIDTH
        )
    ));
    area.removeFromLeft(PROCESSING_TOOLBAR_CONTROL_GAP);
    presetMenuButton.setBounds(area.removeFromLeft(PROCESSING_TOOLBAR_MENU_WIDTH));
}

/// Replays history and invalidates a pending load even when another action restores the same sound.
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

/// Consumes worker values and refreshes A/B, persistent identity, sound status, and separate library diagnostics.
void ProcessingToolbarContent::refresh()
{
    observePendingLoad();
    std::optional<PresetCompletion> completion;
    if (busy) {
        const std::scoped_lock lock(mailbox->mutex);
        completion = std::move(mailbox->completion);
        mailbox->completion.reset();
    }
    if (!completion && deferredCompletion) {
        completion = std::move(deferredCompletion);
        deferredCompletion.reset();
    }
    if (completion) {
        const auto operation = completion->request.operation;
        const bool publishesMetadata = operation == PresetOperation::saveAs || operation == PresetOperation::update
            || operation == PresetOperation::rename || operation == PresetOperation::remove
            || operation == PresetOperation::reconnect;
        const auto& localHistory = processor.getHistory();
        if (publishesMetadata && completion->error.isEmpty() && requestIsCurrent(completion->request)
            && (localHistory.isEditing() || localHistory.isReplaying()))
        {
            deferredCompletion = std::move(completion);
        } else {
            busy = false;
            acceptCompletion(std::move(*completion));
            pendingLoadSound.reset();
        }
    }
    const auto presentation = processor.getPresetPresentation();
    if (nameEditContext && !requestMatchesPresentation(*nameEditContext, presentation)) {
        cancelNameEdit();
    }
    const auto& history = processor.getHistory();
    const bool editing = history.isEditing();
    const bool slotA = presentation.comparisonSlot == 0;
    // Accessibility can toggle the active button without changing the processor's selection.
    slotAButton.setToggleState(slotA, juce::dontSendNotification);
    slotBButton.setToggleState(!slotA, juce::dontSendNotification);
    if (shownSlotA != slotA) {
        shownSlotA = slotA;
        copyButton.setButtonText(juce::String::fromUTF8(slotA ? "A \xe2\x86\x92 B" : "B \xe2\x86\x92 A"));
        copyButton.setTitle(slotA ? "Copy A to B" : "Copy B to A");
        copyButton.setTooltip(
            slotA ? "Replace only slot B's sound with A. Preset identities and baselines are unchanged"
                  : "Replace only slot A's sound with B. Preset identities and baselines are unchanged"
        );
    }
    const bool slotEnabled = !editing;
    if (slotAButton.isEnabled() != slotEnabled) {
        slotAButton.setEnabled(slotEnabled);
    }
    if (slotBButton.isEnabled() != slotEnabled) {
        slotBButton.setEnabled(slotEnabled);
    }
    if (copyButton.isEnabled() != slotEnabled) {
        copyButton.setEnabled(slotEnabled);
    }
    const bool interactive = canInteract();
    if (presetSelector.isEnabled() != interactive) {
        presetSelector.setEnabled(interactive);
    }
    if (presetMenuButton.isEnabled() != interactive) {
        presetMenuButton.setEnabled(interactive);
    }
    updatePresetSelector(presentation);
}

/// Captures the active-slot presentation and updates the preset selector.
void ProcessingToolbarContent::updatePresetSelector()
{
    updatePresetSelector(processor.getPresetPresentation());
}

/// Presents persistent name and dirty state without silently adopting any revision from the latest library scan.
void ProcessingToolbarContent::updatePresetSelector(const PresetPresentation& presentation)
{
    const auto& provenance = presentation.provenance;
    const bool modified = presentation.modified || (nameEditContext && draftName != nameEditContext->provenanceName);
    auto name = provenance ? provenance->name : juce::String("Unsaved");
    if (nameEditContext) {
        name = draftName;
    }
    const auto displayName = name + (modified && provenance ? " *" : "");
    // Preserve a newly chosen ComboBox item until its asynchronous onChange callback is delivered.
    if (!shownPresetText || *shownPresetText != displayName) {
        presetSelector.setText(displayName, juce::dontSendNotification);
        shownPresetText = displayName;
    }
    juce::String soundDescription = "The active sound matches the stored baseline for " + name.quoted();
    if (!provenance) {
        soundDescription = "The active sound has no saved preset baseline. Use Save As.";
    } else if (modified) {
        soundDescription = "The active sound differs from the stored baseline for " + name.quoted();
    }
    const auto title = "User preset: " + name;
    if (shownSelectorTitle != title) {
        shownSelectorTitle = title;
        presetSelector.setTitle(title);
    }
    const auto feedback = libraryFeedback(presentation);
    const auto selectorTooltip = name + juce::newLine + soundDescription + juce::newLine + feedback + juce::newLine
        + "Click the name to edit. Use the arrow to load a preset.";
    if (shownSelectorTooltip != selectorTooltip) {
        shownSelectorTooltip = selectorTooltip;
        presetSelector.setNameFeedback(feedback);
        presetSelector.setTooltip(selectorTooltip);
    }
    const auto menuTooltip = "Save and manage presets" + juce::newLine + feedback;
    if (shownMenuTooltip != menuTooltip) {
        shownMenuTooltip = menuTooltip;
        presetMenuButton.setTooltip(menuTooltip);
    }
    const auto fullDescription = soundDescription + juce::newLine + feedback;
    if (shownSelectorDescription != fullDescription) {
        shownSelectorDescription = fullDescription;
        presetSelector.setDescription(fullDescription);
    }
    const bool hasError = nameError.isNotEmpty();
    if (shownSelectorError != hasError) {
        shownSelectorError = hasError;
        presetSelector.setColour(
            juce::ComboBox::outlineColourId, juce::Colour(hasError ? palette::YELLOW : palette::GREY_MEDIUM)
        );
    }
}

/// Keeps operation progress, naming feedback, and library diagnostics separate from the modified marker.
juce::String ProcessingToolbarContent::libraryFeedback(const PresetPresentation& presentation) const
{
    if (nameError.isNotEmpty()) {
        return nameError;
    }
    if (busy) {
        return pendingOperation;
    }
    if (savingAs) {
        return "Enter a new preset name. Press Enter to save, or Escape to cancel.";
    }
    if (listingError.isNotEmpty()) {
        return "Scan failed: " + listingError;
    }
    if (!issues.empty()) {
        return juce::String(static_cast<int>(issues.size()))
            + " invalid: Some preset files were rejected. Choose Presets > Preset file errors for details.";
    }
    const auto& selection = presentation.selection;
    if (presentation.provenance && !selection) {
        return "Unlinked: Choose Reconnect to link the saved preset, or Save As to create a new preset.";
    }
    if (selection) {
        const auto candidate = std::ranges::find(presets, selection->id, &UserPreset::id);
        if (candidate == presets.end()) {
            return "Missing file: The preset file is unavailable. Restore it, choose Reconnect, or use Save As.";
        }
        if (candidate->file != selection->file || candidate->fingerprint != selection->fingerprint) {
            return "File changed: The preset file changed. Choose Reconnect before saving changes.";
        }
    }
    return "The preset library is available. Scans never change the saved sound baseline.";
}

/// Reads a raw draft synchronously without deriving identity from the displayed ComboBox text.
juce::String ProcessingToolbarContent::currentPresetName() const
{
    if (nameEditContext) {
        return presetSelector.getNameText();
    }
    const auto provenance = processor.getPresetProvenance();
    return provenance ? provenance->name : juce::String();
}

/// Remembers observed edits and transitions even when an undo or A/B round trip restores the captured sound.
void ProcessingToolbarContent::observePendingLoad()
{
    if (!pendingLoadSound || pendingLoadChanged) {
        return;
    }
    pendingLoadChanged = processor.getHistory().isEditing() || processor.getHistory().isReplaying()
        || pendingLoadSlot != processor.getActiveComparisonSlot()
        || !sameProcessingState(*pendingLoadSound, processor.getSoundSnapshot());
}

/// Creates the processor-backed implementation without another editor refresh timer.
ProcessingToolbar::ProcessingToolbar(ImageControlProcessor& processor) :
    ProcessingToolbar(processor, UserPresets::defaultDirectory())
{}

/// Uses explicit library storage for isolated workflows without changing the production directory.
ProcessingToolbar::ProcessingToolbar(
    ImageControlProcessor& processor,
    const juce::File& presetDirectory,
    const PresetDialogPresentation dialogs
) :
    content(std::make_unique<ProcessingToolbarContent>(processor, presetDirectory, dialogs))
{
    setComponentID("processing-toolbar");
    setTitle("Processing toolbar");
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    addAndMakeVisible(*content);
}

/// Releases dialogs and finishes disk work before the processor reference can become invalid.
ProcessingToolbar::~ProcessingToolbar() = default;

/// Enters the implementation's nested focus container rather than falling back to a header sibling.
void ProcessingToolbar::focusControls()
{
    if (const auto traverser = content->createKeyboardFocusTraverser()) {
        if (auto* first = traverser->getDefaultComponent(content.get())) {
            first->grabKeyboardFocus();
        }
    }
}

/// Routes header and keyboard redo through the pending-load guard on the message thread.
void ProcessingToolbar::redo()
{
    content->redo();
}

/// Routes header and keyboard undo through the pending-load guard on the message thread.
void ProcessingToolbar::undo()
{
    content->undo();
}

/// Runs on the editor's existing message-thread refresh, including while this row is collapsed.
void ProcessingToolbar::refresh()
{
    content->refresh();
}

/// Keeps the private implementation aligned with the dedicated processing row.
void ProcessingToolbar::resized()
{
    content->setBounds(getLocalBounds());
}
}  // namespace esgrove::image_control
