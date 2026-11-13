//! The look of the app: its color palette and its bundled Roboto fonts.
//!
//! The palette and the fonts match those of the C++ application, so both applications look alike on screen.
//! The palette is applied as a gpui-kit theme configuration,
//! so the components and the results table pick it up through their theme colors,
//! while the custom views read the palette constants directly.
//! The fonts are embedded from the repository's shared `assets` directory.
//! It also builds the plain text tooltips the views share, which take their colors from the theme.

use std::borrow::Cow;
use std::rc::Rc;

use gpui_kit::component::tooltip::Tooltip;
use gpui_kit::component::{Theme, ThemeConfig, ThemeConfigColors, ThemeMode};
use gpui_kit::{AnyView, App, Hsla, Rgba, SharedString, Window, hsla, rgba};

/// Proportional text font.
pub const TEXT_FONT: &str = "Roboto";

/// Monospaced font for tabular values.
pub const MONO_FONT: &str = "Roboto Mono";

/// Base font size used by the GPUI theme.
const THEME_FONT_SIZE: f32 = 15.0;

/// Monospaced font size used by the GPUI theme.
const THEME_MONO_FONT_SIZE: f32 = 14.0;

/// General corner radius used by the GPUI theme.
const THEME_RADIUS: usize = 4;

/// Large corner radius used by the GPUI theme.
const THEME_RADIUS_LG: usize = 6;

/// Roboto Regular, from the repository's shared assets.
const ROBOTO: &[u8] = include_bytes!("../../../../assets/Roboto-Regular.ttf");

/// Roboto Mono Regular, from the repository's shared assets.
const ROBOTO_MONO: &[u8] = include_bytes!("../../../../assets/RobotoMono-Regular.ttf");

/// Palette colors shared by the GPUI theme and custom views.
pub mod palette {
    use super::PaletteColor;

    /// The accent for the waveform, selection, and progress.
    pub const BLUE: PaletteColor = PaletteColor::new("#76A8DA");
    /// The hovered accent color.
    pub const BLUE_HOVER: PaletteColor = PaletteColor::new("#8DB7E1");
    /// The active accent color.
    pub const BLUE_ACTIVE: PaletteColor = PaletteColor::new("#5F94C9");
    /// The translucent input selection background.
    pub const BLUE_SELECTION: PaletteColor = PaletteColor::new("#76A8DA3D");
    /// The translucent drag target background.
    pub const BLUE_DROP_TARGET: PaletteColor = PaletteColor::new("#76A8DA26");
    /// The translucent active row background.
    pub const BLUE_ROW_ACTIVE: PaletteColor = PaletteColor::new("#76A8DA38");
    /// The slider track.
    pub const BLUE_SLIDER: PaletteColor = PaletteColor::new("#76A8DAB3");
    /// Link text.
    pub const BLUE_LINK: PaletteColor = PaletteColor::new("#76A8DAE6");
    /// The window background.
    pub const GREY_DARK: PaletteColor = PaletteColor::new("#121315");
    /// The default text.
    pub const GREY_LIGHT: PaletteColor = PaletteColor::new("#DCE0E5");
    /// Outlines and buttons.
    pub const GREY_MEDIUM: PaletteColor = PaletteColor::new("#30343C");
    /// Hovered buttons.
    pub const GREY_BUTTON_HOVER: PaletteColor = PaletteColor::new("#3A3F48");
    /// Active buttons.
    pub const GREY_BUTTON_ACTIVE: PaletteColor = PaletteColor::new("#262A30");
    /// Odd table rows, metric cards, and the waveform background.
    pub const GREY_MEDIUM_DARK: PaletteColor = PaletteColor::new("#202329");
    /// Inactive fills.
    pub const GREY_MIDDLE: PaletteColor = PaletteColor::new("#686F79");
    /// The translucent scrollbar thumb.
    pub const GREY_SCROLLBAR: PaletteColor = PaletteColor::new("#686F7999");
    /// The hovered scrollbar thumb.
    pub const GREY_SCROLLBAR_HOVER: PaletteColor = PaletteColor::new("#808791");
    /// Captions and secondary text.
    pub const GREY_MIDDLE_LIGHT: PaletteColor = PaletteColor::new("#AAB0B9");
    /// Even table rows, the table header, and panels.
    pub const GREY_SEMI_DARK: PaletteColor = PaletteColor::new("#1A1C20");
    /// Emphasized text and values.
    pub const GREY_SUPER_LIGHT: PaletteColor = PaletteColor::new("#F3F5F7");
    /// Errors.
    pub const RED: PaletteColor = PaletteColor::new("#D66565");
    /// Hovered danger buttons.
    pub const RED_HOVER: PaletteColor = PaletteColor::new("#DD7A7A");
    /// Active danger buttons.
    pub const RED_ACTIVE: PaletteColor = PaletteColor::new("#C25353");
    /// Success indicators.
    pub const GREEN_SUCCESS: PaletteColor = PaletteColor::new("#69B786");
    /// Hovered success buttons.
    pub const GREEN_SUCCESS_HOVER: PaletteColor = PaletteColor::new("#7BC495");
    /// Active success buttons.
    pub const GREEN_SUCCESS_ACTIVE: PaletteColor = PaletteColor::new("#58A475");
    /// Source true-peak overloads, distinct from decoded sample-peak warnings.
    pub const ORANGE: PaletteColor = PaletteColor::new("#DD8B65");
    /// Warning indicators.
    pub const YELLOW_WARNING: PaletteColor = PaletteColor::new("#CEAF6C");
    /// The subtle hover background.
    pub const WHITE_HOVER: PaletteColor = PaletteColor::new("#FFFFFF08");
    /// A fully transparent color.
    pub const TRANSPARENT: PaletteColor = PaletteColor::new("#00000000");
    /// Orange red, used for the loudness curve.
    pub const ORANGE_RED: PaletteColor = PaletteColor::new("#FF4500");
}

