/// Implementation of MainWindow:
/// window setup and teardown, and the menu bar model
/// that dispatches menu commands to the AudioBatchComponent and the plugin chain.

#include "MainWindow.h"

#include "AppInfo.h"
#include "FileUtils.h"
#include "Log.h"
#include "PluginChain.h"
#include "StringFormat.h"

/// Menu ids and window limits of the main window.
namespace audiobatch::main_window
{
enum MenuItemId : std::uint8_t {
    chooseFolderMenuItemId = 1,
    rescanMenuItemId,
    clearFilesMenuItemId,
    removeSelectedMenuItemId,
    reanalyzeSelectedMenuItemId,
    audioSettingsMenuItemId,
    supportedFormatsMenuItemId,
    aboutMenuItemId,
    quitMenuItemId,
    editChainMenuItemId,
    clearChainMenuItemId,
    scanPluginsMenuItemId,
    addFolderMenuItemId,
    moveSelectedToTrashMenuItemId,
    revealSelectedMenuItemId,
};

/// Smallest width in pixels the main window can be resized to,
/// chosen so the results table and the preview controls below it stay usable.
constexpr int minimumWindowWidth = 800;
/// Smallest height in pixels the main window can be resized to.
constexpr int minimumWindowHeight = 600;
}  // namespace audiobatch::main_window

using namespace audiobatch::main_window;

MainWindow::MainWindow(const juce::String& name) :
    DocumentWindow(name, juce::CustomLookAndFeel::greyDark(), DocumentWindow::allButtons)
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
    /// Height in pixels of the custom drawn title bar used on Windows.
    constexpr int windowsTitleBarHeight = 30;
    setTitleBarHeight(windowsTitleBarHeight);
#else
    setUsingNativeTitleBar(true);
#endif

    logStartupCheckpoint("creating main content component");
    setContentOwned(new AudioBatchComponent(), true);
    logStartupCheckpoint("main content component created");

    commandManager.registerAllCommandsForTarget(this);
    commandManager.setFirstCommandTarget(this);

#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(this);
#else
    /// Height in pixels of the in-window menu bar used on platforms without a global menu bar.
    constexpr int inWindowMenuBarHeight = 26;
    setMenuBar(this, inWindowMenuBarHeight);
#endif
    logStartupCheckpoint("menu bar configured");

    setResizable(true, true);
    setResizeLimits(minimumWindowWidth, minimumWindowHeight, 8192, 8192);
    centreWithSize(getWidth(), getHeight());
    logStartupCheckpoint("constructor complete");
}

void MainWindow::showAndActivate()
{
    utils::logDebug("MainWindow startup: showAndActivate begin");
    setVisible(true);
    setMinimised(false);
    toFront(true);

    if (auto* peer = getPeer()) {
        peer->grabFocus();
    }

    if (auto* content = getContentComponent()) {
        content->grabKeyboardFocus();
    } else {
        grabKeyboardFocus();
    }

    utils::logDebug("MainWindow startup: showAndActivate end");
}

