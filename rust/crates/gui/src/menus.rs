//! Application actions, keyboard shortcuts, and the native menu bar.
//!
//! The menu bar has an application menu on macOS, then File, Plugins, Options, and Help.
//! On other platforms the File menu ends with Quit instead.
//! The Plugins menu shows the chain summary and adds the known effects, grouped by vendor.
//! Shortcuts use Command on macOS and Control on Windows.
//! The app view handles most actions, so they work from the menu bar and the shortcuts alike.

use audiobatch_playback::Player;
#[cfg(target_os = "windows")]
use gpui_kit::base::GlobalState;
use gpui_kit::{App, KeyBinding, Menu, MenuItem, SharedString, SystemMenuType};

use crate::files::{OPEN_PARENT_FOLDER_LABEL, REVEAL_LABEL};
use crate::plugins::labels::VendorGroup;
use crate::settings::PreviewViewMode;

/// Key context of the main view, which scopes the shortcuts that act on the results table.
pub const KEY_CONTEXT: &str = "AudioBatch";

/// Context of the shortcuts whose keys also edit text, such as the digits, Space, and Backspace.
/// They are left out while a text input of the main view has focus, so those keys edit its text instead.
const OUTSIDE_TEXT_KEY_CONTEXT: &str = "AudioBatch && !Input";

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

/// Shows the selected files in the platform file manager, one window per parent folder.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct RevealSelected;

/// Opens the parent folders of the selected files in the platform file manager.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct OpenParentFolder;

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

/// Plays the selected file from a Serato hot cue, from the keys 1 to 8.
#[derive(Clone, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch, no_json)]
pub struct JumpToHotCue {
    /// Zero-based hot cue slot.
    pub slot: u8,
}

/// Turns colouring the waveform from the Serato Overview tag on or off.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ToggleSeratoColours;

/// Shows or hides the embedded album artwork beside the file name.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ToggleAlbumArtwork;

/// Shows or hides custom-gain predictions beside the measured metrics.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ToggleGainAdjustedMetrics;

/// Selects the source visualization and arrangement shown in the preview.
#[non_exhaustive]
#[derive(Clone, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch, no_json)]
pub struct SetPreviewViewMode {
    /// Source visualization and arrangement to show.
    pub mode: PreviewViewMode,
}

/// Shows or hides source sample-peak overload indicators.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ToggleSampleOverloads;

/// Shows or hides source true-peak overload indicators.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ToggleTruePeakOverloads;

/// Extends enabled overload indicators as vertical lines, or returns to edge markers only.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ToggleOverloadLines;

/// Collapses the preview into its compact row, or expands it again.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct TogglePreviewSection;

/// Shows or hides a results table column.
#[derive(Clone, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch, no_json)]
pub struct ToggleColumn {
    /// Identifier of the column.
    pub id: SharedString,
}

/// Selects every file in the list.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct SelectAll;

/// Moves the selected file to the system trash after a confirmation.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct TrashSelected;

/// Moves the selected file to the system trash without asking, from the shortcut with Shift held.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct TrashSelectedNow;

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

/// Turns continuing active playback on a newly selected file on or off.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ToggleContinuePlayback;

/// Turns starting a loaded file at its first cue point on or off.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct TogglePlayFromFirstCuePoint;

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

/// Rescans VST3 and CLAP plugins in the configured search folders.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct RescanPlugins;

/// Closes the focused Plugin Chain or Plugin Manager window.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ClosePluginWindow;

/// Removes every plugin from the chain.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ClearPluginChain;

/// Opens the Plugin Chain window to save the chain as a preset.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct SaveChainPreset;

/// Opens the chain presets folder in the platform file manager, creating it first.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ShowPresetsFolder;

/// Replaces the plugin chain with a saved preset.
#[derive(Clone, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch, no_json)]
pub struct LoadChainPreset {
    /// Name of the preset.
    pub name: SharedString,
}