/// A palette color, written once as a `#RRGGBB` or `#RRGGBBAA` literal.
///
/// The literal is parsed at compile time for custom views and given to the gpui-kit theme unchanged,
/// so an invalid color fails the build.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PaletteColor {
    /// The color as `0xRRGGBBAA`.
    rgba: u32,
    /// The literal the color was defined with.
    hex: &'static str,
}

impl PaletteColor {
    /// Defines a color from an uppercase `#RRGGBB` or `#RRGGBBAA` literal, panicking on any other text.
    // Slice patterns and `assert!` replace indexing and `panic!`, which the workspace lints forbid.
    const fn new(hex: &'static str) -> Self {
        let digits: &[u8] = match hex.as_bytes() {
            [b'#', digits @ ..] => digits,
            _ => &[],
        };
        assert!(
            digits.len() == 6 || digits.len() == 8,
            "palette colors must be #RRGGBB or #RRGGBBAA"
        );
        let mut value: u32 = 0;
        let mut rest = digits;
        while let [byte, tail @ ..] = rest {
            let digit = match *byte {
                b'0'..=b'9' => *byte - b'0',
                b'A'..=b'F' => *byte - b'A' + 10,
                _ => u8::MAX,
            };
            assert!(digit < 16, "palette colors must use uppercase hexadecimal digits");
            value = (value << 4) | digit as u32;
            rest = tail;
        }
        let rgba = if digits.len() == 6 { (value << 8) | 0xFF } else { value };
        Self { rgba, hex }
    }

    /// Returns the gpui-kit theme representation without allocating.
    const fn theme_value(self) -> SharedString {
        SharedString::new_static(self.hex)
    }
}

impl From<PaletteColor> for Rgba {
    fn from(color: PaletteColor) -> Self {
        rgba(color.rgba)
    }
}

/// Returns a palette color, or an opaque color from [`gpui_kit::rgb`], preserving its alpha.
#[must_use]
pub fn color(value: impl Into<Rgba>) -> Hsla {
    value.into().into()
}

/// Returns white with the given opacity, which the results table uses for its text and dividers.
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
/// so the app keeps its dark look when the system appearance changes.
pub fn install(cx: &mut App) {
    if let Err(error) = cx
        .text_system()
        .add_fonts(vec![Cow::Borrowed(ROBOTO), Cow::Borrowed(ROBOTO_MONO)])
    {
        tracing::error!("Failed to load the bundled fonts: {error:#}");
    }
    let dark = theme_config();
    let mut light = dark.clone();
    light.mode = ThemeMode::Light;
    let theme = Theme::global_mut(cx);
    theme.apply_config(&Rc::new(light));
    theme.apply_config(&Rc::new(dark));
    Theme::change(ThemeMode::Dark, None, cx);
}

