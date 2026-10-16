/// GUI application entry point.
/// Defines the JUCEApplication subclass that drives startup and shutdown
/// and creates the MainWindow declared in MainWindow.h.

#include "gui/MainWindow.h"
#include "plugins/PluginScanner.h"
#include "utils/AppInfo.h"
#include "utils/ConsolePrint.h"
#include "utils/Log.h"

#include <JuceHeader.h>

#if JUCE_WINDOWS
#include <array>
#include <bit>
#include <windows.h>
#endif

namespace audiobatch
{
namespace
{
/// Returns true when the command line contains the version flag.
bool isVersionRequest(const juce::String& commandLine)
{
    const juce::ArgumentList arguments {"AudioBatch", commandLine};
    return arguments.containsOption("--version|-V");
}

#if JUCE_WINDOWS
/// Identifies the existing executable without granting foreground permission to unrelated applications.
struct ForegroundPermissionRequest {
    juce::String applicationName;
    juce::String executablePath;
    DWORD currentProcessId;
};

/// Transfers a user launch's foreground permission to an existing instance before JUCE forwards its command line.
/// Windows otherwise allows the new process, but not the background instance, to activate a window.
void allowExistingInstanceToActivate()
{
    const ForegroundPermissionRequest request {
        .applicationName = ProjectInfo::projectName,
        .executablePath = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
        .currentProcessId = GetCurrentProcessId(),
    };
    EnumWindows(
        [](HWND window, const LPARAM context) -> BOOL {
            const auto& [applicationName, executablePath, currentProcessId]
                = *std::bit_cast<const ForegroundPermissionRequest*>(context);
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId == 0 || processId == currentProcessId) {
                return TRUE;
            }
            // Scan workers use the same executable, but never create the main application's titled window.
            constexpr std::size_t maximumWindowTitleLength = 256;
            std::array<wchar_t, maximumWindowTitleLength> windowTitle {};
            if (GetWindowTextW(window, windowTitle.data(), static_cast<int>(windowTitle.size())) == 0
                || juce::String(windowTitle.data()).upToFirstOccurrenceOf(" - ", false, false) != applicationName)
            {
                return TRUE;
            }
            const std::unique_ptr<void, decltype(&CloseHandle)> process {
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId), &CloseHandle
            };
            if (process == nullptr) {
                return TRUE;
            }
            constexpr std::size_t maximumWindowsPathLength = 32768;
            std::array<wchar_t, maximumWindowsPathLength> executablePathBuffer {};
            auto length = static_cast<DWORD>(executablePathBuffer.size());
            if (QueryFullProcessImageNameW(process.get(), 0, executablePathBuffer.data(), &length) != 0
                && executablePath.equalsIgnoreCase(juce::String(executablePathBuffer.data(), length)))
            {
                if (AllowSetForegroundWindow(processId) == 0) {
                    utils::logDebug("Windows did not transfer foreground permission to the existing instance");
                }
                return FALSE;
            }
            return TRUE;
        },
        std::bit_cast<LPARAM>(&request)
    );
}
#endif

/// JUCE application entry point for the desktop GUI build.
class AudioBatchApplication : public juce::JUCEApplication
{
public:
    /// All real startup work happens in initialise, so the constructor has nothing to do.
    AudioBatchApplication() = default;

    /// Reports the application name from the generated project info.
    const juce::String getApplicationName() override
    {
        return ProjectInfo::projectName;
    }

    /// Reports the application version from the generated project info.
    const juce::String getApplicationVersion() override
    {
        return ProjectInfo::versionString;
    }

    /// Restricts the app to a single running instance.
    /// Version requests and plugin scan worker processes are exempt.
    /// A version request still prints its output while another instance is running
    /// instead of being forwarded to that instance,
    /// and the running instance launches scan workers from its own executable.
    bool moreThanOneInstanceAllowed() override
    {
        const auto commandLine = getCommandLineParameters();
        const auto independent = isVersionRequest(commandLine) || isPluginScanWorkerCommandLine(commandLine);
#if JUCE_WINDOWS
        const juce::ArgumentList arguments {getApplicationName(), commandLine};
        if (!independent && !arguments.containsOption("--headless|-H")) {
            allowExistingInstanceToActivate();
        }
#endif
        return independent;
    }