/// Processes the selected file with the plugin chain.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ProcessSelected;

/// Processes every analyzed file in the list with the plugin chain, after a confirmation.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct ProcessAll;

/// Cancels the running analysis, normalization, or plugin processing,
/// leaving the files it has not committed unchanged.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
pub struct CancelRun;

/// Adds a known effect at the end of the plugin chain.
#[derive(Clone, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch, no_json)]
pub struct AddPluginToChain {
    /// Plugin API namespace, so equal text ids from different formats cannot collide.
    pub format: audiobatch_plugins::PluginFormat,
    /// Original plugin identifier, hexadecimal for VST3 and descriptor text for CLAP.
    pub class_id: SharedString,
    /// Path of the plugin's bundle, since the same plugin can be installed in more than one folder.
    pub bundle: SharedString,
}

/// Menu items whose check marks and labels reflect the app state.
#[non_exhaustive]
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct MenuState {
    /// Name of the output device in use.
    pub output_device: Option<String>,
    /// Source visualization and arrangement shown in the preview.
    pub preview_view_mode: PreviewViewMode,
    /// True when source sample-peak overload indicators are shown.
    pub show_sample_overloads: bool,
    /// True when source true-peak overload indicators are shown.
    pub show_true_peak_overloads: bool,
    /// True when enabled overload indicators extend as vertical lines.
    pub show_overload_lines: bool,
    /// True when the loudness graph is shown.
    pub show_loudness_graph: bool,
    /// True when the Serato Overview colours the waveform.
    pub serato_colours: bool,
    /// True when the album artwork shows beside the file name.
    pub show_album_artwork: bool,
    /// True when custom-gain estimates show beside the source measurements.
    pub show_gain_adjusted_metrics: bool,
    /// True while the preview is collapsed into its compact row.
    pub preview_compact: bool,
    /// True when the preview plays through the plugin chain.
    pub preview_plugin_chain: bool,
    /// True when the plugin chain preview compensates its latency.
    pub compensate_preview_latency: bool,
    /// True when active playback continues on a newly selected file.
    pub continue_playback: bool,
    /// True when a loaded file starts at its first cue point in the first half of the file.
    pub play_from_first_cue_point: bool,
    /// Normalization target shown in the normalize items, for example "-14.0 LUFS".
    pub normalize_target: String,
    /// Summary of the plugin chain, for example "2 plugins".
    pub chain_summary: String,
    /// The known effects by vendor, for the Add Plugin submenu.
    pub plugin_groups: Vec<VendorGroup>,
    /// True while a plugin scan is running, disabling duplicate scan commands.
    pub scanning_plugins: bool,
    /// Names of the saved chain presets, for the Chain Presets submenu.
    pub presets: Vec<String>,
    /// Name of the preset the chain matches, which the submenu checks.
    /// It is `None` once the chain was modified since the preset was loaded or saved.
    pub current_preset: Option<String>,
    /// The results table columns the user can show or hide, for the Columns submenu.
    pub columns: Vec<ColumnChoice>,
}

