/// Implementation of reusable window, label, and alert operations.
/// Keeps Windows foreground-permission handling and its private process-query state out of application classes,
/// and provides consistent label configuration and failure summaries without depending on a specific theme.

#include "utils/GuiUtils.h"

#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <cstddef>

#if JUCE_WINDOWS
#include <array>
#include <bit>
#include <memory>
#include <windows.h>
#endif

namespace audiobatch::utils
{
namespace
{
#if JUCE_WINDOWS
/// Identifies the existing executable without granting foreground permission to unrelated applications.
struct ForegroundPermissionRequest {
    /// Name of this application, which the window titles of the existing instance start with.
    juce::String applicationName;
    /// Path of the executable of this application, which the existing instance must run from.
    juce::String executablePath;
    /// Id of this process, whose own windows are skipped.
    DWORD currentProcessId;
};
#endif

/// Formats a bounded list for an alert, summarizing additional failures rather than growing the dialog indefinitely.
juce::String buildFailureListMessage(const juce::StringArray& failures)
{
    constexpr int maximumListedFailures = 8;
    juce::StringArray lines;
    const auto linesToShow = juce::jmin(maximumListedFailures, failures.size());
    for (int index = 0; index < linesToShow; ++index) {
        lines.add(format("- {}", failures[index]));
    }
    if (failures.size() > linesToShow) {
        lines.add(format("- ...and {} more", failures.size() - linesToShow));
    }
    return lines.joinIntoString("\n");
}
}  // namespace

/// Transfers a user launch's foreground permission before JUCE forwards to an existing instance on Windows.
/// Matches both the main-window title and executable path to exclude unrelated applications and scan workers.
/// Other platforms retain their native activation behavior and need no permission transfer.
void allowExistingInstanceToActivate(const juce::String& applicationName)
{
#if JUCE_WINDOWS
    const ForegroundPermissionRequest request {
        .applicationName = applicationName,
        .executablePath = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
        .currentProcessId = GetCurrentProcessId(),
    };
    EnumWindows(
        [](HWND window, const LPARAM context) -> BOOL {
            const auto& [requestedApplicationName, executablePath, currentProcessId]
                = *std::bit_cast<const ForegroundPermissionRequest*>(context);
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId == 0 || processId == currentProcessId) {
                return TRUE;
            }
            constexpr std::size_t maximumWindowTitleLength = 256;
            std::array<wchar_t, maximumWindowTitleLength> windowTitle {};
            if (GetWindowTextW(window, windowTitle.data(), static_cast<int>(windowTitle.size())) == 0
                || juce::String(windowTitle.data()).upToFirstOccurrenceOf(" - ", false, false)
                    != requestedApplicationName)
            {
                return TRUE;
            }
            const std::unique_ptr<void, decltype(&CloseHandle)> process {
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId), &CloseHandle
            };
            if (process == nullptr) {
                return TRUE;
            }
            constexpr std::size_t maximumWindowsPathLength = 32'768;
            std::array<wchar_t, maximumWindowsPathLength> executablePathBuffer {};
            auto length = static_cast<DWORD>(executablePathBuffer.size());
            if (QueryFullProcessImageNameW(process.get(), 0, executablePathBuffer.data(), &length) != 0
                && executablePath.equalsIgnoreCase(juce::String(executablePathBuffer.data(), length)))
            {
                if (AllowSetForegroundWindow(processId) == 0) {
                    logDebug("Windows did not transfer foreground permission to the existing instance");
                }
                return FALSE;
            }
            return TRUE;
        },
        std::bit_cast<LPARAM>(&request)
    );
#else
    juce::ignoreUnused(applicationName);
#endif
}

/// Brings a visible window forward with keyboard focus on the message thread.
/// Windows also needs an explicit foreground request, which JUCE's toFront and grabFocus do not perform.
/// Showing the window, restoring it, and selecting the focused child remain the caller's responsibility.
void bringWindowToFront(juce::TopLevelWindow& window)
{
    window.toFront(true);
    if (auto* peer = window.getPeer()) {
#if JUCE_WINDOWS
        auto* const windowHandle = static_cast<HWND>(peer->getNativeHandle());
        if (SetForegroundWindow(windowHandle) == 0 && GetForegroundWindow() != windowHandle) {
            logWarn("Windows did not grant foreground activation to window {}", window.getName().quoted());
        }
#endif
        peer->grabFocus();
    }
}

/// Configures a read-only label without compressing long text to fit its available width.
void styleLabel(juce::Label& label, const juce::Font& font, const juce::Colour colour)
{
    label.setFont(font);
    label.setColour(juce::Label::textColourId, colour);
    label.setBorderSize(juce::BorderSize<int>());
    label.setMinimumHorizontalScale(1.0f);
}

/// Shows failures asynchronously on the message thread, bounding multi-file summaries for readability.
/// A single failure is shown verbatim, and an empty collection produces no dialog.
void showFailureSummary(
    const juce::StringArray& failures,
    const juce::String& title,
    const juce::String& failedAction,
    juce::Component* associatedComponent
)
{
    if (failures.isEmpty()) {
        return;
    }
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOk(
            juce::MessageBoxIconType::WarningIcon,
            title,
            failures.size() == 1
                ? failures[0]
                : format("Some files could not be {}:\n\n{}", failedAction, buildFailureListMessage(failures)),
            "OK",
            associatedComponent
        ),
        nullptr
    );
}
}  // namespace audiobatch::utils
