/// Implementation of MainWindow.
/// Covers window setup and teardown, and the menu bar model that dispatches menu commands to the AudioBatchComponent
/// and the plugin chain.

#include "gui/MainWindow.h"

#include "gui/AudioBatchComponent.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "gui/LiveChain.h"
#include "utils/AppInfo.h"
#include "utils/FileUtils.h"
#include "utils/GuiUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace audiobatch
{
namespace
{
/// First id of the plugin chain presets submenu, which uses the ids from it upwards.
/// It sits above the command ids and far below the Add Plugin submenu ids of juce::KnownPluginList.
constexpr int presetsMenuFirstItemId {1000};

/// Menu bar items that menuItemSelected runs directly.
/// Their ids stay below the command ids, and both stay far below the Add Plugin submenu ids of juce::KnownPluginList.
enum class MenuItemId : std::uint8_t {
    addFolder = 1,
    clearFiles,
    reanalyzeSelected,
    supportedFormats,
    clearChain,
    scanPlugins,
    normalizeAll,
    processAll,
};

/// Menu bar commands that the command manager runs through perform, so they can have keyboard shortcuts.
enum class CommandId : std::uint8_t {
    rescan = 101,
    revealSelected,
    removeSelected,
    moveSelectedToTrash,
    about,
    quit,
    togglePreviewSection,
    toggleSeratoWaveformColouring,
    toggleLoudnessGraph,
    toggleAlbumArtwork,
    toggleCustomGainEstimates,
    togglePluginPreview,
    togglePreviewCompensation,
    toggleContinuePlayback,
    togglePlayFromFirstCuePoint,
    previewWaveform,
    previewSpectrogram,
    previewSideBySide,
    previewStacked,
    toggleSampleOverloads,
    toggleTruePeakOverloads,
    toggleOverloadLines,
    chooseFolder,
    audioSettings,
    editChain,
    cancelRun,
    addFolder,
    addFiles,
    selectAll,
    normalizeSelected,
    openParentFolder,
    clearCustomGain,
    processSelected,
    stopPlayback,
};

/// Paired menu commands and preview modes in their canonical display order.
constexpr std::array PREVIEW_VIEW_COMMANDS {
    std::pair {CommandId::previewWaveform, PreviewViewMode::waveform},
    std::pair {CommandId::previewSpectrogram, PreviewViewMode::spectrogram},
    std::pair {CommandId::previewSideBySide, PreviewViewMode::sideBySide},
    std::pair {CommandId::previewStacked, PreviewViewMode::stacked},
};

/// First of the contiguous command ids.
constexpr CommandId firstCommand {CommandId::rescan};
/// Last of the contiguous command ids, which new commands replace.
constexpr CommandId lastCommand {CommandId::stopPlayback};

/// Disjoint submenu ranges below the plugin library's generated ids.
constexpr int COLUMNS_MENU_FIRST_ID {2000};
constexpr int OUTPUT_MENU_FIRST_ID {3000};
constexpr int SUBMENU_RANGE_SIZE {1000};

/// Returns the menu item for a menu bar result id, or nullopt for a command or an Add Plugin submenu id.
constexpr std::optional<MenuItemId> menuItemFromId(const int menuItemID) noexcept
{
    return utils::enumFromId<MenuItemId, MenuItemId::addFolder, MenuItemId::processAll>(menuItemID);
}

/// Returns the command for a JUCE command id, or nullopt for an id this window does not register.
constexpr std::optional<CommandId> commandFromId(const juce::CommandID commandID) noexcept
{
    return utils::enumFromId<CommandId, firstCommand, lastCommand>(commandID);
}

/// Adds a menu bar item that menuItemSelected runs.
void addMenuItem(juce::PopupMenu& menu, const MenuItemId item, const juce::String& text, const bool isEnabled = true)
{
    menu.addItem(std::to_underlying(item), text, isEnabled);
}

/// Adds a menu bar item for a command, showing its key press and its registered name unless a display name is given.
void addCommandItem(
    juce::PopupMenu& menu,
    juce::ApplicationCommandManager& commandManager,
    const CommandId command,
    const juce::String& displayName = {}
)
{
    menu.addCommandItem(&commandManager, std::to_underlying(command), displayName);
}
}  // namespace

/// Builds the window, installs the custom look-and-feel as the global default,
/// creates the owned AudioBatchComponent content, and configures the platform menu bar.
/// The window is not made visible here, callers use showAndActivate for that.
MainWindow::MainWindow(const juce::String& name) :
    DocumentWindow(name, CustomLookAndFeel::greyDark(), DocumentWindow::allButtons)
{
    const auto constructorStartedAtMs = juce::Time::getMillisecondCounterHiRes();
    const auto logStartupCheckpoint = [constructorStartedAtMs](const juce::String& step) {
        utils::logDebug(
            "MainWindow startup: {} ({:.1f} ms)",
            step,
            juce::Time::getMillisecondCounterHiRes() - constructorStartedAtMs
        );
    };

    logStartupCheckpoint("constructor begin");
    juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());
#if JUCE_WINDOWS
    setUsingNativeTitleBar(false);
    setTitleBarTextCentred(false);
    setTitleBarHeight(layout::titleBarHeight);
#else
    setUsingNativeTitleBar(true);
#endif

    logStartupCheckpoint("creating main content component");
    content = std::make_unique<AudioBatchComponent>();
    setContentNonOwned(content.get(), true);
    logStartupCheckpoint("main content component created");

    commandManager.registerAllCommandsForTarget(this);
    commandManager.setFirstCommandTarget(this);
    getAudioBatch().setViewOptionsChangedCallback([this] {
        commandManager.commandStatusChanged();
        menuItemsChanged();
    });
    addKeyListener(commandManager.getKeyMappings());

#if JUCE_MAC
    // Test executables have no application menu to replace, so they only exercise the commands and key mappings.
#ifndef AUDIOBATCH_TESTING
    juce::MenuBarModel::setMacMainMenu(this);
#endif
#else
    setMenuBar(this, layout::inWindowMenuBarHeight);
#endif
    logStartupCheckpoint("menu bar configured");

    setResizable(true, true);
    setResizeLimits(
        layout::minimumWindowWidth, layout::minimumWindowHeight, layout::maximumWindowSize, layout::maximumWindowSize
    );
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
        setBounds(layout::initialWindowBounds(display->userBounds.toNearestInt()));
    } else {
        centreWithSize(layout::defaultWindowWidth, layout::defaultWindowHeight);
    }
    content->fitPreviewToControls();
    logStartupCheckpoint("constructor complete");
}

