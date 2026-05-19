#include "AudioBatchComponent.h"
#include "CustomLookAndFeel.h"
#include "PluginChain.h"
#include "utils.h"

#include <JuceHeader.h>

namespace audiobatch::app
{
enum MenuItemId {
    chooseFolderMenuItemId = 1,
    rescanMenuItemId,
    clearFilesMenuItemId,
    removeSelectedMenuItemId,
    reanalyzeSelectedMenuItemId,
    audioSettingsMenuItemId,
    supportedFormatsMenuItemId,
    aboutMenuItemId,
    quitMenuItemId,
    editPluginMenuItemId,
    clearPluginMenuItemId,
    scanPluginsMenuItemId,
};
}  // namespace audiobatch::app

using namespace audiobatch::app;

/// Top-level application window that hosts the main AudioBatch component.
class MainWindow : public juce::DocumentWindow, juce::MenuBarModel
{
public:
    explicit MainWindow(const juce::String& name) :
        DocumentWindow(name, juce::CustomLookAndFeel::greyDark, DocumentWindow::allButtons)
    {
        const auto constructorStartedAtMs = juce::Time::getMillisecondCounterHiRes();
        const auto logStartupCheckpoint = [constructorStartedAtMs](const juce::String& step) {
            utils::logDebug(
                "MainWindow startup: " + step + " ("
                + juce::String(juce::Time::getMillisecondCounterHiRes() - constructorStartedAtMs, 1) + " ms)"
            );
        };

        logStartupCheckpoint("constructor begin");
        juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());
#if JUCE_WINDOWS
        setUsingNativeTitleBar(false);
        setTitleBarTextCentred(false);
        setTitleBarHeight(30);
#else
        setUsingNativeTitleBar(true);
#endif

        logStartupCheckpoint("creating main content component");
        setContentOwned(new AudioBatchComponent(), true);
        logStartupCheckpoint("main content component created");

#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(this);
#else
        setMenuBar(this, 26);
#endif
        logStartupCheckpoint("menu bar configured");