/// Builds the app palette as a typed gpui-kit theme configuration.
fn theme_config() -> ThemeConfig {
    let mut colors = ThemeConfigColors::default();
    colors.background = Some(palette::GREY_DARK.theme_value());
    colors.foreground = Some(palette::GREY_LIGHT.theme_value());
    colors.border = Some(palette::GREY_MEDIUM.theme_value());
    colors.input = Some(palette::GREY_MEDIUM.theme_value());
    colors.ring = Some(palette::BLUE.theme_value());
    colors.caret = Some(palette::BLUE.theme_value());
    colors.selection = Some(palette::BLUE_SELECTION.theme_value());
    colors.muted = Some(palette::GREY_MEDIUM_DARK.theme_value());
    colors.muted_foreground = Some(palette::GREY_MIDDLE_LIGHT.theme_value());
    colors.accent = Some(palette::GREY_MEDIUM.theme_value());
    colors.accent_foreground = Some(palette::GREY_SUPER_LIGHT.theme_value());
    colors.primary = Some(palette::BLUE.theme_value());
    colors.primary_foreground = Some(palette::GREY_DARK.theme_value());
    colors.primary_hover = Some(palette::BLUE_HOVER.theme_value());
    colors.primary_active = Some(palette::BLUE_ACTIVE.theme_value());
    colors.secondary = Some(palette::GREY_MEDIUM.theme_value());
    colors.secondary_foreground = Some(palette::GREY_SUPER_LIGHT.theme_value());
    colors.secondary_hover = Some(palette::GREY_BUTTON_HOVER.theme_value());
    colors.secondary_active = Some(palette::GREY_BUTTON_ACTIVE.theme_value());
    colors.button = Some(palette::GREY_MEDIUM.theme_value());
    colors.button_foreground = Some(palette::GREY_SUPER_LIGHT.theme_value());
    colors.button_hover = Some(palette::GREY_BUTTON_HOVER.theme_value());
    colors.button_active = Some(palette::GREY_BUTTON_ACTIVE.theme_value());
    colors.button_primary = Some(palette::BLUE.theme_value());
    colors.button_primary_foreground = Some(palette::GREY_DARK.theme_value());
    colors.button_primary_hover = Some(palette::BLUE_HOVER.theme_value());
    colors.button_primary_active = Some(palette::BLUE_ACTIVE.theme_value());
    colors.button_success = Some(palette::GREEN_SUCCESS.theme_value());
    colors.button_success_foreground = Some(palette::GREY_SUPER_LIGHT.theme_value());
    colors.button_success_hover = Some(palette::GREEN_SUCCESS_HOVER.theme_value());
    colors.button_success_active = Some(palette::GREEN_SUCCESS_ACTIVE.theme_value());
    colors.button_danger = Some(palette::RED.theme_value());
    colors.button_danger_foreground = Some(palette::GREY_SUPER_LIGHT.theme_value());
    colors.button_danger_hover = Some(palette::RED_HOVER.theme_value());
    colors.button_danger_active = Some(palette::RED_ACTIVE.theme_value());
    colors.success = Some(palette::GREEN_SUCCESS.theme_value());
    colors.success_foreground = Some(palette::GREY_DARK.theme_value());
    colors.danger = Some(palette::RED.theme_value());
    colors.danger_foreground = Some(palette::GREY_SUPER_LIGHT.theme_value());
    colors.warning = Some(palette::YELLOW_WARNING.theme_value());
    colors.warning_foreground = Some(palette::GREY_DARK.theme_value());
    colors.info = Some(palette::BLUE.theme_value());
    colors.info_foreground = Some(palette::GREY_DARK.theme_value());
    colors.popover = Some(palette::GREY_SEMI_DARK.theme_value());
    colors.popover_foreground = Some(palette::GREY_LIGHT.theme_value());
    colors.drop_target = Some(palette::BLUE_DROP_TARGET.theme_value());
    colors.list = Some(palette::GREY_SEMI_DARK.theme_value());
    colors.list_even = Some(palette::GREY_MEDIUM_DARK.theme_value());
    colors.list_active = Some(palette::BLUE_ROW_ACTIVE.theme_value());
    colors.list_active_border = Some(palette::BLUE.theme_value());
    colors.list_hover = Some(palette::WHITE_HOVER.theme_value());
    colors.list_head = Some(palette::GREY_SEMI_DARK.theme_value());
    colors.table = Some(palette::GREY_SEMI_DARK.theme_value());
    colors.table_even = Some(palette::GREY_MEDIUM_DARK.theme_value());
    colors.table_active = Some(palette::BLUE_ROW_ACTIVE.theme_value());
    colors.table_active_border = Some(palette::BLUE.theme_value());
    colors.table_head = Some(palette::GREY_SEMI_DARK.theme_value());
    colors.table_head_foreground = Some(palette::GREY_LIGHT.theme_value());
    colors.table_hover = Some(palette::WHITE_HOVER.theme_value());
    colors.table_row_border = Some(palette::TRANSPARENT.theme_value());
    colors.scrollbar = Some(palette::TRANSPARENT.theme_value());
    colors.scrollbar_thumb = Some(palette::GREY_SCROLLBAR.theme_value());
    colors.scrollbar_thumb_hover = Some(palette::GREY_SCROLLBAR_HOVER.theme_value());
    colors.slider_bar = Some(palette::BLUE_SLIDER.theme_value());
    colors.slider_thumb = Some(palette::GREY_SUPER_LIGHT.theme_value());
    colors.progress_bar = Some(palette::BLUE.theme_value());
    colors.link = Some(palette::BLUE_LINK.theme_value());
    colors.title_bar = Some(palette::GREY_DARK.theme_value());
    colors.title_bar_border = Some(palette::GREY_MEDIUM.theme_value());
    colors.window_border = Some(palette::GREY_MEDIUM.theme_value());

    let mut config = ThemeConfig::default();
    config.name = "AudioBatch".into();
    config.mode = ThemeMode::Dark;
    config.font_family = Some(TEXT_FONT.into());
    config.font_size = Some(THEME_FONT_SIZE);
    config.mono_font_family = Some(MONO_FONT.into());
    config.mono_font_size = Some(THEME_MONO_FONT_SIZE);
    config.radius = Some(THEME_RADIUS);
    config.radius_lg = Some(THEME_RADIUS_LG);
    config.shadow = Some(false);
    config.colors = colors;
    config
}