/// Loads the folders and files given on the command line, or forwarded from another launch, into the results table.
/// Folders replace the root folders, and files are added to the table, as when they are dropped onto the window.
void MainWindow::openPaths(const juce::StringArray& paths) const
{
    getAudioBatch().openPaths(paths, false);
}

/// Makes the window visible and pushes it to the front with keyboard focus.
/// Safe to call repeatedly,
/// the GUI startup path also invokes it again asynchronously to work around platform focus quirks on first launch.
void MainWindow::showAndActivate()
{
    utils::logDebug("MainWindow startup: showAndActivate begin");
    setVisible(true);
    setMinimised(false);
    utils::bringWindowToFront(*this);

    content->grabKeyboardFocus();

    utils::logDebug("MainWindow startup: showAndActivate end");
}

/// Detaches the menu bar model and clears the global look-and-feel before the owned look-and-feel object is destroyed.
MainWindow::~MainWindow()
{
    removeKeyListener(commandManager.getKeyMappings());
#if JUCE_MAC
#ifndef AUDIOBATCH_TESTING
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
#else
    setMenuBar(nullptr);
#endif
    clearContentComponent();
    content.reset();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}

/// Routes the window close button to a normal application quit request.
void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

/// Returns the top-level menu names.
/// On macOS the application menu is included as the first entry, other platforms omit it.
juce::StringArray MainWindow::getMenuBarNames()
{
#if JUCE_MAC
    return {juce::String(ProjectInfo::projectName), "File", "Plugins", "Options", "Help"};
#else
    return {"File", "Plugins", "Options", "Help"};
#endif
}