        setResizable(true, true);
        setResizeLimits(800, 600, 8192, 8192);
        centreWithSize(getWidth(), getHeight());
        logStartupCheckpoint("constructor complete");
    }

    void showAndActivate()
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

    ~MainWindow() override
    {
#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(nullptr);
#else
        setMenuBar(nullptr);
#endif
        clearContentComponent();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    juce::StringArray getMenuBarNames() override
    {
#if JUCE_MAC
        return {juce::JUCEApplication::getInstance()->getApplicationName(), "File", "Options", "Help"};
#else
        return {"File", "Options", "Help"};
#endif
    }

    juce::PopupMenu getMenuForIndex(const int topLevelMenuIndex, const juce::String& menuName) override
    {
        juce::ignoreUnused(menuName);

        juce::PopupMenu menu;

#if JUCE_MAC
        switch (topLevelMenuIndex) {
            case 0:
                menu.addItem(aboutMenuItemId, "About " + juce::JUCEApplication::getInstance()->getApplicationName());
                menu.addSeparator();
                menu.addItem(quitMenuItemId, "Quit");
                break;
            case 1:
                menu.addItem(chooseFolderMenuItemId, "Choose Folder...");
                menu.addItem(rescanMenuItemId, "Rescan");
                menu.addSeparator();
                menu.addItem(reanalyzeSelectedMenuItemId, "Re-analyze Selected");
                menu.addItem(removeSelectedMenuItemId, "Remove Selected from List");
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
                menu.addItem(aboutMenuItemId, "About " + juce::JUCEApplication::getInstance()->getApplicationName());
                break;
            default:
                break;
        }
#else
        switch (topLevelMenuIndex) {
            case 0:
                menu.addItem(chooseFolderMenuItemId, "Choose Folder...");
                menu.addItem(rescanMenuItemId, "Rescan");
                menu.addSeparator();
                menu.addItem(reanalyzeSelectedMenuItemId, "Re-analyze Selected");
                menu.addItem(removeSelectedMenuItemId, "Remove Selected from List");
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
                menu.addItem(aboutMenuItemId, "About " + juce::JUCEApplication::getInstance()->getApplicationName());
                break;
            default:
                break;
        }
#endif

        return menu;
    }

    void menuItemSelected(const int menuItemID, int topLevelMenuIndex) override
    {
        juce::ignoreUnused(topLevelMenuIndex);

        switch (menuItemID) {
            case chooseFolderMenuItemId:
                getAudioBatch().chooseRootFolder();
                break;
            case rescanMenuItemId:
                getAudioBatch().rescanCurrentRoot();
                break;
            case clearFilesMenuItemId:
                getAudioBatch().clearAllRecords();
                break;
            case removeSelectedMenuItemId:
                getAudioBatch().removeSelectedRecords();
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
            case aboutMenuItemId:
                juce::AlertWindow::showAsync(
                    juce::MessageBoxOptions::makeOptionsOk(
                        juce::MessageBoxIconType::InfoIcon,
                        "About " + juce::JUCEApplication::getInstance()->getApplicationName(),
                        juce::JUCEApplication::getInstance()->getApplicationName() + "\nVersion "
                            + juce::JUCEApplication::getInstance()->getApplicationVersion()
                            + "\n\nBatch audio analysis, preview, cleanup, and normalization.",
                        "OK",
                        getContentComponent()
                    ),
                    nullptr
                );
                break;
            case quitMenuItemId:
                juce::JUCEApplication::getInstance()->systemRequestedQuit();
                break;
            case editPluginMenuItemId:
                if (auto* chain = getAudioBatch().getPluginChain()) {
                    chain->openEditor();
                }
                break;
            case clearPluginMenuItemId:
                if (auto* chain = getAudioBatch().getPluginChain()) {
                    chain->clearSelection();
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

    /// Appends the plugin section into a menu, mirroring the popup attached to the Plugin toolbar button.
    void appendPluginOptionsMenuItems(juce::PopupMenu& menu)
    {
        auto* chain = getAudioBatch().getPluginChain();
        if (chain == nullptr) {
            return;
        }

        const auto description = chain->getSelectedPluginDescription();
        const bool hasSelection = description.fileOrIdentifier.isNotEmpty();

        if (hasSelection) {
            menu.addSectionHeader(description.name + " (" + description.pluginFormatName + ")");
            menu.addItem(editPluginMenuItemId, "Edit Plugin...");
            menu.addItem(clearPluginMenuItemId, "Clear Plugin");
            menu.addSeparator();
        }

        const auto& types = chain->getKnownPluginList().getTypes();
        juce::PopupMenu chooseSubmenu;
        juce::KnownPluginList::addToMenu(chooseSubmenu, types, juce::KnownPluginList::sortByManufacturer);
        menu.addSubMenu("Choose Plugin", chooseSubmenu, !types.isEmpty());
        menu.addItem(scanPluginsMenuItemId, "Scan for Plugins...");
    }

    /// Decodes a menu result against the known plugin list and applies it as the current selection.
    void handlePluginChoiceMenuItem(const int menuItemID)
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

        chain->selectPlugin(types.getReference(index));
    }

    AudioBatchComponent& getAudioBatch() const
    {
        return *static_cast<AudioBatchComponent*>(getContentComponent());
    }

    std::unique_ptr<juce::CustomLookAndFeel> lookAndFeel = std::make_unique<juce::CustomLookAndFeel>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

/// JUCE application entry point for the desktop GUI build.
class AudioBatchApplication : public juce::JUCEApplication
{
public:
    AudioBatchApplication() = default;

    const juce::String getApplicationName() override
    {
        return ProjectInfo::projectName;
    }
    const juce::String getApplicationVersion() override
    {
        return ProjectInfo::versionString;
    }
    bool moreThanOneInstanceAllowed() override
    {
        return false;
    }

    void initialise(const juce::String& commandLineParameters) override
    {
        const auto initialiseStartedAtMs = juce::Time::getMillisecondCounterHiRes();
        const auto logInitialiseCheckpoint = [initialiseStartedAtMs](const juce::String& step) {
            utils::logInfo(
                "Application startup: " + step + " ("
                + juce::String(juce::Time::getMillisecondCounterHiRes() - initialiseStartedAtMs, 1) + " ms)"
            );
        };
        const juce::ArgumentList arguments {getApplicationName(), commandLineParameters};
        const bool startHeadless = arguments.containsOption("--headless|-H");

        // Init log file in OS default location under dir "AudioBatch"
        // Mac:     /Users/<username>/Library/Logs/AudioBatch
        // Windows: C:\Users\<username>\AppData\Roaming\AudioBatch
        logger = utils::createDefaultLogger(getApplicationName());

        juce::Logger::setCurrentLogger(logger.get());

        utils::logSystemInfo();
        logInitialiseCheckpoint("logger ready");
        if (arguments.size() > 0) {
            utils::logInfo("Args: " + commandLineParameters);
        }

        logInitialiseCheckpoint("creating main window");
        mainWindow = std::make_unique<MainWindow>(getApplicationName());
        logInitialiseCheckpoint("main window created");

        if (startHeadless) {
            logInitialiseCheckpoint("headless startup requested");
            mainWindow->setVisible(false);
        } else {
            logInitialiseCheckpoint("showing main window");
            mainWindow->showAndActivate();

            juce::MessageManager::callAsync([safeWindow = juce::Component::SafePointer(mainWindow.get())] {
                if (safeWindow != nullptr) {
                    utils::logInfo("Application startup: async window activation begin");
                    safeWindow->showAndActivate();
                    utils::logInfo("Application startup: async window activation end");
                }
            });
        }

        logInitialiseCheckpoint("initialise complete");
    }

    void shutdown() override
    {
        mainWindow.reset();
        juce::Logger::setCurrentLogger(nullptr);
    }

    void systemRequestedQuit() override
    {
        if (const auto exit_code = getApplicationReturnValue(); exit_code != 0) {
            utils::logInfo("Quit with non-zero exit code: " + juce::String(exit_code));
        } else {
            utils::logInfo("Quit");
        }
        quit();
    }

    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        utils::logInfo("Another instance started with args: " + commandLine);
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::FileLogger> logger;
};

START_JUCE_APPLICATION(AudioBatchApplication)