/// One results table column in the Columns submenu.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ColumnChoice {
    /// Identifier of the column.
    pub id: SharedString,
    /// Title of the column.
    pub title: &'static str,
    /// True while the column shows.
    pub visible: bool,
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
    crate::controls::install_slider_bindings(cx);
    let context = Some(KEY_CONTEXT);
    let outside_text = Some(OUTSIDE_TEXT_KEY_CONTEXT);
    cx.bind_keys([
        KeyBinding::new(primary!("q"), Quit, None),
        KeyBinding::new(primary!("i"), About, None),
        KeyBinding::new(primary!("o"), ChooseFolder, None),
        KeyBinding::new(primary!("shift-o"), AddFolder, None),
        KeyBinding::new("f5", Rescan, None),
        KeyBinding::new(primary!("."), CancelRun, context),
        KeyBinding::new(primary!("r"), RevealSelected, context),
        KeyBinding::new(primary!("a"), SelectAll, context),
        KeyBinding::new(primary!("shift-s"), ToggleSeratoColours, context),
        KeyBinding::new(primary!("shift-space"), TogglePreviewSection, context),
        KeyBinding::new("1", JumpToHotCue { slot: 0 }, outside_text),
        KeyBinding::new("2", JumpToHotCue { slot: 1 }, outside_text),
        KeyBinding::new("3", JumpToHotCue { slot: 2 }, outside_text),
        KeyBinding::new("4", JumpToHotCue { slot: 3 }, outside_text),
        KeyBinding::new("5", JumpToHotCue { slot: 4 }, outside_text),
        KeyBinding::new("6", JumpToHotCue { slot: 5 }, outside_text),
        KeyBinding::new("7", JumpToHotCue { slot: 6 }, outside_text),
        KeyBinding::new("8", JumpToHotCue { slot: 7 }, outside_text),
        KeyBinding::new("delete", RemoveSelected, outside_text),
        KeyBinding::new("backspace", RemoveSelected, outside_text),
        KeyBinding::new(primary!("backspace"), TrashSelected, outside_text),
        KeyBinding::new(primary!("shift-backspace"), TrashSelectedNow, outside_text),
        KeyBinding::new("space", TogglePlayback, outside_text),
        KeyBinding::new("escape", StopPlayback, outside_text),
        KeyBinding::new(primary!("p"), OpenAudioSettings, context),
        KeyBinding::new(primary!("e"), OpenPluginChain, context),
        KeyBinding::new("escape", CloseAudioSettings, Some(SETTINGS_KEY_CONTEXT)),
        KeyBinding::new(primary!("w"), CloseAudioSettings, Some(SETTINGS_KEY_CONTEXT)),
        KeyBinding::new("escape", ClosePluginWindow, Some(PLUGIN_WINDOW_KEY_CONTEXT)),
        KeyBinding::new(primary!("w"), ClosePluginWindow, Some(PLUGIN_WINDOW_KEY_CONTEXT)),
    ]);
    set_menus(cx, &MenuState::default());
}

/// Builds the menu bar, with check marks on the options and the current output device,
/// and the normalization target in the normalize items.
#[cfg_attr(
    not(target_os = "windows"),
    expect(
        clippy::needless_pass_by_ref_mut,
        reason = "only Windows mutates the app, to hand the menus to its in-window menu bar"
    )
)]
pub fn set_menus(cx: &mut App, state: &MenuState) {
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
    menus.push(file_menu(state));
    menus.push(plugins_menu(state));
    menus.push(options_menu(state));
    menus.push(Menu::new("Help").items([MenuItem::action("About AudioBatchRs", About)]));
    cx.set_menus(menus);
    #[cfg(target_os = "windows")]
    {
        let menus = cx.get_menus().unwrap_or_default();
        GlobalState::global_mut(cx).set_app_menus(menus);
    }
}

/// Builds the File menu, which ends with Quit outside macOS.
fn file_menu(state: &MenuState) -> Menu {
    let mut file_items = vec![
        MenuItem::action("Choose Folder…", ChooseFolder),
        MenuItem::action("Add Folder…", AddFolder),
        MenuItem::action("Add Files…", AddFiles),
        MenuItem::action("Rescan", Rescan),
        MenuItem::separator(),
        MenuItem::action(REVEAL_LABEL, RevealSelected),
        MenuItem::action(OPEN_PARENT_FOLDER_LABEL, OpenParentFolder),
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
        MenuItem::action("Select All Files", SelectAll),
        MenuItem::action("Remove Selected from List", RemoveSelected),
        MenuItem::action("Move Selected to Trash", TrashSelected),
        MenuItem::action("Clear All Files", ClearList),
        MenuItem::separator(),
        MenuItem::action("Cancel Run", CancelRun),
    ];
    if !cfg!(target_os = "macos") {
        file_items.push(MenuItem::separator());
        file_items.push(MenuItem::action("Quit", Quit));
    }
    Menu::new("File").items(file_items)
}