/// Builds the popup menu for one top-level menu.
/// macOS moves About and Quit into the application menu, so its indices are offset by that menu.
juce::PopupMenu MainWindow::getMenuForIndex(const int topLevelMenuIndex, const juce::String& menuName)
{
    juce::ignoreUnused(menuName);

    juce::PopupMenu menu;
#if JUCE_MAC
    if (topLevelMenuIndex == 0) {
        addCommandItem(menu, commandManager, CommandId::about);
        menu.addSeparator();
        addCommandItem(menu, commandManager, CommandId::quit);
        return menu;
    }
    const auto index = topLevelMenuIndex - 1;
#else
    const auto index = topLevelMenuIndex;
#endif
    switch (index) {
        case 0:
            appendFileMenuItems(menu);
#if !JUCE_MAC
            menu.addSeparator();
            addCommandItem(menu, commandManager, CommandId::quit);
#endif
            break;
        case 1:
            appendPluginOptionsMenuItems(menu);
            break;
        case 2:
            appendOptionsMenuItems(menu);
            break;
        case 3:
            addMenuItem(menu, MenuItemId::supportedFormats, "Normalization Format Support...");
            menu.addSeparator();
            addCommandItem(menu, commandManager, CommandId::about);
            break;
        default:
            break;
    }
    return menu;
}

/// Dispatches a chosen menu item to the matching AudioBatchComponent or plugin chain action.
/// Commands are skipped, because the menu invokes them through perform before it reports the choice here.
/// Other unknown ids go to handlePluginChoiceMenuItem, because the Add Plugin submenu generates its ids dynamically.
void MainWindow::menuItemSelected(const int menuItemID, int topLevelMenuIndex)
{
    juce::ignoreUnused(topLevelMenuIndex);

    if (menuItemID > COLUMNS_MENU_FIRST_ID && menuItemID < COLUMNS_MENU_FIRST_ID + SUBMENU_RANGE_SIZE) {
        getAudioBatch().handleColumnsMenuItem(menuItemID, COLUMNS_MENU_FIRST_ID);
        menuItemsChanged();
        return;
    }
    if (menuItemID >= OUTPUT_MENU_FIRST_ID && menuItemID < OUTPUT_MENU_FIRST_ID + SUBMENU_RANGE_SIZE) {
        getAudioBatch().handleOutputDeviceMenuItem(menuItemID, OUTPUT_MENU_FIRST_ID);
        return;
    }
    if (std::cmp_greater_equal(menuItemID, std::to_underlying(firstCommand))
        && std::cmp_less_equal(menuItemID, std::to_underlying(lastCommand)))
    {
        return;
    }
    if (std::cmp_less(menuItemID, std::to_underlying(MenuItemId::addFolder))
        || std::cmp_greater(menuItemID, std::to_underlying(MenuItemId::processAll)))
    {
        handlePluginChoiceMenuItem(menuItemID);
        return;
    }
    const auto item = menuItemFromId(menuItemID);
    if (!item) {
        return;
    }

    switch (*item) {
        case MenuItemId::addFolder:
            getAudioBatch().addRootFolder();
            break;
        case MenuItemId::clearFiles:
            getAudioBatch().clearAllRecords();
            break;
        case MenuItemId::reanalyzeSelected:
            getAudioBatch().reanalyzeSelectedRecords();
            break;
        case MenuItemId::supportedFormats:
            getAudioBatch().showSupportedNormalizationFormats();
            break;
        case MenuItemId::clearChain:
            getAudioBatch().getLiveChain().clearChain();
            break;
        case MenuItemId::scanPlugins:
            getAudioBatch().getLiveChain().showScanWindow();
            break;
        case MenuItemId::normalizeAll:
            getAudioBatch().normalizeAllRecords();
            break;
        case MenuItemId::processAll:
            getAudioBatch().processAllRecords();
            break;
    }
}

/// Returns nullptr, the window is the only command target and has no parent target to forward to.
juce::ApplicationCommandTarget* MainWindow::getNextCommandTarget()
{
    return nullptr;
}

/// Lists the commands that appear in the menu bar with a keyboard shortcut.
void MainWindow::getAllCommands(juce::Array<juce::CommandID>& commands)
{
    constexpr juce::CommandID lastCommandID {std::to_underlying(lastCommand)};
    for (juce::CommandID commandID = std::to_underlying(firstCommand); commandID <= lastCommandID; ++commandID) {
        commands.add(commandID);
    }
}

