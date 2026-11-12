/// Implements explicit preset-library workflows, revision-checked worker commands, and lifetime-safe dialogs.
/// Scans never mutate provenance. Reconnection binds a UUID-matched revision without recalling its sound.

#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "ProcessingState.h"
#include "ui/ProcessingToolbar.h"
#include "ui/ProcessingToolbarContent.h"
#include "UserPresets.h"

#include <algorithm>
#include <cstddef>
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
/// Cancel is the non-destructive default for confirmations, including Return and Escape.
constexpr int ACCEPT_RESULT {1};
constexpr int CANCEL_RESULT {0};
/// Rejected-file diagnostics remain bounded even for an unexpectedly large directory.
constexpr std::size_t MAX_VISIBLE_ISSUES {8};

/// Combines the chooser's open or save mode with file selection.
int fileChooserFlags(const juce::FileBrowserComponent::FileChooserFlags mode)
{
    return static_cast<int>(
        static_cast<unsigned int>(mode) | static_cast<unsigned int>(juce::FileBrowserComponent::canSelectFiles)
    );
}

/// Keeps non-rendering workflow alerts out of desktop activation as well as painting.
class PresetAlertWindow final : public juce::AlertWindow
{
public:
    /// Production alerts use ordinary JUCE modality. Tests keep alerts hidden without a desktop peer.
    PresetAlertWindow(
        const juce::String& title,
        const juce::String& message,
        const juce::MessageBoxIconType icon,
        juce::Component& owner,
        const PresetDialogPresentation dialogMode
    ) :
        AlertWindow(title, message, icon, &owner),
        presentation(dialogMode)
    {
        if (presentation == PresetDialogPresentation::nonRendering) {
            removeFromDesktop();
        }
    }

protected:
    /// JUCE's default top-level visibility callback activates the window, which a silent test must not do.
    void visibilityChanged() override
    {
        if (presentation == PresetDialogPresentation::visible) {
            juce::TopLevelWindow::visibilityChanged();
        }
    }

private:
    /// Per-instance presentation is never inherited from environment variables or another plugin instance.
    const PresetDialogPresentation presentation;
};

/// Names filesystem work for the selector and preset-menu tooltips.
juce::String describeProgress(const PresetOperation operation)
{
    switch (operation) {
        case PresetOperation::list:
            return "Scanning...";
        case PresetOperation::load:
            return "Loading...";
        case PresetOperation::reconnect:
            return "Checking...";
        case PresetOperation::saveAs:
            return "Saving...";
        case PresetOperation::update:
            return "Updating...";
        case PresetOperation::rename:
            return "Renaming...";
        case PresetOperation::remove:
            return "Deleting...";
        case PresetOperation::importPreset:
            return "Importing...";
        case PresetOperation::exportPreset:
            return "Exporting...";
    }
    return {};
}

/// Captures a library revision without losing its exact-byte conflict-check fingerprint.
void capturePresetResult(PresetCompletion& completion, std::expected<UserPreset, juce::String> result)
{
    if (result) {
        completion.preset = std::move(*result);
    } else {
        completion.error = result.error();
    }
}

