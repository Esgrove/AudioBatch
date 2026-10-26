//! Application actions, keyboard shortcuts, and the native menu bar.
//!
//! The menus follow `MainWindow.cpp` of the C++ application:
//! an application menu on macOS, then File, Options, and Help.
//! On other platforms the File menu ends with Quit instead.
//! Shortcuts use Command on macOS and Control on Windows and Linux.
//! The app view handles most actions, so they work from the menu bar and the shortcuts alike.

use audiobatch_playback::Player;
use gpui_kit::{App, KeyBinding, Menu, MenuItem, SharedString, SystemMenuType};

use crate::files::REVEAL_LABEL;

/// Key context of the main view, which scopes the shortcuts that act on the results table.
pub const KEY_CONTEXT: &str = "AudioBatch";

/// Key context of the Audio Settings window.
pub const SETTINGS_KEY_CONTEXT: &str = "AudioSettings";

/// Quits the application.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct Quit;

/// Shows the version and system information.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct About;

/// Opens a folder dialog whose choice replaces the listed folders.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ChooseFolder;

/// Opens a folder dialog whose choice is added to the listed folders.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct AddFolder;

/// Opens a file dialog whose choice is added to the list.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct AddFiles;

/// Scans the listed folders again, analyzing only new and changed files.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct Rescan;

/// Shows the selected file in the platform file manager.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct RevealSelected;

/// Analyzes the selected file again, ignoring its cached result.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ReanalyzeSelected;

/// Removes the selected file from the list without touching the file.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct RemoveSelected;

/// Moves the selected file to the system trash.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct TrashSelected;

/// Removes every file and folder from the list.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ClearList;

/// Shows or hides the short-term loudness graph over the waveform.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ToggleLoudnessGraph;

/// Opens the Audio Settings window, or brings it to the front.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct OpenAudioSettings;

/// Closes the Audio Settings window.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct CloseAudioSettings;

/// Plays or stops the selected file.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct TogglePlayback;

/// Stops playback and returns to the start.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct StopPlayback;

/// Switches playback to the named output device.
#[derive(Clone, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch, no_json)]
pub struct SelectOutputDevice {
    /// Device name as reported by the audio host.
    pub name: SharedString,
}

/// Menu items whose check marks reflect the app state.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct MenuState {
    /// Name of the output device in use.
    pub output_device: Option<String>,
    /// True when the loudness graph is shown.
    pub show_loudness_graph: bool,
}

/// Prefixes a keystroke with the platform's primary shortcut modifier.
///
/// `KeyBinding::new` needs a static string and does not accept GPUI's `secondary-` alias,
/// so the modifier is chosen at compile time.
macro_rules! primary {
    ($keystroke:literal) => {
        if cfg!(target_os = "macos") {
            concat!("cmd-", $keystroke)
        } else {
            concat!("ctrl-", $keystroke)
        }
    };
}

/// Registers the global actions and shortcuts and installs the menu bar.
pub fn install(cx: &mut App) {
    cx.on_action(|_: &Quit, cx: &mut App| cx.quit());
    let context = Some(KEY_CONTEXT);
    cx.bind_keys([
        KeyBinding::new(primary!("q"), Quit, None),
        KeyBinding::new(primary!("i"), About, None),
        KeyBinding::new(primary!("o"), ChooseFolder, None),
        KeyBinding::new(primary!("shift-o"), AddFolder, None),
        KeyBinding::new("f5", Rescan, None),
        KeyBinding::new(primary!("r"), RevealSelected, context),
        KeyBinding::new("delete", RemoveSelected, context),
        KeyBinding::new("backspace", RemoveSelected, context),
        KeyBinding::new(primary!("backspace"), TrashSelected, context),
        KeyBinding::new("space", TogglePlayback, context),
        KeyBinding::new("escape", StopPlayback, context),
        KeyBinding::new(primary!(","), OpenAudioSettings, context),
        KeyBinding::new("escape", CloseAudioSettings, Some(SETTINGS_KEY_CONTEXT)),
        KeyBinding::new(primary!("w"), CloseAudioSettings, Some(SETTINGS_KEY_CONTEXT)),
    ]);
    set_menus(cx, &MenuState::default());
}

/// Builds the menu bar, with check marks on the loudness graph option and the current output device.
pub fn set_menus(cx: &App, state: &MenuState) {
    let devices = Player::output_device_names()
        .into_iter()
        .map(|name| {
            let checked = state.output_device.as_deref() == Some(name.as_str());
            MenuItem::action(name.clone(), SelectOutputDevice { name: name.into() }).checked(checked)
        })
        .collect::<Vec<_>>();

    let mut file_items = vec![
        MenuItem::action("Choose Folder…", ChooseFolder),
        MenuItem::action("Add Folder…", AddFolder),
        MenuItem::action("Add Files…", AddFiles),
        MenuItem::action("Rescan", Rescan),
        MenuItem::separator(),
        MenuItem::action(REVEAL_LABEL, RevealSelected),
        MenuItem::action("Re-analyze Selected", ReanalyzeSelected),
        MenuItem::action("Remove Selected from List", RemoveSelected),
        MenuItem::action("Move Selected to Trash", TrashSelected),
        MenuItem::action("Clear All Files", ClearList),
    ];
    if !cfg!(target_os = "macos") {
        file_items.push(MenuItem::separator());
        file_items.push(MenuItem::action("Quit", Quit));
    }

    let mut menus = Vec::new();
    if cfg!(target_os = "macos") {
        menus.push(Menu::new("AudioBatchRs").items([
            MenuItem::action("About AudioBatchRs", About),
            MenuItem::separator(),
            MenuItem::os_submenu("Services", SystemMenuType::Services),
            MenuItem::separator(),
            MenuItem::action("Quit AudioBatchRs", Quit),
        ]));
    }
    menus.push(Menu::new("File").items(file_items));
    menus.push(Menu::new("Options").items([
        MenuItem::action("Show Loudness Graph", ToggleLoudnessGraph).checked(state.show_loudness_graph),
        MenuItem::separator(),
        MenuItem::submenu(Menu::new("Output Device").items(devices)),
        MenuItem::separator(),
        MenuItem::action("Audio Settings…", OpenAudioSettings),
    ]));
    menus.push(Menu::new("Help").items([MenuItem::action("About AudioBatchRs", About)]));
    cx.set_menus(menus);
}
