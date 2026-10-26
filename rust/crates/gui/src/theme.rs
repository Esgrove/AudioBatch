//! The look of the C++ `AudioBatch` application: its color palette and its bundled Roboto fonts.
//!
//! The palette is the one in `CustomLookAndFeel.h`.
//! It is applied as a gpui-kit theme configuration,
//! so the components and the results table pick it up through their theme colors,
//! while the custom views read the palette constants directly.
//! The fonts are embedded from the repository's shared `assets` directory.
//! It also builds the plain text tooltips the views share, which take their colors from the theme.

use std::borrow::Cow;
use std::rc::Rc;

use gpui_kit::component::tooltip::Tooltip;
use gpui_kit::component::{Theme, ThemeConfig, ThemeMode};
use gpui_kit::{AnyView, App, Hsla, SharedString, Window, hsla, rgb};

/// Proportional text font, the same face the C++ application uses.
pub const TEXT_FONT: &str = "Roboto";

/// Monospaced font for tabular values.
pub const MONO_FONT: &str = "Roboto Mono";

/// Roboto Regular, from the repository's shared assets.
const ROBOTO: &[u8] = include_bytes!("../../../../assets/Roboto-Regular.ttf");

/// Roboto Mono Regular, from the repository's shared assets.
const ROBOTO_MONO: &[u8] = include_bytes!("../../../../assets/RobotoMono-Regular.ttf");

/// Palette colors of `CustomLookAndFeel.h` as `0xRRGGBB` values.
pub mod palette {
    /// #76A8DA, the accent for the waveform, selection, and progress.
    pub const BLUE: u32 = 0x0076_A8DA;
    /// #121315, the window background.
    pub const GREY_DARK: u32 = 0x0012_1315;
    /// #DCE0E5, the default text.
    pub const GREY_LIGHT: u32 = 0x00DC_E0E5;
    /// #30343C, outlines and buttons.
    pub const GREY_MEDIUM: u32 = 0x0030_343C;
    /// #202329, odd table rows, metric cards, and the waveform background.
    pub const GREY_MEDIUM_DARK: u32 = 0x0020_2329;
    /// #686F79, inactive fills.
    pub const GREY_MIDDLE: u32 = 0x0068_6F79;
    /// #AAB0B9, captions and secondary text.
    pub const GREY_MIDDLE_LIGHT: u32 = 0x00AA_B0B9;
    /// #1A1C20, even table rows, the table header, and panels.
    pub const GREY_SEMI_DARK: u32 = 0x001A_1C20;
    /// #F3F5F7, emphasized text and values.
    pub const GREY_SUPER_LIGHT: u32 = 0x00F3_F5F7;
    /// #D66565, errors.
    pub const RED: u32 = 0x00D6_6565;
    /// #FF4500, JUCE's `Colours::orangered`, used for the loudness curve.
    pub const ORANGE_RED: u32 = 0x00FF_4500;
}