/// Builds the Options menu with check marks on the preview options and the current output device.
fn options_menu(state: &MenuState) -> Menu {
    // Nonvisual tests must not initialize audio drivers, including during menu refreshes.
    let devices = if cfg!(test) {
        Vec::new()
    } else {
        Player::output_device_names()
    };
    let devices = devices
        .into_iter()
        .map(|name| {
            let checked = state.output_device.as_deref() == Some(name.as_str());
            MenuItem::action(name.clone(), SelectOutputDevice { name: name.into() }).checked(checked)
        })
        .collect::<Vec<_>>();
    Menu::new("Options").items([
        MenuItem::submenu(Menu::new("Preview view").items(PreviewViewMode::ALL.map(|mode| {
            MenuItem::action(mode.label(), SetPreviewViewMode { mode }).checked(state.preview_view_mode == mode)
        }))),
        MenuItem::submenu(Menu::new("Overload indicators").items([
            MenuItem::action("Peak overloads", ToggleSampleOverloads).checked(state.show_sample_overloads),
            MenuItem::action("True-peak overloads", ToggleTruePeakOverloads).checked(state.show_true_peak_overloads),
            MenuItem::action("Overload vertical lines", ToggleOverloadLines).checked(state.show_overload_lines),
        ])),
        MenuItem::action("Show Loudness Graph", ToggleLoudnessGraph).checked(state.show_loudness_graph),
        MenuItem::action("Serato Waveform Colours", ToggleSeratoColours).checked(state.serato_colours),
        MenuItem::action("Show Album Artwork", ToggleAlbumArtwork).checked(state.show_album_artwork),
        MenuItem::action("Show custom gain estimates", ToggleGainAdjustedMetrics)
            .checked(state.show_gain_adjusted_metrics),
        MenuItem::submenu(Menu::new("Columns").items(state.columns.iter().map(|column| {
            MenuItem::action(column.title, ToggleColumn { id: column.id.clone() }).checked(column.visible)
        }))),
        MenuItem::action(
            if state.preview_compact {
                "Expand Bottom Section"
            } else {
                "Collapse Bottom Section"
            },
            TogglePreviewSection,
        ),
        MenuItem::separator(),
        MenuItem::action("Preview Plugin Chain", TogglePluginPreview).checked(state.preview_plugin_chain),
        MenuItem::action("Compensate Preview Plugin Latency", TogglePreviewLatencyCompensation)
            .checked(state.compensate_preview_latency),
        MenuItem::action("Continue Playback When Changing Files", ToggleContinuePlayback)
            .checked(state.continue_playback),
        MenuItem::action("Play from First Cue Point", TogglePlayFromFirstCuePoint)
            .checked(state.play_from_first_cue_point),
        MenuItem::separator(),
        MenuItem::submenu(Menu::new("Output Device").items(devices)),
        MenuItem::separator(),
        MenuItem::action("Audio Settings…", OpenAudioSettings),
    ])
}

/// Builds the Chain Presets submenu: saving the chain, showing the presets folder,
/// then the saved presets with the current one checked.
fn presets_menu(state: &MenuState) -> Menu {
    let mut items = vec![
        MenuItem::action("Save Chain as Preset…", SaveChainPreset),
        MenuItem::action("Show Presets Folder", ShowPresetsFolder),
    ];
    if !state.presets.is_empty() {
        items.push(MenuItem::separator());
    }
    items.extend(state.presets.iter().map(|name| {
        let checked = state.current_preset.as_deref() == Some(name.as_str());
        MenuItem::action(
            name.clone(),
            LoadChainPreset {
                name: name.clone().into(),
            },
        )
        .checked(checked)
    }));
    Menu::new("Chain Presets").items(items)
}

