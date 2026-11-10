/// Implements A/B controls and independent sound identity, modified status, and library-status presentation.
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
/// Compact controls preserve a flexible preset selector at the editor's 760-pixel minimum width.
constexpr int CONTROL_INSET {4};
constexpr int CONTROL_GAP {4};
constexpr int GROUP_GAP {12};
constexpr int SLOT_WIDTH {32};
constexpr int COPY_WIDTH {72};
constexpr int MENU_WIDTH {72};
constexpr int STATUS_WIDTH {64};
constexpr int LIBRARY_STATUS_WIDTH {84};
/// Status text remains legible without competing with the compact preset selector.
constexpr float STATUS_FONT_HEIGHT {12.0F};
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

/// Configures a readable status label that can actually receive tooltip hit testing.
void configureStatus(juce::Label& status, const juce::String& identifier, const juce::String& title)
{
    status.setComponentID(identifier);
    status.setTitle(title);
    status.setFont(textFont(STATUS_FONT_HEIGHT));
    status.setJustificationType(juce::Justification::centredRight);
    status.setInterceptsMouseClicks(true, false);
}
}  // namespace

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
    configureStatus(presetStatus, "preset-status", "Preset sound status");
    configureStatus(libraryStatus, "preset-library-status", "Preset library status");
    slotAButton.setConnectedEdges(juce::Button::ConnectedOnRight);
    slotBButton.setConnectedEdges(juce::Button::ConnectedOnLeft);
    slotAButton.setToggleable(true);
    slotBButton.setToggleable(true);
    slotAButton.setTooltip("Recall processing slot A. Display settings and audition controls are unchanged");
    slotBButton.setTooltip("Recall processing slot B. Display settings and audition controls are unchanged");
    presetSelector.setEditableText(false);
    presetSelector.setTextWhenNothingSelected("Unsaved sound");
    presetSelector.setTextWhenNoChoicesAvailable("No user presets. Choose Save As from Presets");
    presetMenuButton.setTooltip(
        "Save As, confirm Update, reconnect a restored preset, Rename, Delete, Import to Library, and Export Saved "
        "Preset"
    );
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
             &libraryStatus,
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
    const auto presentation = processor.getPresetPresentation();
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

/// Keeps independently hit-testable sound and library statuses next to the flexible name-only selector.
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
    libraryStatus.setBounds(area.removeFromRight(LIBRARY_STATUS_WIDTH));
    area.removeFromRight(CONTROL_GAP);
    presetStatus.setBounds(area.removeFromRight(STATUS_WIDTH));
    area.removeFromRight(CONTROL_GAP);
    presetSelector.setBounds(area);
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
    {
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
    const auto& history = processor.getHistory();
    const bool slotA = processor.getActiveComparisonSlot() == 0;
    slotAButton.setToggleState(slotA, juce::dontSendNotification);
    slotBButton.setToggleState(!slotA, juce::dontSendNotification);
    slotAButton.setEnabled(!history.isEditing());
    slotBButton.setEnabled(!history.isEditing());
    copyButton.setEnabled(!history.isEditing());
    copyButton.setButtonText(juce::String::fromUTF8(slotA ? "A \xe2\x86\x92 B" : "B \xe2\x86\x92 A"));
    copyButton.setTitle(slotA ? "Copy A to B" : "Copy B to A");
    copyButton.setTooltip(
        slotA ? "Replace only slot B's sound with A. Preset identities and baselines are unchanged"
              : "Replace only slot A's sound with B. Preset identities and baselines are unchanged"
    );
    presetSelector.setEnabled(canInteract());
    presetMenuButton.setEnabled(canInteract());
    updatePresetSelector();
}

/// Presents persistent name and dirty state without silently adopting any revision from the latest library scan.
void ProcessingToolbarContent::updatePresetSelector()
{
    const auto presentation = processor.getPresetPresentation();
    const auto& provenance = presentation.provenance;
    const auto& selection = presentation.selection;
    const bool modified = presentation.modified;
    const auto name = provenance ? provenance->name : juce::String("Unsaved sound");
    // Preserve a newly chosen ComboBox item until its asynchronous onChange callback is delivered.
    if (!shownPresetText || *shownPresetText != name) {
        presetSelector.setText(name, juce::dontSendNotification);
        shownPresetText = name;
    }
    juce::String soundStatus {"Saved"};
    juce::String soundDescription = "The active sound matches the stored baseline for " + name.quoted();
    if (!provenance) {
        soundStatus = "Unsaved";
        soundDescription = "The active sound has no saved preset baseline. Use Save As.";
    } else if (modified) {
        soundStatus = "Modified";
        soundDescription = "The active sound differs from the stored baseline for " + name.quoted();
    }
    presetSelector.setTitle("User preset: " + name);
    presetSelector.setDescription(soundDescription);
    presetSelector.setTooltip(
        name + juce::newLine + soundDescription + juce::newLine
        + (provenance ? "Preset UUID: " + provenance->id + juce::newLine : juce::String())
        + "Choose a library preset to load its sound into the active slot."
    );
    presetStatus.setText(soundStatus, juce::dontSendNotification);
    presetStatus.setColour(
        juce::Label::textColourId, juce::Colour(provenance && !modified ? palette::GREY_MIDDLE_LIGHT : palette::YELLOW)
    );
    presetStatus.setTooltip(soundDescription);
    presetStatus.setDescription(soundDescription);

    juce::String status;
    juce::String explanation;
    if (busy) {
        status = pendingOperation;
        explanation = pendingOperation + " The preset sound status remains independent.";
    } else if (listingError.isNotEmpty()) {
        status = "Scan failed";
        explanation = listingError;
    } else if (!issues.empty()) {
        status = juce::String(static_cast<int>(issues.size())) + " invalid";
        explanation = "Some preset files were rejected. Choose Presets > Preset file errors for details.";
    } else if (provenance && !selection) {
        status = "Unlinked";
        explanation = "The stored preset name and baseline are retained independently of the library. "
                      "Choose Reconnect by UUID in Presets to bind the current saved revision without loading audio, "
                      "or Save As to create a new preset.";
    } else if (selection) {
        const auto candidate = std::ranges::find(presets, selection->id, &UserPreset::id);
        if (candidate == presets.end()) {
            status = "Missing file";
            explanation = "The bound library file is unavailable or invalid. The stored preset name and baseline "
                          "are retained. Restore the file, explicitly reconnect by UUID, or use Save As.";
        } else if (candidate->file != selection->file || candidate->fingerprint != selection->fingerprint) {
            status = "File changed";
            explanation = "The library revision changed. The bound fingerprint and stored baseline are unchanged. "
                          "Explicitly reconnect by UUID to target the current revision without loading its sound.";
        } else {
            explanation = "The preset library is available. Scans never change the saved sound baseline.";
        }
    } else {
        explanation = "The preset library is available. Scans never change the saved sound baseline.";
    }
    libraryStatus.setText(status, juce::dontSendNotification);
    libraryStatus.setColour(juce::Label::textColourId, juce::Colour(busy ? palette::BLUE : palette::YELLOW));
    libraryStatus.setTooltip(explanation);
    libraryStatus.setDescription(explanation);
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