/// Describes one command, including the default key presses the menu displays next to its item,
/// and whether the command can run right now.
/// The window's key mappings handle shortcuts not already consumed by AudioBatchComponent::keyPressed.
/// On macOS the menu also installs the shortcuts other than plain Delete and Backspace as key equivalents,
/// which fire when no component consumes the key press, for example while a dialog is open.
/// A command is therefore inactive while the window is blocked by a dialog or the command has nothing to act on,
/// and the command manager does not invoke inactive commands.
void MainWindow::getCommandInfo(const juce::CommandID commandID, juce::ApplicationCommandInfo& result)
{
    constexpr auto fileCategory = "File";
    const auto& audioBatch = getAudioBatch();
    const auto blockedByDialog = audioBatch.isCurrentlyBlockedByAnotherModalComponent();
    const auto canActOnSelection = !blockedByDialog && audioBatch.hasSelectedRecords();
    const auto command = commandFromId(commandID);
    if (!command.has_value()) {
        return;
    }

    switch (*command) {
        case CommandId::addFolder:
            result.setInfo("Add Folder...", "Add a root folder", fileCategory, 0);
            result.addDefaultKeypress(
                'o',
                juce::ModifierKeys(juce::ModifierKeys::commandModifier).withFlags(juce::ModifierKeys::shiftModifier)
            );
            result.setActive(!blockedByDialog && !audioBatch.isFileOperationInProgress());
            break;
        case CommandId::addFiles:
            result.setInfo("Add Files...", "Add audio files to the results", fileCategory, 0);
            result.setActive(!blockedByDialog && !audioBatch.isFileOperationInProgress());
            break;
        case CommandId::selectAll:
            result.setInfo("Select All Files", "Select every listed file", fileCategory, 0);
            result.addDefaultKeypress('a', juce::ModifierKeys::commandModifier);
            result.setActive(!blockedByDialog);
            break;
        case CommandId::stopPlayback:
            result.setInfo("Stop Playback", "Stop playback and return to the start", fileCategory, 0);
            result.addDefaultKeypress(juce::KeyPress::escapeKey, juce::ModifierKeys::noModifiers);
            result.setActive(!blockedByDialog);
            break;
        case CommandId::normalizeSelected:
            result.setInfo(
                utils::format("Normalize Selected to {}", audioBatch.describeNormalizationTarget()),
                "Normalize selected files",
                fileCategory,
                0
            );
            result.setActive(canActOnSelection && !audioBatch.isFileOperationInProgress());
            break;
        case CommandId::openParentFolder:
            result.setInfo("Open Parent Folder", "Open the selected files' parent folders", fileCategory, 0);
            result.setActive(canActOnSelection);
            break;
        case CommandId::clearCustomGain:
            result.setInfo("Clear Per-file Gain", "Clear the selected files' explicit gains", fileCategory, 0);
            result.setActive(canActOnSelection);
            break;
        case CommandId::processSelected:
            result.setInfo(
                "Process Selected With Plugin Chain", "Render selected files through enabled plugins", "Plugins", 0
            );
            result.setActive(
                canActOnSelection && !audioBatch.isFileOperationInProgress()
                && audioBatch.getLiveChain().getNumEnabledValidSlots() > 0
            );
            break;
        case CommandId::chooseFolder:
            result.setInfo("Choose Folder...", "Choose the root folder to analyze", fileCategory, 0);
            result.addDefaultKeypress('o', juce::ModifierKeys::commandModifier);
            result.setActive(!blockedByDialog && !audioBatch.isFileOperationInProgress());
            break;
        case CommandId::editChain:
            result.setInfo("Edit Chain...", "Open the plugin chain editor", "Plugins", 0);
            result.addDefaultKeypress('e', juce::ModifierKeys::commandModifier);
            result.setActive(!blockedByDialog);
            break;
        case CommandId::audioSettings:
            result.setInfo("Audio Settings...", "Configure audio playback and preview preferences", "Options", 0);
            result.addDefaultKeypress('p', juce::ModifierKeys::commandModifier);
            result.setActive(!blockedByDialog);
            break;
        case CommandId::previewWaveform:
        case CommandId::previewSpectrogram:
        case CommandId::previewSideBySide:
        case CommandId::previewStacked:
            for (const auto& [viewCommand, mode] : PREVIEW_VIEW_COMMANDS) {
                if (*command == viewCommand) {
                    result.setInfo(previewViewModeLabel(mode), "Choose the preview visualization", "Options", 0);
                    result.setTicked(audioBatch.getPreviewViewMode() == mode);
                    result.setActive(!blockedByDialog);
                    break;
                }
            }
            break;
        case CommandId::toggleSampleOverloads:
            result.setInfo("Peak overloads", "Show source sample-peak overload indicators", "Options", 0);
            result.setTicked(audioBatch.isSampleOverloadVisible());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::toggleTruePeakOverloads:
            result.setInfo("True-peak overloads", "Show source true-peak overload indicators", "Options", 0);
            result.setTicked(audioBatch.isTruePeakOverloadVisible());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::toggleOverloadLines:
            result.setInfo(
                "Overload vertical lines", "Extend enabled overload indicators as vertical lines", "Options", 0
            );
            result.setTicked(audioBatch.areOverloadLinesVisible());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::toggleLoudnessGraph:
            result.setInfo(
                "Show Loudness Graph", "Overlay source-file short-term loudness on the preview", "Options", 0
            );
            result.setTicked(audioBatch.isLoudnessGraphEnabled());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::toggleAlbumArtwork:
            result.setInfo("Show Album Artwork", "Show the embedded album artwork beside the file name", "Options", 0);
            result.setTicked(audioBatch.isAlbumArtworkEnabled());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::toggleCustomGainEstimates:
            result.setInfo(
                "Show custom gain estimates",
                "Show custom-gain estimates beside the unchanged source measurements",
                "Options",
                0
            );
            result.setTicked(audioBatch.isCustomGainEstimatesEnabled());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::togglePluginPreview:
            result.setInfo(
                "Preview Plugin Chain", "Listen through enabled plugins and custom input gain", "Options", 0
            );
            result.setTicked(audioBatch.isPluginPreviewEnabled());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::togglePreviewCompensation:
            result.setInfo(
                "Compensate Preview Plugin Latency", "Align processed audio with the file position", "Options", 0
            );
            result.setTicked(audioBatch.isPreviewLatencyCompensated());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::toggleContinuePlayback:
            result.setInfo(
                "Continue Playback When Changing Files",
                "Continue active playback on a newly selected file, and play the next file when one ends",
                "Options",
                0
            );
            result.setTicked(audioBatch.isContinuePreviewPlaybackEnabled());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::togglePlayFromFirstCuePoint:
            result.setInfo(
                "Play from First Cue Point",
                "Load files at the first cue point if it is at or before halfway, otherwise start at zero",
                "Options",
                0
            );
            result.setTicked(audioBatch.isPlayFromFirstCuePointEnabled());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::quit:
            result.setInfo("Quit", "Quits the application", fileCategory, 0);
            result.addDefaultKeypress('q', juce::ModifierKeys::commandModifier);
            result.setActive(!blockedByDialog);
            break;
        case CommandId::toggleSeratoWaveformColouring:
            result.setInfo(
                "Serato Waveform Colours", "Uses Serato spectral colours for waveform previews", "Options", 0
            );
            result.addDefaultKeypress(
                's',
                juce::ModifierKeys(juce::ModifierKeys::commandModifier).withFlags(juce::ModifierKeys::shiftModifier)
            );
            result.setTicked(audioBatch.isSeratoWaveformColouringEnabled());
            result.setActive(!blockedByDialog);
            break;
        case CommandId::togglePreviewSection:
            result.setInfo(
                audioBatch.isPreviewSectionExpanded() ? "Collapse Bottom Section" : "Expand Bottom Section",
                "Toggles the compact preview and processing row",
                "Options",
                0
            );
            result.addDefaultKeypress(
                juce::KeyPress::spaceKey,
                juce::ModifierKeys(juce::ModifierKeys::commandModifier).withFlags(juce::ModifierKeys::shiftModifier)
            );
            result.setActive(!blockedByDialog);
            break;
        case CommandId::about:
            result.setInfo(
                utils::format("About {}", juce::String(ProjectInfo::projectName)),
                "Shows application information",
                "Help",
                0
            );
            result.addDefaultKeypress('i', juce::ModifierKeys::commandModifier);
#if JUCE_MAC
            result.addDefaultKeypress('i', juce::ModifierKeys::ctrlModifier);
#endif
            result.setActive(!blockedByDialog);
            break;
        case CommandId::cancelRun:
            result.setInfo(
                "Cancel Run", "Cancels the running analysis, normalization, or plugin processing", fileCategory, 0
            );
            result.addDefaultKeypress('.', juce::ModifierKeys::commandModifier);
            result.setActive(!blockedByDialog && audioBatch.isBackgroundWorkRunning());
            break;
        case CommandId::rescan:
            result.setInfo("Rescan", "Re-runs analysis for all root folders", fileCategory, 0);
            result.addDefaultKeypress(juce::KeyPress::F5Key, juce::ModifierKeys::noModifiers);
            result.setActive(!blockedByDialog && !audioBatch.isFileOperationInProgress());
            break;
        case CommandId::revealSelected:
            result.setInfo(
                utils::revealInFileManagerLabel(), "Reveals the selected files in the file manager", fileCategory, 0
            );
            result.addDefaultKeypress('r', juce::ModifierKeys::commandModifier);
            result.setActive(canActOnSelection);
            break;
        case CommandId::removeSelected:
            result.setInfo("Remove Selected from List", "Removes the selected files from the list", fileCategory, 0);
            result.addDefaultKeypress(juce::KeyPress::deleteKey, juce::ModifierKeys::noModifiers);
            result.addDefaultKeypress(juce::KeyPress::backspaceKey, juce::ModifierKeys::noModifiers);
            result.setActive(canActOnSelection);
            break;
        case CommandId::moveSelectedToTrash:
            result.setInfo("Move Selected to Trash", "Moves the selected files to the system trash", fileCategory, 0);
#if JUCE_WINDOWS
            result.addDefaultKeypress(juce::KeyPress::deleteKey, juce::ModifierKeys::ctrlModifier);
            result.addDefaultKeypress(
                juce::KeyPress::deleteKey,
                juce::ModifierKeys(juce::ModifierKeys::ctrlModifier).withFlags(juce::ModifierKeys::shiftModifier)
            );
#endif
            result.addDefaultKeypress(juce::KeyPress::backspaceKey, juce::ModifierKeys::commandModifier);
            result.addDefaultKeypress(
                juce::KeyPress::backspaceKey,
                juce::ModifierKeys(juce::ModifierKeys::commandModifier).withFlags(juce::ModifierKeys::shiftModifier)
            );
#if JUCE_MAC
            result.addDefaultKeypress(juce::KeyPress::backspaceKey, juce::ModifierKeys::ctrlModifier);
            result.addDefaultKeypress(
                juce::KeyPress::backspaceKey,
                juce::ModifierKeys(juce::ModifierKeys::ctrlModifier).withFlags(juce::ModifierKeys::shiftModifier)
            );
#endif
            result.setActive(canActOnSelection);
            break;
    }
}