/// Runs commands against their captured exact revision, never a lookup by display name.
void performSelectedPresetRequest(const UserPresets& library, const UserPreset& selection, PresetCompletion& completion)
{
    const auto& command = completion.request;
    switch (command.operation) {
        case PresetOperation::load:
        case PresetOperation::reconnect:
            capturePresetResult(completion, library.load(selection));
            break;
        case PresetOperation::update:
            capturePresetResult(completion, library.update(selection, command.name, command.sound));
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

/// Performs independent filesystem work and returns detached values without calling components or the processor.
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
            case PresetOperation::reconnect:
            case PresetOperation::update:
            case PresetOperation::rename:
            case PresetOperation::remove:
            case PresetOperation::exportPreset:
                if (command.selection) {
                    performSelectedPresetRequest(library, *command.selection, completion);
                } else {
                    completion.error = "Reconnect or select a library preset before choosing this action.";
                }
                break;
        }
        auto listing = library.list();
        if (listing) {
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

/// Describes malformed files without flooding the dialog with an unbounded directory listing.
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
}  // namespace

/// Serializes disk work after rejecting an asynchronously accepted command whose processor context changed.
void ProcessingToolbarContent::queueRequest(PresetRequest request)
{
    if (busy || closing) {
        return;
    }
    if (request.operation != PresetOperation::list && !requestIsCurrent(request)) {
        showError("The plugin state, comparison slot, or preset baseline changed. Please choose the action again.");
        return;
    }
    busy = true;
    pendingOperation = describeProgress(request.operation);
    pendingLoadChanged = false;
    if (request.operation == PresetOperation::load) {
        pendingLoadSlot = request.comparisonSlot;
        pendingLoadSound = request.sound;
    }
    const auto sharedMailbox = mailbox;
    // Destruction joins the pool before destroying library or its processor-owned parameter definitions.
    diskWorker.addJob([sharedMailbox, &presetLibrary = library, request] mutable {
        auto completion = performPresetRequest(presetLibrary, std::move(request));
        const std::scoped_lock lock(sharedMailbox->mutex);
        sharedMailbox->completion = std::move(completion);
    });
}

/// Rebuilds selectable files without replacing the processor's provenance or exact bound revision.
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

/// Publishes only current-context results. Loading atomically publishes provenance before host parameter callbacks.
void ProcessingToolbarContent::acceptPresetResult(const PresetRequest& request, UserPreset preset)
{
    if (request.operation == PresetOperation::importPreset || !requestIsCurrent(request)) {
        return;
    }
    if (request.operation == PresetOperation::load) {
        if (pendingLoadChanged || !sameProcessingState(request.sound, processor.getSoundSnapshot())
            || processor.getHistory().isEditing())
        {
            showError("The sound changed while the preset was being read. Select the preset again to load it.");
            return;
        }
        if (!processor.loadUserPreset(preset, request.generation, request.comparisonSlot)) {
            showError("The preset could not be applied while a processing edit is active.");
        }
        return;
    }
    if (request.operation == PresetOperation::reconnect) {
        // The worker has freshly read and fingerprint-checked the exact UUID revision accepted in the confirmation.
        if (!request.selection || preset.id != request.provenanceId || preset.id != request.selection->id
            || preset.file != request.selection->file || preset.fingerprint != request.selection->fingerprint)
        {
            return;
        }
        if (!processor.reconnectSelectedUserPreset(preset, request.generation, request.comparisonSlot)) {
            showError("The stored preset identity changed before the library revision could be reconnected.");
        }
        return;
    }
    if (request.operation == PresetOperation::rename) {
        // Rename atomically publishes the actual revision and new name without replacing the retained sound baseline.
        juce::ignoreUnused(processor.renameSelectedUserPreset(preset, request.generation, request.comparisonSlot));
        return;
    }
    if (request.operation == PresetOperation::saveAs || request.operation == PresetOperation::update) {
        cancelNameEdit();
        juce::ignoreUnused(
            processor.setSelectedUserPreset(std::move(preset), request.generation, request.comparisonSlot)
        );
    }
}

/// Retains library scan results but rejects stale sound, provenance, and binding publication after asynchronous work.
void ProcessingToolbarContent::acceptCompletion(PresetCompletion completion)
{
    if (completion.listing) {
        applyListing(std::move(*completion.listing));
    }
    listingError = completion.listingError;
    const auto& request = completion.request;
    if (request.operation != PresetOperation::list && !requestIsCurrent(request)) {
        return;
    }
    if (completion.error.isNotEmpty()) {
        if (request.operation == PresetOperation::list) {
            listingError = completion.error;
        } else if (request.operation == PresetOperation::saveAs || request.operation == PresetOperation::update) {
            nameError = completion.error;
            if (!nameEditContext) {
                nameEditContext = request;
                draftName = request.name;
                savingAs = request.operation == PresetOperation::saveAs;
            }
            presetSelector.beginNameEdit(draftName);
        } else {
            showError(completion.error);
        }
        return;
    }
    if (request.operation == PresetOperation::remove) {
        // Clearing a transient binding must retain persistent identity and its latest dirty-state baseline.
        juce::ignoreUnused(
            processor.reconnectSelectedUserPreset(std::nullopt, request.generation, request.comparisonSlot)
        );
    } else if (completion.preset) {
        acceptPresetResult(request, std::move(*completion.preset));
    }
    if (completion.listingError.isNotEmpty() && request.operation != PresetOperation::list) {
        showError("The preset list could not be refreshed. " + completion.listingError);
    }
}

/// Resolves the displayed selector index immediately, then reads its exact revision on the worker.
void ProcessingToolbarContent::loadSelectedPreset()
{
    const int index = presetSelector.getSelectedId() - 1;
    shownPresetText.reset();
    if (!canInteract() || index < 0 || static_cast<std::size_t>(index) >= presets.size()) {
        updatePresetSelector();
        return;
    }
    cancelNameEdit();
    auto request = makeRequest(PresetOperation::load);
    request.selection = presets.at(static_cast<std::size_t>(index));
    queueRequest(std::move(request));
    refresh();
}

/// Builds inspectable menu contents without creating a platform popup window.
juce::PopupMenu ProcessingToolbarContent::buildPresetMenu() const
{
    const bool selected = processor.getSelectedUserPreset().has_value();
    juce::PopupMenu menu;
    menu.addItem(static_cast<int>(PresetMenuAction::update), "Save", canSave());
    menu.addItem(static_cast<int>(PresetMenuAction::saveAs), "Save As...");
    menu.addSeparator();
    menu.addItem(static_cast<int>(PresetMenuAction::rename), "Rename...", selected);
    menu.addItem(static_cast<int>(PresetMenuAction::remove), "Delete...", selected);
    menu.addSeparator();
    menu.addItem(static_cast<int>(PresetMenuAction::importPreset), "Import...");
    menu.addItem(static_cast<int>(PresetMenuAction::exportPreset), "Export...", selected);
    menu.addItem(
        static_cast<int>(PresetMenuAction::reconnect), "Reconnect...", processor.getPresetProvenance().has_value()
    );
    menu.addSeparator();
    menu.addItem(static_cast<int>(PresetMenuAction::refresh), "Refresh preset list");
    if (!issues.empty() || listingError.isNotEmpty()) {
        menu.addItem(static_cast<int>(PresetMenuAction::issues), "Preset file errors...");
    }
    if (nameError.isNotEmpty()) {
        juce::PopupMenu::Item feedback(nameError);
        feedback.setEnabled(false);
        menu.addItem(std::move(feedback));
    }
    return menu;
}

/// Offers concise library actions while retaining diagnostics outside the preset name field.
void ProcessingToolbarContent::showPresetMenu()
{
    if (!canInteract()) {
        return;
    }
    auto menu = buildPresetMenu();
    menu.setLookAndFeel(&dialogLookAndFeel);
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

/// Routes explicit commands, never inferring a destructive or reconnecting action from a name alone.
void ProcessingToolbarContent::handleMenuAction(const int action)
{
    if (!canInteract() || action == 0) {
        return;
    }
    switch (static_cast<PresetMenuAction>(action)) {
        case PresetMenuAction::saveAs:
            beginNameEdit(true);
            break;
        case PresetMenuAction::update:
            savePreset();
            break;
        case PresetMenuAction::reconnect:
            confirmReconnect();
            break;
        case PresetMenuAction::rename:
            beginNameEdit(false);
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

/// Keeps workflow alerts hidden and peerless, so native modal-fronting cannot activate them or create taskbar entries.
void ProcessingToolbarContent::configureDialogPresentation()
{
    if (dialogPresentation != PresetDialogPresentation::nonRendering) {
        return;
    }
    dialog->setVisible(false);
    dialog->removeFromDesktop();
    dialog->setAlpha(0.0F);
    dialog->setDropShadowEnabled(false);
    dialog->setWantsKeyboardFocus(false);
    dialog->setBroughtToFrontOnMouseClick(false);
    for (auto* child : dialog->getChildren()) {
        child->setWantsKeyboardFocus(false);
    }
}

/// Presents production dialogs modally, while tests retain actions and asynchronous results without a native peer.
void ProcessingToolbarContent::beginDialog(std::function<void(int)> completion)
{
    configureDialogPresentation();
    if (dialogPresentation == PresetDialogPresentation::visible) {
        dialog->enterModalState(true, juce::ModalCallbackFunction::create(std::move(completion)));
        return;
    }
    dialogCompletion = std::move(completion);
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    for (int index = 0; index < dialog->getNumButtons(); ++index) {
        auto* button = dialog->getButton(index);
        const int result = index == 0 ? ACCEPT_RESULT : CANCEL_RESULT;
        button->onClick = [safeThis, result] {
            if (safeThis != nullptr) {
                safeThis->finishDialog(result);
            }
        };
    }
}

/// Ends the same command on either presentation path, never deleting an alert from its own button callback.
void ProcessingToolbarContent::finishDialog(const int result)
{
    if (dialogPresentation == PresetDialogPresentation::visible) {
        dialog->exitModalState(result);
        return;
    }
    auto pendingCompletion = std::exchange(dialogCompletion, {});
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, completion = std::move(pendingCompletion), result] {
        if (safeThis != nullptr && !safeThis->closing && completion) {
            completion(result);
        }
    });
}

/// Reports whether Save has a changed, valid name or sound and a writable bound revision.
bool ProcessingToolbarContent::canSave() const
{
    const auto presentation = processor.getPresetPresentation();
    if (!canInteract() || (nameEditContext && !requestIsCurrent(*nameEditContext))) {
        return false;
    }
    const auto name = currentPresetName();
    if (UserPresets::validateName(name).failed()) {
        return false;
    }
    if (savingAs) {
        return nameEditContext.has_value();
    }
    return presentation.selection
        && (presentation.modified || (presentation.provenance && name != presentation.provenance->name));
}

/// Starts inline naming without changing the current file binding or recalling a matching library name.
void ProcessingToolbarContent::beginNameEdit(const bool saveAs)
{
    if (!canInteract()) {
        return;
    }
    if (nameEditContext && !requestIsCurrent(*nameEditContext)) {
        cancelNameEdit();
    }
    if (!nameEditContext) {
        nameEditContext = makeRequest(PresetOperation::update);
        draftName = nameEditContext->provenanceName;
    } else {
        draftName = presetSelector.getNameText();
    }
    savingAs = savingAs || saveAs || !nameEditContext->selection;
    nameError.clear();
    presetSelector.beginNameEdit(draftName);
    updatePresetSelector();
}

/// Discards only the local name draft. Sound and library identity remain unchanged.
void ProcessingToolbarContent::cancelNameEdit()
{
    nameEditContext.reset();
    draftName.clear();
    nameError.clear();
    savingAs = false;
    presetSelector.endNameEdit();
    shownPresetText.reset();
    updatePresetSelector();
}

/// Enter commits Save As, while ordinary name entry remains a draft until explicit Save.
void ProcessingToolbarContent::commitNameEdit()
{
    if (const auto validation = UserPresets::validateName(currentPresetName()); validation.failed()) {
        nameError = validation.getErrorMessage();
        updatePresetSelector();
        return;
    }
    if (savingAs) {
        savePreset();
    } else {
        presetSelector.endNameEdit();
        updatePresetSelector();
    }
}

/// Saves the current name and sound in one revision-checked request without an extra confirmation.
void ProcessingToolbarContent::savePreset()
{
    if (!canInteract()) {
        return;
    }
    if (nameEditContext && !requestIsCurrent(*nameEditContext)) {
        cancelNameEdit();
        return;
    }
    const auto name = currentPresetName();
    const auto validation = UserPresets::validateName(name);
    if (validation.failed()) {
        nameError = validation.getErrorMessage();
        updatePresetSelector();
        return;
    }
    if (!canSave()) {
        return;
    }
    auto request = makeRequest(savingAs ? PresetOperation::saveAs : PresetOperation::update);
    if (nameEditContext) {
        // Keep the draft's original context while capturing the latest sound. Queue admission rejects any restore.
        auto context = *nameEditContext;
        context.operation = request.operation;
        context.sound = std::move(request.sound);
        request = std::move(context);
    }
    request.name = name;
    if (nameEditContext) {
        draftName = name;
    }
    presetSelector.endNameEdit();
    queueRequest(std::move(request));
    refresh();
}

/// Owns an asynchronous dialog with lifetime-checked callbacks and Cancel as the safe confirmation default.
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
    dialog = std::make_unique<PresetAlertWindow>(title, message, icon, *this, dialogPresentation);
    dialog->setComponentID("preset-confirmation-dialog");
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
    beginDialog([safeThis, accepted](const int result) {
        if (safeThis == nullptr || safeThis->closing) {
            return;
        }
        safeThis->dialog.reset();
        if (result == ACCEPT_RESULT && accepted) {
            accepted();
        }
        safeThis->refresh();
    });
}

/// Reports failures without implying the active sound or retained preset baseline changed.
void ProcessingToolbarContent::showError(const juce::String& message)
{
    showDialog("ImageControl Presets", message, juce::MessageBoxIconType::WarningIcon, "OK", {});
}

/// Matches only the persistent UUID and confirms an exact displayed revision before a fresh worker validation.
/// Binding must not load sound or replace the restored modified-state baseline.
void ProcessingToolbarContent::confirmReconnect()
{
    auto request = makeRequest(PresetOperation::reconnect);
    if (!request.baseline) {
        return;
    }
    const auto candidate = std::ranges::find(presets, request.provenanceId, &UserPreset::id);
    if (candidate == presets.end()) {
        showError(
            "The original preset file is unavailable for " + request.provenanceName.quoted()
            + ". Refresh the list or use Save As to create a new preset."
        );
        return;
    }
    request.selection = *candidate;
    const bool soundChanged = !sameProcessingState(*request.baseline, candidate->snapshot);
    const auto discrepancy = soundChanged
        ? juce::String("The library sound differs from the project's stored preset baseline.")
        : juce::String("The library sound matches the project's stored preset baseline.");
    const auto message = "Reconnect stored preset " + request.provenanceName.quoted() + " to library preset "
        + candidate->name.quoted() + "?" + juce::newLine + discrepancy + juce::newLine
        + "The exact saved revision will be read and checked again. No sound is loaded, "
          "and the stored name and modified-state baseline stay unchanged. Save, Rename, Delete, and Export "
          "will then target this library revision.";
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    showDialog(
        "Reconnect Preset", message, juce::MessageBoxIconType::QuestionIcon, "Reconnect", [safeThis, request] mutable {
            if (safeThis != nullptr) {
                safeThis->queueRequest(std::move(request));
            }
        }
    );
}

/// Deletes only the captured revision. Sound, persistent name, and the latest baseline survive successful deletion.
void ProcessingToolbarContent::confirmDelete()
{
    auto request = makeRequest(PresetOperation::remove);
    if (!request.selection) {
        return;
    }
    const auto message = "Delete " + request.selection->name.quoted()
        + " from the user preset library? This cannot be undone. "
          "The current sound, stored preset name, and modified-state baseline will not change. Save As remains "
          "available.";
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    showDialog("Delete Preset", message, juce::MessageBoxIconType::WarningIcon, "Delete", [safeThis, request] mutable {
        if (safeThis != nullptr) {
            safeThis->queueRequest(std::move(request));
        }
    });
}

/// Imports and validates a file into the library only. Import never recalls sound or replaces provenance.
void ProcessingToolbarContent::chooseImport()
{
    auto request = makeRequest(PresetOperation::importPreset);
    fileChooser
        = std::make_unique<juce::FileChooser>("Import ImageControl Preset", juce::File(), "*.xml", true, false, this);
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    fileChooser->launchAsync(
        fileChooserFlags(juce::FileBrowserComponent::openMode),
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

/// Exports the exact saved revision rather than unsaved current edits, without overwriting existing destinations.
void ProcessingToolbarContent::chooseExport()
{
    auto request = makeRequest(PresetOperation::exportPreset);
    if (!request.selection) {
        return;
    }
    fileChooser = std::make_unique<juce::FileChooser>(
        "Export Saved Preset", request.selection->file, "*.xml", true, false, this
    );
    const juce::Component::SafePointer<ProcessingToolbarContent> safeThis(this);
    fileChooser->launchAsync(
        fileChooserFlags(juce::FileBrowserComponent::saveMode),
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
}  // namespace esgrove::image_control