    /// Starts the application: sets up file logging, creates the main window,
    /// and either shows it or keeps it hidden when the --headless flag is present.
    /// Window activation is repeated asynchronously to reliably gain focus on first launch.
    /// The --version flag only prints the version line and quits,
    /// before the logger or the window are created.
    /// When launched as a plugin scan worker, only the worker is started instead.
    void initialise(const juce::String& commandLineParameters) override
    {
        if (isVersionRequest(commandLineParameters)) {
            console::printLineToConsole(utils::versionLine(getApplicationName()));
            setApplicationReturnValue(0);
            quit();
            return;
        }

        if (isPluginScanWorkerCommandLine(commandLineParameters)) {
            startPluginScanWorker(commandLineParameters);
            return;
        }

        const juce::ArgumentList arguments {getApplicationName(), commandLineParameters};
        const bool startHeadless = arguments.containsOption("--headless|-H");

        // Initialise the log file in the OS default location, under an "AudioBatch" directory.
        // Mac:     /Users/<username>/Library/Logs/AudioBatch
        // Windows: C:\Users\<username>\AppData\Roaming\AudioBatch
        logger = utils::createDefaultLogger(getApplicationName());

        juce::Logger::setCurrentLogger(logger.get());
        utils::installCrashLogging();

        utils::logSystemInfo();
        if (arguments.size() > 0) {
            utils::logInfo("Args: {}", commandLineParameters);
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

    /// Destroys the main window before detaching the logger,
    /// so teardown messages are still written to the log file.
    void shutdown() override
    {
        mainWindow.reset();
        pluginScanWorker.reset();
        juce::Logger::setCurrentLogger(nullptr);
    }

    /// Releases the GUI while the message loop still accepts cleanup callbacks, then quits.
    /// Teardown is deferred beyond the close-button callback so destroying its window cannot invalidate that callback.
    void systemRequestedQuit() override
    {
        if (quitRequested) {
            return;
        }

        quitRequested = true;
        if (const auto exitCode = getApplicationReturnValue(); exitCode != 0) {
            utils::logInfo("Quit with non-zero exit code: {}", exitCode);
        } else {
            utils::logInfo("Quit");
        }
        if (!juce::MessageManager::callAsync([this] {
                mainWindow.reset();
                quit();
            }))
        {
            quit();
        }
    }

    /// Activates the existing Windows window on a normal relaunch, preserving deliberate headless launches.
    /// macOS keeps its existing platform activation behavior.
    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        utils::logInfo("Another instance started with args: {}", commandLine);
#if JUCE_WINDOWS
        const juce::ArgumentList arguments {getApplicationName(), commandLine};
        if (mainWindow != nullptr && !quitRequested && !arguments.containsOption("--headless|-H")) {
            mainWindow->showAndActivate();
        }
#endif
    }

private:
    /// Runs this process as a plugin scan worker for the main application instance.
    /// The worker gets its own log file and no window,
    /// and the process quits when the main instance disconnects.
    void startPluginScanWorker(const juce::String& commandLineParameters)
    {
        logger = utils::createDefaultLogger(getApplicationName(), "AudioBatchPluginScanner");
        juce::Logger::setCurrentLogger(logger.get());

#if JUCE_MAC
        // Keep the worker out of the Dock while it scans.
        juce::Process::setDockIconVisible(false);
#endif

        pluginScanWorker = std::make_unique<PluginScanWorker>([] { juce::JUCEApplicationBase::quit(); });

        if (!pluginScanWorker->initialiseFromCommandLine(commandLineParameters)) {
            utils::logError("Could not connect the plugin scan worker to the main process");
            setApplicationReturnValue(1);
            quit();
            return;
        }

        utils::logInfo("Plugin scan worker started");
    }

    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<PluginScanWorker> pluginScanWorker;
    std::unique_ptr<juce::FileLogger> logger;

    /// Prevents repeated close requests from scheduling teardown more than once.
    bool quitRequested = false;
};
}  // namespace
}  // namespace audiobatch

START_JUCE_APPLICATION(audiobatch::AudioBatchApplication)