/// Theme configuration with the palette of the C++ application, in the gpui-kit theme file format.
const THEME_CONFIG: &str = r##"{
    "name": "AudioBatch",
    "mode": "dark",
    "font.family": "Roboto",
    "font.size": 15,
    "mono_font.family": "Roboto Mono",
    "mono_font.size": 14,
    "radius": 4,
    "radius.lg": 6,
    "shadow": false,
    "colors": {
        "background": "#121315",
        "foreground": "#DCE0E5",
        "border": "#30343C",
        "input.border": "#30343C",
        "ring": "#76A8DA",
        "caret": "#76A8DA",
        "selection.background": "#76A8DA3D",
        "muted.background": "#202329",
        "muted.foreground": "#AAB0B9",
        "accent.background": "#30343C",
        "accent.foreground": "#F3F5F7",
        "primary.background": "#76A8DA",
        "primary.foreground": "#121315",
        "primary.hover.background": "#8DB7E1",
        "primary.active.background": "#5F94C9",
        "secondary.background": "#30343C",
        "secondary.foreground": "#F3F5F7",
        "secondary.hover.background": "#3A3F48",
        "secondary.active.background": "#262A30",
        "button.background": "#30343C",
        "button.foreground": "#F3F5F7",
        "button.hover.background": "#3A3F48",
        "button.active.background": "#262A30",
        "button.primary.background": "#76A8DA",
        "button.primary.foreground": "#121315",
        "button.primary.hover.background": "#8DB7E1",
        "button.primary.active.background": "#5F94C9",
        "button.success.background": "#69B786",
        "button.success.foreground": "#F3F5F7",
        "button.success.hover.background": "#7BC495",
        "button.success.active.background": "#58A475",
        "button.danger.background": "#D66565",
        "button.danger.foreground": "#F3F5F7",
        "button.danger.hover.background": "#DD7A7A",
        "button.danger.active.background": "#C25353",
        "success.background": "#69B786",
        "success.foreground": "#121315",
        "danger.background": "#D66565",
        "danger.foreground": "#F3F5F7",
        "warning.background": "#CEAF6C",
        "warning.foreground": "#121315",
        "info.background": "#76A8DA",
        "info.foreground": "#121315",
        "popover.background": "#1A1C20",
        "popover.foreground": "#DCE0E5",
        "drop_target.background": "#76A8DA26",
        "list.background": "#1A1C20",
        "list.even.background": "#202329",
        "list.active.background": "#76A8DA38",
        "list.active.border": "#76A8DA",
        "list.hover.background": "#FFFFFF08",
        "list.head.background": "#1A1C20",
        "table.background": "#1A1C20",
        "table.even.background": "#202329",
        "table.active.background": "#76A8DA38",
        "table.active.border": "#76A8DA",
        "table.head.background": "#1A1C20",
        "table.head.foreground": "#DCE0E5",
        "table.hover.background": "#FFFFFF08",
        "table.row.border": "#00000000",
        "scrollbar.background": "#00000000",
        "scrollbar.thumb.background": "#686F7999",
        "scrollbar.thumb.hover.background": "#808791",
        "slider.background": "#76A8DAB3",
        "slider.thumb.background": "#F3F5F7",
        "progress.bar.background": "#76A8DA",
        "link": "#76A8DAE6",
        "title_bar.background": "#121315",
        "title_bar.border": "#30343C",
        "window.border": "#30343C"
    }
}"##;

/// Returns a palette color.
#[must_use]
pub fn color(value: u32) -> Hsla {
    rgb(value).into()
}

/// Returns white with the given opacity, which the C++ table uses for its text and dividers.
#[must_use]
pub const fn white(alpha: f32) -> Hsla {
    hsla(0.0, 0.0, 1.0, alpha)
}

/// Returns black with the given opacity.
#[must_use]
pub const fn black(alpha: f32) -> Hsla {
    hsla(0.0, 0.0, 0.0, alpha)
}

/// Returns a tooltip builder for a plain text tooltip, for the `tooltip` method of an element with an id.
pub fn tooltip(text: impl Into<SharedString>) -> impl Fn(&mut Window, &mut App) -> AnyView + 'static {
    let text = text.into();
    move |window, cx| Tooltip::new(text.clone()).build(window, cx)
}

/// Embeds the fonts and applies the palette, for both light and dark system appearance,
/// so the app keeps the dark look of the C++ version when the system appearance changes.
pub fn install(cx: &mut App) {
    if let Err(error) = cx
        .text_system()
        .add_fonts(vec![Cow::Borrowed(ROBOTO), Cow::Borrowed(ROBOTO_MONO)])
    {
        eprintln!("Failed to load the bundled fonts: {error:#}");
    }
    let dark: ThemeConfig = match serde_json::from_str(THEME_CONFIG) {
        Ok(config) => config,
        Err(error) => {
            eprintln!("Invalid theme configuration: {error}");
            return;
        }
    };
    let mut light = dark.clone();
    light.mode = ThemeMode::Light;
    let theme = Theme::global_mut(cx);
    theme.apply_config(&Rc::new(light));
    theme.apply_config(&Rc::new(dark));
    Theme::change(ThemeMode::Dark, None, cx);
}