/// Builds the Plugins menu: the chain summary, the chain items, the Add Plugin submenu by vendor,
/// the chain presets, the plugin manager, and processing.
fn plugins_menu(state: &MenuState) -> Menu {
    let vendors: Vec<MenuItem> = state
        .plugin_groups
        .iter()
        .map(|group| {
            let plugins = group.plugins.iter().map(|plugin| {
                MenuItem::action(
                    format!("{} ({})", plugin.name, plugin.format),
                    AddPluginToChain {
                        format: plugin.format,
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
        MenuItem::submenu(presets_menu(state)),
        MenuItem::action("Clear Chain", ClearPluginChain),
        MenuItem::separator(),
        MenuItem::action("Plugin Manager…", OpenPluginManager),
        MenuItem::action("Rescan VST3 and CLAP plugins", RescanPlugins).disabled(state.scanning_plugins),
        MenuItem::separator(),
        MenuItem::action("Process Selected With Plugin Chain", ProcessSelected),
        MenuItem::action("Process All Files With Plugin Chain", ProcessAll),
    ])
}

#[cfg(test)]
mod test_preview_menus {
    use gpui_kit::{Keystroke, OwnedMenu, OwnedMenuItem, TestAppContext};

    use super::*;

    /// Finds a submenu by its canonical name in the installed Options menu.
    fn submenu(options: &OwnedMenu, name: &str) -> OwnedMenu {
        options
            .items
            .iter()
            .find_map(|item| match item {
                OwnedMenuItem::Submenu(menu) if menu.name == name => Some(menu.clone()),
                _ => None,
            })
            .expect("the Options submenu exists")
    }

    /// Reads the native Options menu installed by the production builder.
    fn options_menu(cx: &App) -> OwnedMenu {
        cx.get_menus()
            .expect("menus are installed")
            .into_iter()
            .find(|menu| menu.name == "Options")
            .expect("the Options menu exists")
    }

    #[expect(
        clippy::needless_pass_by_ref_mut,
        reason = "the GPUI test macro provides a mutable test context"
    )]
    #[gpui_kit::test]
    fn folder_and_settings_shortcuts_remain_distinct_and_settings_displays_p(cx: &mut TestAppContext) {
        cx.update(|cx| {
            gpui_kit::init(cx);
            install(cx);
            let bindings = cx.key_bindings();
            let bindings = bindings.borrow();
            let folder_key = Keystroke::parse(primary!("o")).expect("valid folder shortcut");
            let settings_key = Keystroke::parse(primary!("p")).expect("valid settings shortcut");
            assert!(
                bindings
                    .bindings_for_action(&ChooseFolder)
                    .any(|binding| { binding.match_keystrokes(std::slice::from_ref(&folder_key)) == Some(false) })
            );
            let settings = bindings
                .bindings_for_action(&OpenAudioSettings)
                .next_back()
                .expect("settings shortcut");
            assert_eq!(settings.match_keystrokes(&[settings_key]), Some(false));
            assert!(bindings.bindings_for_action(&ChooseFolder).all(|binding| {
                binding.match_keystrokes(&[Keystroke::parse(primary!("p")).expect("valid shortcut")]) != Some(false)
            }));
            assert_eq!(
                bindings.bindings_for_action(&OpenAudioSettings).count(),
                1,
                "Audio Settings has only the shared primary-P shortcut"
            );
        });
    }

    #[expect(
        clippy::needless_pass_by_ref_mut,
        reason = "the GPUI test macro provides a mutable test context"
    )]
    #[gpui_kit::test]
    fn shared_shortcuts_bind_select_all_add_folder_and_stop(cx: &mut TestAppContext) {
        cx.update(|cx| {
            gpui_kit::init(cx);
            install(cx);
            let bindings = cx.key_bindings();
            let bindings = bindings.borrow();
            let bound = |action: &dyn gpui_kit::Action, keystroke: &'static str| {
                let keystroke = Keystroke::parse(keystroke).expect("valid shortcut");
                bindings
                    .bindings_for_action(action)
                    .any(|binding| binding.match_keystrokes(std::slice::from_ref(&keystroke)) == Some(false))
            };
            assert!(bound(&SelectAll, primary!("a")));
            assert!(bound(&AddFolder, primary!("shift-o")));
            assert!(bound(&StopPlayback, "escape"));
            let comma = Keystroke::parse(primary!(",")).expect("valid shortcut");
            assert!(
                bindings
                    .bindings()
                    .all(|binding| binding.match_keystrokes(std::slice::from_ref(&comma)) != Some(false)),
                "primary-comma is not bound, Audio Settings uses primary-P"
            );
        });
    }

    #[expect(
        clippy::needless_pass_by_ref_mut,
        reason = "the GPUI test macro provides a mutable test context"
    )]
    #[gpui_kit::test]
    fn playback_options_and_presets_folder_are_in_the_menus(cx: &mut TestAppContext) {
        cx.update(|cx| {
            gpui_kit::init(cx);
            install(cx);
            for enabled in [false, true] {
                set_menus(
                    cx,
                    &MenuState {
                        continue_playback: enabled,
                        play_from_first_cue_point: !enabled,
                        ..MenuState::default()
                    },
                );
                let options = options_menu(cx);
                let checked = |name: &str| {
                    options.items.iter().find_map(|item| match item {
                        OwnedMenuItem::Action {
                            name: item_name,
                            checked,
                            ..
                        } if item_name == name => Some(*checked),
                        _ => None,
                    })
                };
                assert_eq!(checked("Continue Playback When Changing Files"), Some(enabled));
                assert_eq!(checked("Play from First Cue Point"), Some(!enabled));
            }
            let plugins = cx
                .get_menus()
                .expect("menus installed")
                .into_iter()
                .find(|menu| menu.name == "Plugins")
                .expect("Plugins menu exists");
            let presets = submenu(&plugins, "Chain Presets");
            assert!(presets.items.iter().any(|item| matches!(item,
                OwnedMenuItem::Action { name, action, .. }
                if name == "Show Presets Folder" && action.as_any().is::<ShowPresetsFolder>()
            )));
        });
    }

    #[expect(
        clippy::needless_pass_by_ref_mut,
        reason = "the GPUI test macro provides a mutable test context"
    )]
    #[gpui_kit::test]
    fn plugin_menu_namespaces_equal_text_ids_by_format(cx: &mut TestAppContext) {
        cx.update(|cx| {
            gpui_kit::init(cx);
            install(cx);
            let vst3 = crate::test_helpers::effect("Tests", "Equal");
            let mut clap = vst3.clone();
            clap.format = audiobatch_plugins::PluginFormat::Clap;
            clap.class_id = audiobatch_plugins::PluginId::Clap(vst3.class_id.to_string());
            set_menus(
                cx,
                &MenuState {
                    plugin_groups: vec![VendorGroup {
                        vendor: "Tests".to_owned(),
                        plugins: vec![vst3, clap],
                    }],
                    ..MenuState::default()
                },
            );
            let menu = cx
                .get_menus()
                .expect("menus")
                .into_iter()
                .find(|menu| menu.name == "Plugins")
                .expect("Plugins");
            let vendor = submenu(&submenu(&menu, "Add Plugin"), "Tests");
            let actions: Vec<_> = vendor
                .items
                .iter()
                .map(|item| {
                    let OwnedMenuItem::Action { name, action, .. } = item else {
                        panic!("plugin action")
                    };
                    let action = action
                        .as_any()
                        .downcast_ref::<AddPluginToChain>()
                        .expect("add plugin action");
                    (name.as_str(), action.clone())
                })
                .collect();
            let (vst3_name, vst3) = actions.first().expect("VST3 action");
            let (clap_name, clap) = actions.get(1).expect("CLAP action");
            assert_eq!(*vst3_name, "Equal (VST3)");
            assert_eq!(*clap_name, "Equal (CLAP)");
            assert_eq!(vst3.class_id, clap.class_id);
            assert_eq!(vst3.bundle, clap.bundle);
            assert_ne!(vst3, clap);
        });
    }

    #[expect(
        clippy::needless_pass_by_ref_mut,
        reason = "the GPUI test macro provides a mutable test context"
    )]
    #[gpui_kit::test]
    fn plugin_rescan_menu_tracks_scan_state(cx: &mut TestAppContext) {
        cx.update(|cx| {
            gpui_kit::init(cx);
            install(cx);
            for scanning in [false, true] {
                set_menus(
                    cx,
                    &MenuState {
                        scanning_plugins: scanning,
                        ..MenuState::default()
                    },
                );
                let menu = cx
                    .get_menus()
                    .expect("menus installed")
                    .into_iter()
                    .find(|menu| menu.name == "Plugins")
                    .expect("Plugins menu exists");
                assert!(menu.items.iter().any(|item| matches!(item,
                    OwnedMenuItem::Action { name, action, disabled, .. }
                    if name == "Rescan VST3 and CLAP plugins" && action.as_any().is::<RescanPlugins>() && *disabled == scanning
                )));
            }
        });
    }

    #[expect(
        clippy::needless_pass_by_ref_mut,
        reason = "the GPUI test macro provides a mutable test context"
    )]
    #[gpui_kit::test]
    fn preview_view_menu_checks_exactly_the_canonical_mode(cx: &mut TestAppContext) {
        cx.update(|cx| {
            gpui_kit::init(cx);
            install(cx);
            for mode in PreviewViewMode::ALL {
                set_menus(
                    cx,
                    &MenuState {
                        preview_view_mode: mode,
                        ..MenuState::default()
                    },
                );
                let menu = submenu(&options_menu(cx), "Preview view");
                assert_eq!(menu.items.len(), PreviewViewMode::ALL.len());
                for (item, candidate) in menu.items.iter().zip(PreviewViewMode::ALL) {
                    let OwnedMenuItem::Action {
                        name,
                        action,
                        checked,
                        disabled,
                        ..
                    } = item
                    else {
                        unreachable!("preview choices are actions")
                    };
                    assert_eq!(name, candidate.label());
                    assert_eq!(
                        action
                            .as_any()
                            .downcast_ref::<SetPreviewViewMode>()
                            .expect("preview action")
                            .mode,
                        candidate
                    );
                    assert_eq!(*checked, candidate == mode);
                    assert!(!disabled);
                }
            }
        });
    }

    #[expect(
        clippy::needless_pass_by_ref_mut,
        reason = "the GPUI test macro provides a mutable test context"
    )]
    #[gpui_kit::test]
    fn overload_menu_retains_all_eight_independent_checkmark_combinations(cx: &mut TestAppContext) {
        cx.update(|cx| {
            gpui_kit::init(cx);
            install(cx);
            for sample in [false, true] {
                for true_peak in [false, true] {
                    for lines in [false, true] {
                        set_menus(
                            cx,
                            &MenuState {
                                show_sample_overloads: sample,
                                show_true_peak_overloads: true_peak,
                                show_overload_lines: lines,
                                ..MenuState::default()
                            },
                        );
                        let menu = submenu(&options_menu(cx), "Overload indicators");
                        assert_eq!(menu.items.len(), 3);
                        for (item, expected) in menu.items.iter().zip([sample, true_peak, lines]) {
                            let OwnedMenuItem::Action { checked, disabled, .. } = item else {
                                unreachable!("overload choices are actions")
                            };
                            assert_eq!(*checked, expected);
                            assert!(!disabled);
                        }
                    }
                }
            }
        });
    }
}