/// Runs a command chosen from the menu.
/// The menu also reports the choice through menuItemSelected, which ignores these commands so each choice runs once.
bool MainWindow::perform(const InvocationInfo& info)
{
    const auto command = commandFromId(info.commandID);
    if (!command.has_value()) {
        return false;
    }

    switch (*command) {
        case CommandId::addFolder:
            getAudioBatch().addRootFolder();
            return true;
        case CommandId::addFiles:
            getAudioBatch().addFiles();
            return true;
        case CommandId::selectAll:
            getAudioBatch().selectAllRecords();
            return true;
        case CommandId::stopPlayback:
            getAudioBatch().stopPlayback();
            return true;
        case CommandId::normalizeSelected:
            getAudioBatch().normalizeSelectedRecords();
            return true;
        case CommandId::openParentFolder:
            getAudioBatch().openSelectedParentFolders();
            return true;
        case CommandId::clearCustomGain:
            getAudioBatch().clearSelectedCustomGain();
            return true;
        case CommandId::processSelected:
            getAudioBatch().processSelectedRecords();
            return true;
        case CommandId::chooseFolder:
            getAudioBatch().chooseRootFolder();
            return true;
        case CommandId::editChain:
            getAudioBatch().getLiveChain().showChainEditor();
            return true;
        case CommandId::audioSettings:
            getAudioBatch().showAudioSettingsWindow();
            return true;
        case CommandId::previewWaveform:
        case CommandId::previewSpectrogram:
        case CommandId::previewSideBySide:
        case CommandId::previewStacked:
            for (const auto& [viewCommand, mode] : PREVIEW_VIEW_COMMANDS) {
                if (*command == viewCommand) {
                    getAudioBatch().setPreviewViewMode(mode);
                    return true;
                }
            }
            return false;
        case CommandId::toggleSampleOverloads:
            getAudioBatch().toggleSampleOverloads();
            return true;
        case CommandId::toggleTruePeakOverloads:
            getAudioBatch().toggleTruePeakOverloads();
            return true;
        case CommandId::toggleOverloadLines:
            getAudioBatch().toggleOverloadLines();
            return true;
        case CommandId::toggleLoudnessGraph:
            getAudioBatch().toggleLoudnessGraph();
            return true;
        case CommandId::toggleAlbumArtwork:
            getAudioBatch().toggleAlbumArtwork();
            return true;
        case CommandId::toggleCustomGainEstimates:
            getAudioBatch().toggleCustomGainEstimates();
            return true;
        case CommandId::togglePluginPreview:
            getAudioBatch().togglePluginPreview();
            return true;
        case CommandId::togglePreviewCompensation:
            getAudioBatch().togglePreviewLatencyCompensation();
            return true;
        case CommandId::toggleContinuePlayback:
            getAudioBatch().toggleContinuePreviewPlayback();
            return true;
        case CommandId::togglePlayFromFirstCuePoint:
            getAudioBatch().togglePlayFromFirstCuePoint();
            return true;
        case CommandId::quit:
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
            return true;
        case CommandId::toggleSeratoWaveformColouring:
            getAudioBatch().toggleSeratoWaveformColouring();
            return true;
        case CommandId::togglePreviewSection:
            getAudioBatch().togglePreviewSection();
            return true;
        case CommandId::about: {
            const auto appName = juce::String(ProjectInfo::projectName);
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions::makeOptionsOk(
                    juce::MessageBoxIconType::InfoIcon,
                    utils::format("About {}", appName),
                    utils::aboutMessage(appName),
                    "OK",
                    getContentComponent()
                ),
                nullptr
            );
            return true;
        }
        case CommandId::cancelRun:
            getAudioBatch().cancelCurrentWork();
            return true;
        case CommandId::rescan:
            getAudioBatch().rescanCurrentRoot();
            return true;
        case CommandId::revealSelected:
            getAudioBatch().revealSelectedRecord();
            return true;
        case CommandId::removeSelected:
            getAudioBatch().removeSelectedRecords();
            return true;
        case CommandId::moveSelectedToTrash:
            getAudioBatch().moveSelectedRecordsToTrash(
                info.invocationMethod != InvocationInfo::fromKeyPress || !info.keyPress.getModifiers().isShiftDown()
            );
            return true;
    }
    return false;
}