MainWindow::~MainWindow()
{
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#else
    setMenuBar(nullptr);
#endif
    clearContentComponent();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

juce::StringArray MainWindow::getMenuBarNames()
{
#if JUCE_MAC
    return {juce::JUCEApplication::getInstance()->getApplicationName(), "File", "Options", "Help"};
#else
    return {"File", "Options", "Help"};
#endif
}

juce::PopupMenu MainWindow::getMenuForIndex(const int topLevelMenuIndex, const juce::String& menuName)
{
    juce::ignoreUnused(menuName);

    juce::PopupMenu menu;

#if JUCE_MAC
    switch (topLevelMenuIndex) {
        case 0:
            menu.addItem(
                aboutMenuItemId, utils::format("About {}", juce::JUCEApplication::getInstance()->getApplicationName())
            );
            menu.addSeparator();
            menu.addItem(quitMenuItemId, "Quit");
            break;
        case 1:
            menu.addItem(chooseFolderMenuItemId, "Choose Folder...");
            menu.addItem(addFolderMenuItemId, "Add Folder...");
            menu.addCommandItem(&commandManager, rescanMenuItemId, "Rescan");
            menu.addSeparator();
            menu.addCommandItem(&commandManager, revealSelectedMenuItemId, utils::revealInFileManagerLabel());
            menu.addItem(reanalyzeSelectedMenuItemId, "Re-analyze Selected");
            menu.addCommandItem(&commandManager, removeSelectedMenuItemId, "Remove Selected from List");
            menu.addCommandItem(&commandManager, moveSelectedToTrashMenuItemId, "Move Selected to Trash");
            menu.addItem(clearFilesMenuItemId, "Clear All Files");
            break;
        case 2:
            appendPluginOptionsMenuItems(menu);
            menu.addSeparator();
            menu.addItem(audioSettingsMenuItemId, "Audio Settings...");
            break;
        case 3:
            menu.addItem(supportedFormatsMenuItemId, "Normalization Format Support...");
            menu.addSeparator();
            menu.addItem(
                aboutMenuItemId, utils::format("About {}", juce::JUCEApplication::getInstance()->getApplicationName())
            );
            break;
        default:
            break;
    }
#else
    switch (topLevelMenuIndex) {
        case 0:
            menu.addItem(chooseFolderMenuItemId, "Choose Folder...");
            menu.addItem(addFolderMenuItemId, "Add Folder...");
            menu.addCommandItem(&commandManager, rescanMenuItemId, "Rescan");
            menu.addSeparator();
            menu.addCommandItem(&commandManager, revealSelectedMenuItemId, utils::revealInFileManagerLabel());
            menu.addItem(reanalyzeSelectedMenuItemId, "Re-analyze Selected");
            menu.addCommandItem(&commandManager, removeSelectedMenuItemId, "Remove Selected from List");
            menu.addCommandItem(&commandManager, moveSelectedToTrashMenuItemId, "Move Selected to Trash");
            menu.addItem(clearFilesMenuItemId, "Clear All Files");
            menu.addSeparator();
            menu.addItem(quitMenuItemId, "Quit");
            break;
        case 1:
            appendPluginOptionsMenuItems(menu);
            menu.addSeparator();
            menu.addItem(audioSettingsMenuItemId, "Audio Settings...");
            break;
        case 2:
            menu.addItem(supportedFormatsMenuItemId, "Normalization Format Support...");
            menu.addSeparator();
            menu.addItem(
                aboutMenuItemId, utils::format("About {}", juce::JUCEApplication::getInstance()->getApplicationName())
            );
            break;
        default:
            break;
    }
#endif

    return menu;
}

void MainWindow::menuItemSelected(const int menuItemID, int topLevelMenuIndex)
{
    juce::ignoreUnused(topLevelMenuIndex);

    switch (menuItemID) {
        case chooseFolderMenuItemId:
            getAudioBatch().chooseRootFolder();
            break;
        case addFolderMenuItemId:
            getAudioBatch().addRootFolder();
            break;
        case clearFilesMenuItemId:
            getAudioBatch().clearAllRecords();
            break;
        case rescanMenuItemId:
        case removeSelectedMenuItemId:
        case moveSelectedToTrashMenuItemId:
        case revealSelectedMenuItemId:
            // Handled by perform, the menu invokes the command before it reports the choice here.
            break;
        case reanalyzeSelectedMenuItemId:
            getAudioBatch().reanalyzeSelectedRecords();
            break;
        case audioSettingsMenuItemId:
            getAudioBatch().showAudioSettingsWindow();
            break;
        case supportedFormatsMenuItemId:
            getAudioBatch().showSupportedNormalizationFormats();
            break;
        case aboutMenuItemId: {
            const auto appName = juce::JUCEApplication::getInstance()->getApplicationName();

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
            break;
        }
        case quitMenuItemId:
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
            break;
        case editChainMenuItemId:
            if (auto* chain = getAudioBatch().getPluginChain()) {
                chain->showChainEditor();
            }
            break;
        case clearChainMenuItemId:
            if (auto* chain = getAudioBatch().getPluginChain()) {
                chain->clearChain();
            }
            break;
        case scanPluginsMenuItemId:
            if (auto* chain = getAudioBatch().getPluginChain()) {
                chain->showScanWindow();
            }
            break;
        default:
            handlePluginChoiceMenuItem(menuItemID);
            break;
    }
}

juce::ApplicationCommandTarget* MainWindow::getNextCommandTarget()
{
    return nullptr;
}

void MainWindow::getAllCommands(juce::Array<juce::CommandID>& commands)
{
    commands.addArray(
        {rescanMenuItemId, revealSelectedMenuItemId, removeSelectedMenuItemId, moveSelectedToTrashMenuItemId}
    );
}

void MainWindow::getCommandInfo(const juce::CommandID commandID, juce::ApplicationCommandInfo& result)
{
    constexpr auto fileCategory = "File";
    const auto& audioBatch = getAudioBatch();
    const auto blockedByDialog = audioBatch.isCurrentlyBlockedByAnotherModalComponent();
    const auto canActOnSelection = !blockedByDialog && audioBatch.hasSelectedRecords();

    switch (commandID) {
        case rescanMenuItemId:
            result.setInfo("Rescan", "Re-runs analysis for all root folders", fileCategory, 0);
            result.addDefaultKeypress(juce::KeyPress::F5Key, juce::ModifierKeys::noModifiers);
            result.setActive(!blockedByDialog && !audioBatch.isFileOperationInProgress());
            break;
        case revealSelectedMenuItemId:
            result.setInfo(
                utils::revealInFileManagerLabel(), "Reveals the selected file in the file manager", fileCategory, 0
            );
            result.addDefaultKeypress('r', juce::ModifierKeys::commandModifier);
            result.setActive(canActOnSelection);
            break;
        case removeSelectedMenuItemId:
            result.setInfo("Remove Selected from List", "Removes the selected files from the list", fileCategory, 0);
            result.addDefaultKeypress(juce::KeyPress::deleteKey, juce::ModifierKeys::noModifiers);
            result.addDefaultKeypress(juce::KeyPress::backspaceKey, juce::ModifierKeys::noModifiers);
            result.setActive(canActOnSelection);
            break;
        case moveSelectedToTrashMenuItemId:
            result.setInfo("Move Selected to Trash", "Moves the selected files to the system trash", fileCategory, 0);
            result.addDefaultKeypress(juce::KeyPress::backspaceKey, juce::ModifierKeys::commandModifier);
            result.setActive(canActOnSelection);
            break;
        default:
            break;
    }
}

bool MainWindow::perform(const InvocationInfo& info)
{
    switch (info.commandID) {
        case rescanMenuItemId:
            getAudioBatch().rescanCurrentRoot();
            return true;
        case revealSelectedMenuItemId:
            getAudioBatch().revealSelectedRecord();
            return true;
        case removeSelectedMenuItemId:
            getAudioBatch().removeSelectedRecords();
            return true;
        case moveSelectedToTrashMenuItemId:
            getAudioBatch().moveSelectedRecordsToTrash(true);
            return true;
        default:
            return false;
    }
}

void MainWindow::appendPluginOptionsMenuItems(juce::PopupMenu& menu) const
{
    auto* chain = getAudioBatch().getPluginChain();
    if (chain == nullptr) {
        return;
    }

    const auto summary = chain->getChainSummary();
    menu.addSectionHeader(
        summary.isEmpty() ? juce::String("No plugins in chain") : utils::format("Chain: {}", summary)
    );
    menu.addItem(editChainMenuItemId, "Edit Chain...");
    menu.addSeparator();

    const auto& types = chain->getKnownPluginList().getTypes();
    juce::PopupMenu addSubmenu;
    juce::KnownPluginList::addToMenu(addSubmenu, types, juce::KnownPluginList::sortByManufacturer);
    menu.addSubMenu("Add Plugin", addSubmenu, !types.isEmpty());
    menu.addItem(clearChainMenuItemId, "Clear Chain", chain->getNumSlots() > 0);
    menu.addItem(scanPluginsMenuItemId, "Scan for Plugins...");
}

void MainWindow::handlePluginChoiceMenuItem(const int menuItemID) const
{
    auto* chain = getAudioBatch().getPluginChain();
    if (chain == nullptr) {
        return;
    }

    const auto& types = chain->getKnownPluginList().getTypes();
    const auto index = juce::KnownPluginList::getIndexChosenByMenu(types, menuItemID);
    if (index < 0 || index >= types.size()) {
        return;
    }

    chain->addPlugin(types.getReference(index));
}

AudioBatchComponent& MainWindow::getAudioBatch() const
{
    return *dynamic_cast<AudioBatchComponent*>(getContentComponent());
}
