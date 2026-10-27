//! Application actions, keyboard shortcuts, and the native menu bar.
//!
//! The menu bar has an application menu on macOS, then File, Plugins, Options, and Help.
//! On other platforms the File menu ends with Quit instead.
//! The Plugins menu shows the chain summary and adds the known effects, grouped by vendor.
//! Shortcuts use Command on macOS and Control on Windows and Linux.
//! The app view handles most actions, so they work from the menu bar and the shortcuts alike.

use audiobatch_playback::Player;
use gpui_kit::{App, KeyBinding, Menu, MenuItem, SharedString, SystemMenuType};

use crate::files::REVEAL_LABEL;
use crate::plugins::labels::VendorGroup;

/// Key context of the main view, which scopes the shortcuts that act on the results table.
pub const KEY_CONTEXT: &str = "AudioBatch";

/// Key context of the Audio Settings window.
pub const SETTINGS_KEY_CONTEXT: &str = "AudioSettings";

/// Key context of the Plugin Chain and Plugin Manager windows.
pub const PLUGIN_WINDOW_KEY_CONTEXT: &str = "PluginWindow";

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

/// Clears the custom gain of the selected file.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ClearCustomGain;

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

/// Normalizes the selected file to the normalization target.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct NormalizeSelected;

/// Normalizes every analyzed file in the list to the normalization target, after a confirmation.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct NormalizeAll;

/// Shows or hides the short-term loudness graph over the waveform.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ToggleLoudnessGraph;

/// Turns playing the preview through the plugin chain on or off.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct TogglePluginPreview;

/// Turns latency compensation of the plugin chain preview on or off.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct TogglePreviewLatencyCompensation;

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

/// Opens the Plugin Chain window, or brings it to the front.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct OpenPluginChain;

/// Opens the Plugin Manager window, or brings it to the front.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct OpenPluginManager;

/// Closes the focused Plugin Chain or Plugin Manager window.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ClosePluginWindow;

/// Removes every plugin from the chain.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ClearPluginChain;

/// Processes the selected file with the plugin chain.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ProcessSelected;

/// Processes every analyzed file in the list with the plugin chain, after a confirmation.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ProcessAll;

/// Cancels the plugin processing run, leaving the files in progress unchanged.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct CancelProcessing;

/// Adds a known effect at the end of the plugin chain.
#[derive(Clone, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch, no_json)]
pub struct AddPluginToChain {
    /// Class id of the plugin in hex.
    pub class_id: SharedString,
    /// Path of the plugin's bundle, since the same plugin can be installed in more than one folder.
    pub bundle: SharedString,
}

/// Menu items whose check marks and labels reflect the app state.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct MenuState {
    /// Name of the output device in use.
    pub output_device: Option<String>,
    /// True when the loudness graph is shown.
    pub show_loudness_graph: bool,
    /// True when the preview plays through the plugin chain.
    pub preview_plugin_chain: bool,
    /// True when the plugin chain preview compensates its latency.
    pub compensate_preview_latency: bool,
    /// Normalization target shown in the normalize items, for example "-14.0 LUFS".
    pub normalize_target: String,
    /// Summary of the plugin chain, for example "2 plugins".
    pub chain_summary: String,
    /// The known effects by vendor, for the Add Plugin submenu.
    pub plugin_groups: Vec<VendorGroup>,
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
        KeyBinding::new("escape", ClosePluginWindow, Some(PLUGIN_WINDOW_KEY_CONTEXT)),
        KeyBinding::new(primary!("w"), ClosePluginWindow, Some(PLUGIN_WINDOW_KEY_CONTEXT)),
    ]);
    set_menus(cx, &MenuState::default());
}

/// Builds the menu bar, with check marks on the options and the current output device,
/// and the normalization target in the normalize items.
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
        MenuItem::action(
            format!("Normalize Selected to {}", state.normalize_target),
            NormalizeSelected,
        ),
        MenuItem::action(
            format!("Normalize All Files to {}…", state.normalize_target),
            NormalizeAll,
        ),
        MenuItem::action("Clear Per-file Gain", ClearCustomGain),
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
    menus.push(plugins_menu(state));
    menus.push(
        Menu::new("Options").items([
            MenuItem::action("Show Loudness Graph", ToggleLoudnessGraph).checked(state.show_loudness_graph),
            MenuItem::separator(),
            MenuItem::action("Preview Plugin Chain", TogglePluginPreview).checked(state.preview_plugin_chain),
            MenuItem::action("Compensate Preview Plugin Latency", TogglePreviewLatencyCompensation)
                .checked(state.compensate_preview_latency),
            MenuItem::separator(),
            MenuItem::submenu(Menu::new("Output Device").items(devices)),
            MenuItem::separator(),
            MenuItem::action("Audio Settings…", OpenAudioSettings),
        ]),
    );
    menus.push(Menu::new("Help").items([MenuItem::action("About AudioBatchRs", About)]));
    cx.set_menus(menus);
}

/// Builds the Plugins menu: the chain summary, the chain items, the Add Plugin submenu by vendor,
/// the plugin manager, and processing.
fn plugins_menu(state: &MenuState) -> Menu {
    let vendors: Vec<MenuItem> = state
        .plugin_groups
        .iter()
        .map(|group| {
            let plugins = group.plugins.iter().map(|plugin| {
                MenuItem::action(
                    plugin.name.clone(),
                    AddPluginToChain {
                        class_id: plugin.class_id.to_string().into(),
                        bundle: plugin.bundle.to_string_lossy().into_owned().into(),
                    },
                )
            });
            MenuItem::submenu(Menu::new(group.vendor.clone()).items(plugins))
        })
        .collect();
    let add_plugin = Menu::new("Add Plugin")
        .items(vendors)
        .disabled(state.plugin_groups.is_empty());
    Menu::new("Plugins").items([
        MenuItem::action(format!("Chain: {}", state.chain_summary), OpenPluginChain).disabled(true),
        MenuItem::separator(),
        MenuItem::action("Edit Chain…", OpenPluginChain),
        MenuItem::submenu(add_plugin),
        MenuItem::action("Clear Chain", ClearPluginChain),
        MenuItem::separator(),
        MenuItem::action("Plugin Manager…", OpenPluginManager),
        MenuItem::separator(),
        MenuItem::action("Process Selected With Plugin Chain (output AIFF)", ProcessSelected),
        MenuItem::action("Process All Files With Plugin Chain…", ProcessAll),
        MenuItem::action("Cancel Plugin Processing", CancelProcessing),
    ])
}
