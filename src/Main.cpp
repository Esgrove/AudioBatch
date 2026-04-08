#include "AudioBatchComponent.h"
#include "CustomLookAndFeel.h"
#include "utils.h"

#include <JuceHeader.h>

namespace audiobatch::app
{
enum MenuItemId {
    chooseFolderMenuItemId = 1,
    rescanMenuItemId,
    audioSettingsMenuItemId,
    supportedFormatsMenuItemId,
    aboutMenuItemId,
    quitMenuItemId,
};
}  // namespace audiobatch::app

using namespace audiobatch::app;

/// Top-level application window that hosts the main AudioBatch component.
class MainWindow : public juce::DocumentWindow, juce::MenuBarModel
{
public:
    explicit MainWindow(const juce::String& name) :
        DocumentWindow(name, juce::CustomLookAndFeel::greyDark, DocumentWindow::allButtons),
        audioBatch(std::make_unique<AudioBatchComponent>())
    {
        juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());
#if JUCE_WINDOWS
        setUsingNativeTitleBar(false);
        setTitleBarTextCentred(false);
        setTitleBarHeight(30);
#else
        setUsingNativeTitleBar(true);
#endif
        setContentNonOwned(audioBatch.get(), true);

#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(this);
#else
        setMenuBar(this, 26);
#endif

        setResizable(true, true);
        setResizeLimits(800, 600, 8192, 8192);
        centreWithSize(getWidth(), getHeight());
    }

    void showAndActivate()
    {
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
                break;
            case 2:
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
                menu.addItem(quitMenuItemId, "Quit");
                break;
            case 1:
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
                audioBatch->chooseRootFolder();
                break;
            case rescanMenuItemId:
                audioBatch->rescanCurrentRoot();
                break;
            case audioSettingsMenuItemId:
                audioBatch->showAudioSettingsWindow();
                break;
            case supportedFormatsMenuItemId:
                audioBatch->showSupportedNormalizationFormats();
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
            default:
                break;
        }
    }

    std::unique_ptr<AudioBatchComponent> audioBatch;
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
        const juce::ArgumentList arguments {getApplicationName(), commandLineParameters};
        const bool startHeadless = arguments.containsOption("--headless|-H");

        // Init log file in OS default location under dir "AudioBatch"
        // Mac:     /Users/<username>/Library/Logs/AudioBatch
        // Windows: C:\Users\<username>\AppData\Roaming\AudioBatch
        logger = utils::create_default_logger(getApplicationName());

        juce::Logger::setCurrentLogger(logger.get());

        utils::log_system_info();
        if (arguments.size() > 0) {
            utils::log_info("Args: " + commandLineParameters);
        }

        mainWindow = std::make_unique<MainWindow>(getApplicationName());

        if (startHeadless) {
            mainWindow->setVisible(false);
        } else {
            mainWindow->showAndActivate();

            juce::MessageManager::callAsync([safeWindow = juce::Component::SafePointer(mainWindow.get())] {
                if (safeWindow != nullptr) {
                    safeWindow->showAndActivate();
                }
            });
        }
    }

    void shutdown() override
    {
        mainWindow.reset();
        juce::Logger::setCurrentLogger(nullptr);
    }

    void systemRequestedQuit() override
    {
        if (const auto exit_code = getApplicationReturnValue(); exit_code != 0) {
            utils::log_info("Quit with non-zero exit code: " + juce::String(exit_code));
        } else {
            utils::log_info("Quit");
        }
        quit();
    }

    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        utils::log_info("Another instance started with args: " + commandLine);
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::FileLogger> logger;
};

START_JUCE_APPLICATION(AudioBatchApplication)