/// Appends the File menu items shared by every platform, ending with Cancel.
void MainWindow::appendFileMenuItems(juce::PopupMenu& menu)
{
    addCommandItem(menu, commandManager, CommandId::chooseFolder);
    addCommandItem(menu, commandManager, CommandId::addFolder);
    addCommandItem(menu, commandManager, CommandId::addFiles);
    addCommandItem(menu, commandManager, CommandId::rescan, "Rescan");
    menu.addSeparator();
    addCommandItem(menu, commandManager, CommandId::revealSelected, utils::revealInFileManagerLabel());
    addMenuItem(menu, MenuItemId::reanalyzeSelected, "Re-analyze Selected");
    addCommandItem(menu, commandManager, CommandId::normalizeSelected);
    addMenuItem(menu, MenuItemId::normalizeAll, getNormalizeAllLabel());
    addCommandItem(menu, commandManager, CommandId::openParentFolder);
    addCommandItem(menu, commandManager, CommandId::clearCustomGain);
    addCommandItem(menu, commandManager, CommandId::selectAll);
    addCommandItem(menu, commandManager, CommandId::removeSelected, "Remove Selected from List");
    addCommandItem(menu, commandManager, CommandId::moveSelectedToTrash, "Move Selected to Trash");
    addMenuItem(menu, MenuItemId::clearFiles, "Clear All Files");
    menu.addSeparator();
    addCommandItem(menu, commandManager, CommandId::cancelRun);
}