#[cfg(test)]
mod test_theme {
    use super::*;

    /// The typed configuration retains the application's typography and geometry.
    #[test]
    fn theme_settings() {
        let config = theme_config();
        assert_eq!(config.name.as_ref(), "AudioBatch");
        assert_eq!(config.mode, ThemeMode::Dark);
        assert_eq!(config.font_family.as_deref(), Some(TEXT_FONT));
        assert_eq!(config.font_size, Some(THEME_FONT_SIZE));
        assert_eq!(config.mono_font_family.as_deref(), Some(MONO_FONT));
        assert_eq!(config.mono_font_size, Some(THEME_MONO_FONT_SIZE));
        assert_eq!(config.radius, Some(THEME_RADIUS));
        assert_eq!(config.radius_lg, Some(THEME_RADIUS_LG));
        assert_eq!(config.shadow, Some(false));
    }

    /// Opaque palette colors are written to the theme as `#RRGGBB`.
    #[test]
    fn opaque_theme_colors() {
        let colors = theme_config().colors;
        for (actual, expected) in [
            (colors.primary, "#76A8DA"),
            (colors.primary_hover, "#8DB7E1"),
            (colors.background, "#121315"),
            (colors.foreground, "#DCE0E5"),
            (colors.border, "#30343C"),
            (colors.danger, "#D66565"),
            (colors.scrollbar_thumb_hover, "#808791"),
        ] {
            assert_eq!(actual.as_deref(), Some(expected));
        }
    }

    /// The typed configuration preserves alpha in translucent theme colors.
    #[test]
    fn translucent_theme_colors() {
        let colors = theme_config().colors;
        for (actual, expected) in [
            (colors.selection, "#76A8DA3D"),
            (colors.drop_target, "#76A8DA26"),
            (colors.list_active, "#76A8DA38"),
            (colors.table_active, "#76A8DA38"),
            (colors.list_hover, "#FFFFFF08"),
            (colors.table_row_border, "#00000000"),
            (colors.scrollbar_thumb, "#686F7999"),
            (colors.slider_bar, "#76A8DAB3"),
            (colors.link, "#76A8DAE6"),
        ] {
            assert_eq!(actual.as_deref(), Some(expected));
        }
    }

    /// Custom views get the same color, including alpha, as the theme.
    #[test]
    fn custom_view_colors_keep_alpha() {
        assert_eq!(color(palette::BLUE), Hsla::from(rgba(0x76A8_DAFF)));
        assert_eq!(color(palette::BLUE_SELECTION), Hsla::from(rgba(0x76A8_DA3D)));
        assert!((color(palette::TRANSPARENT).a).abs() < f32::EPSILON);
        assert!((color(palette::GREY_SCROLLBAR).a - f32::from(0x99_u8) / 255.0).abs() < 1e-6);
    }

    /// Opaque literals parse with full alpha, and translucent ones keep their alpha.
    #[test]
    fn literals_parse_to_rgba() {
        assert_eq!(PaletteColor::new("#76A8DA").rgba, 0x76A8_DAFF);
        assert_eq!(PaletteColor::new("#76A8DA3D").rgba, 0x76A8_DA3D);
        assert_eq!(PaletteColor::new("#00000000").rgba, 0);
        assert_eq!(color(gpui_kit::rgb(0x00FF_4500)), color(palette::ORANGE_RED));
    }

    /// Malformed literals are rejected, which fails the build for palette constants.
    #[test]
    fn malformed_literals_are_rejected() {
        let default_hook = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let accepted: Vec<_> = ["76A8DA", "#76A8D", "#76A8DA3", "#76a8da", "#76A8DG", "#76A8DA3D00"]
            .into_iter()
            .filter(|literal| std::panic::catch_unwind(|| PaletteColor::new(literal)).is_ok())
            .collect();
        std::panic::set_hook(default_hook);
        assert!(accepted.is_empty(), "accepted malformed literals: {accepted:?}");
    }
}
