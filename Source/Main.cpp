#include "AudioBatchComponent.h"

#include <JuceHeader.h>

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow(juce::String name)
        : DocumentWindow(
            name,
            juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
            DocumentWindow::allButtons)
        , audioBatch(std::make_unique<AudioBatchComponent>())
    {
#if JUCE_WINDOWS
        setUsingNativeTitleBar(false);
        setTitleBarTextCentred(false);
        setTitleBarHeight(30);
#else
        setUsingNativeTitleBar(true);
#endif
        setContentOwned(audioBatch.get(), true);

        setResizable(true, true);
        setResizeLimits(800, 600, 8192, 8192);
        centreWithSize(getWidth(), getHeight());
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

private:
    std::unique_ptr<AudioBatchComponent> audioBatch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

class AudioBatchApplication : public juce::JUCEApplication
{
public:
    AudioBatchApplication() {}

    const juce::String getApplicationName() override { return ProjectInfo::projectName; }
    const juce::String getApplicationVersion() override { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLineParameters) override
    {
        juce::ArgumentList arguments {getApplicationName(), commandLineParameters};

        mainWindow = std::make_unique<MainWindow>(getApplicationName());

        if (arguments.containsOption("--headless")) {
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

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted(const juce::String& commandLine) override {}

private:
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::FileLogger> logger;
};

START_JUCE_APPLICATION(AudioBatchApplication)