/// Appends the Options menu: the preview preferences, the Columns and Output Device submenus, and Audio Settings.
void MainWindow::appendOptionsMenuItems(juce::PopupMenu& menu)
{
    addCommandItem(menu, commandManager, CommandId::togglePreviewSection);
    appendPreviewViewMenuItems(menu);
    appendOverloadMenuItems(menu);
    addCommandItem(menu, commandManager, CommandId::toggleSeratoWaveformColouring);
    addCommandItem(menu, commandManager, CommandId::toggleLoudnessGraph);
    addCommandItem(menu, commandManager, CommandId::toggleAlbumArtwork);
    addCommandItem(menu, commandManager, CommandId::toggleCustomGainEstimates);
    addCommandItem(menu, commandManager, CommandId::togglePluginPreview);
    addCommandItem(menu, commandManager, CommandId::togglePreviewCompensation);
    addCommandItem(menu, commandManager, CommandId::toggleContinuePlayback);
    addCommandItem(menu, commandManager, CommandId::togglePlayFromFirstCuePoint);
    menu.addSeparator();
    menu.addSubMenu("Columns", getAudioBatch().createColumnsMenu(COLUMNS_MENU_FIRST_ID));
    menu.addSubMenu("Output Device", getAudioBatch().createOutputDeviceMenu(OUTPUT_MENU_FIRST_ID));
    menu.addSeparator();
    addCommandItem(menu, commandManager, CommandId::audioSettings);
}

