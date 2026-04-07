#include "AudioBatchComponent.h"
#include "CustomLookAndFeel.h"
#include "utils.h"

#include <JuceHeader.h>

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow(juce::String name) :
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

        setResizable(true, true);
        setResizeLimits(800, 600, 8192, 8192);
        centreWithSize(getWidth(), getHeight());
    }

    ~MainWindow() override
    {
        clearContentComponent();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    std::unique_ptr<AudioBatchComponent> audioBatch;
    std::unique_ptr<juce::CustomLookAndFeel> lookAndFeel = std::make_unique<juce::CustomLookAndFeel>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

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
        juce::ArgumentList arguments {getApplicationName(), commandLineParameters};
        const bool startHeadless = arguments.containsOption("--headless|-H");

        // Init log file in OS default location under dir "AudioBatch"
        // Mac:     /Users/<username>/Library/Logs/AudioBatch
        // Windows: C:\Users\<username>\AppData\Roaming\AudioBatch
        logger = std::unique_ptr<juce::FileLogger>(juce::FileLogger::createDefaultAppLogger(
            getApplicationName(), getApplicationName() + ".log", getApplicationName(), 32768
        ));

        juce::Logger::setCurrentLogger(logger.get());

        utils::log_system_info();
        if (arguments.size() > 0) {
            utils::log_info("Args: " + commandLineParameters);
        }

        mainWindow = std::make_unique<MainWindow>(getApplicationName());

        if (startHeadless) {
            mainWindow->setVisible(false);
        } else {
            mainWindow->setVisible(true);
            mainWindow->toFront(true);
            mainWindow->grabKeyboardFocus();
        }
    }

    void shutdown() override
    {
        mainWindow.reset();
        juce::Logger::setCurrentLogger(nullptr);
    }

    void systemRequestedQuit() override
    {
        if (auto exit_code = getApplicationReturnValue(); exit_code != 0) {
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