/// Groups independent overload switches without imposing a radio selection or changing the stored line preference.
void MainWindow::appendOverloadMenuItems(juce::PopupMenu& menu)
{
    juce::PopupMenu overloads;
    addCommandItem(overloads, commandManager, CommandId::toggleSampleOverloads);
    addCommandItem(overloads, commandManager, CommandId::toggleTruePeakOverloads);
    addCommandItem(overloads, commandManager, CommandId::toggleOverloadLines);
    menu.addSubMenu("Overload indicators", overloads);
}

/// Builds mutually exclusive preview choices from the canonical preference owner through command tick states.
void MainWindow::appendPreviewViewMenuItems(juce::PopupMenu& menu)
{
    juce::PopupMenu views;
    for (const auto& [command, mode] : PREVIEW_VIEW_COMMANDS) {
        addCommandItem(views, commandManager, command, previewViewModeLabel(mode));
    }
    menu.addSubMenu("Preview view", views);
}

/// Appends the plugin chain section into a menu, mirroring the popup attached to the Plugin toolbar button.
void MainWindow::appendPluginOptionsMenuItems(juce::PopupMenu& menu)
{
    const auto& chain = getAudioBatch().getLiveChain();
    const auto summary = chain.getChainSummary();
    menu.addSectionHeader(
        summary.isEmpty() ? juce::String("No plugins in chain") : utils::format("Chain: {}", summary)
    );
    addCommandItem(menu, commandManager, CommandId::editChain);
    menu.addSubMenu("Chain Presets", chain.createPresetsMenu(presetsMenuFirstItemId));
    menu.addSeparator();

    chain.getLibrary().reload();
    pluginMenuTypes = chain.getLibrary().getKnownEffectTypes();
    juce::PopupMenu addSubmenu;
    juce::KnownPluginList::addToMenu(addSubmenu, pluginMenuTypes, juce::KnownPluginList::sortByManufacturer);
    menu.addSubMenu("Add Plugin", addSubmenu, !pluginMenuTypes.isEmpty());
    addMenuItem(menu, MenuItemId::clearChain, "Clear Chain", chain.getNumSlots() > 0);
    addMenuItem(menu, MenuItemId::scanPlugins, "Scan for Plugins...");
    menu.addSeparator();
    addCommandItem(menu, commandManager, CommandId::processSelected);
    addMenuItem(menu, MenuItemId::processAll, "Process All Files...", chain.getNumEnabledValidSlots() > 0);
}

/// Returns the label of the Normalize All Files item, which names the current target.
juce::String MainWindow::getNormalizeAllLabel() const
{
    return utils::format("Normalize All Files to {}...", getAudioBatch().describeNormalizationTarget());
}

/// Decodes a menu result against the plugin list the Add Plugin submenu was built from,
/// and appends the chosen plugin to the chain.
/// The menu ids are indices into that list, so a scan that adds a plugin while the menu is open cannot shift them.
/// Results that name no known plugin go to the chain presets submenu.
void MainWindow::handlePluginChoiceMenuItem(const int menuItemID) const
{
    auto& chain = getAudioBatch().getLiveChain();
    const auto index = juce::KnownPluginList::getIndexChosenByMenu(pluginMenuTypes, menuItemID);
    if (index < 0 || index >= pluginMenuTypes.size()) {
        chain.handlePresetsMenuItem(menuItemID, presetsMenuFirstItemId);
        return;
    }

    chain.addPlugin(pluginMenuTypes.getReference(index));
}

/// Returns the content component.
/// Valid from the constructor until the destructor, which are the only places that change it.
AudioBatchComponent& MainWindow::getAudioBatch() const
{
    return *content;
}
}  // namespace audiobatch
