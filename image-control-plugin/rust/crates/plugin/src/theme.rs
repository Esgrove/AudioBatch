//! Centralized `ImageControl` palette and the shared Roboto faces used by labels and numeric values.

use iced::widget::{button, checkbox, container, pick_list, slider, text_input};
use iced::{Border, Color, Font, Theme};
use iced_baseview as iced;

pub const BLUE: Color = rgb(0x76_a8_da);
pub const GREY_DARK: Color = rgb(0x12_13_15);
pub const GREY_LIGHT: Color = rgb(0xdc_e0_e5);
pub const GREY_MEDIUM: Color = rgb(0x30_34_3c);
pub const GREY_MEDIUM_DARK: Color = rgb(0x20_23_29);
pub const GREY_MIDDLE: Color = rgb(0x68_6f_79);
pub const GREY_MIDDLE_LIGHT: Color = rgb(0xaa_b0_b9);
pub const GREY_SEMI_DARK: Color = rgb(0x1a_1c_20);
pub const GREY_SUPER_LIGHT: Color = rgb(0xf3_f5_f7);
pub const ORANGE: Color = rgb(0xdd_8b_65);
pub const YELLOW: Color = rgb(0xce_af_6c);
pub const TEXT: Font = Font::with_name("Roboto");
pub const MONO: Font = Font::with_name("Roboto Mono");
pub const SEMIBOLD: Font = Font {
    weight: iced::font::Weight::Semibold,
    ..TEXT
};
pub const BOLD: Font = Font {
    weight: iced::font::Weight::Bold,
    ..TEXT
};
pub const ROBOTO: &[u8] = include_bytes!("../../../../../assets/Roboto-Regular.ttf");
pub const ROBOTO_SEMIBOLD: &[u8] = include_bytes!("../../../../../assets/Roboto-SemiBold.ttf");
pub const ROBOTO_BOLD: &[u8] = include_bytes!("../../../../../assets/Roboto-Bold.ttf");
pub const ROBOTO_MONO: &[u8] = include_bytes!("../../../../../assets/RobotoMono-Regular.ttf");

/// Typography is measured in Iced em units, not JUCE ascent-plus-descent heights.
pub const TITLE_SIZE: f32 = 16.0;
pub const BAND_TITLE_SIZE: f32 = 13.0;
pub const LABEL_SIZE: f32 = 12.0;
pub const VALUE_SIZE: f32 = 12.0;
pub const ANNOTATION_SIZE: f32 = 10.0;
pub const SECTION_TITLE_SIZE: f32 = 14.0;
pub const LINE_HEIGHT: f32 = 1.0;
/// Logical radii are independent of host or backing-pixel scale.
pub const PANEL_RADIUS: f32 = 8.0;
pub const CONTROL_RADIUS: f32 = 6.0;
pub const FIELD_RADIUS: f32 = 4.0;
pub const CHECKBOX_SIZE: f32 = 16.0;

/// Native inputs retain the palette while exposing focus and rejected drafts clearly.
pub fn numeric_input(status: text_input::Status, invalid: bool) -> text_input::Style {
    text_input::Style {
        background: GREY_MEDIUM_DARK.into(),
        border: Border {
            radius: FIELD_RADIUS.into(),
            width: 1.0,
            color: if invalid {
                ORANGE
            } else if matches!(status, text_input::Status::Focused { .. }) {
                BLUE
            } else if matches!(status, text_input::Status::Hovered) {
                GREY_MIDDLE
            } else {
                GREY_MEDIUM
            },
        },
        icon: GREY_MIDDLE_LIGHT,
        placeholder: GREY_MIDDLE,
        value: GREY_LIGHT,
        selection: BLUE.scale_alpha(0.3),
    }
}

/// Settings use a bounded surface over the editor, rather than an extra layout row.
pub fn callout(theme: &Theme) -> container::Style {
    panel(theme).border(Border {
        color: GREY_MEDIUM,
        width: 1.0,
        radius: PANEL_RADIUS.into(),
    })
}

/// Styles explicit choices with the same muted background as the C++ combo boxes.
pub fn choice(_: &Theme, _: pick_list::Status) -> pick_list::Style {
    pick_list::Style {
        text_color: GREY_LIGHT,
        placeholder_color: GREY_MIDDLE,
        handle_color: GREY_MIDDLE_LIGHT,
        background: GREY_MEDIUM_DARK.into(),
        border: Border {
            color: GREY_MEDIUM,
            width: 1.0,
            radius: FIELD_RADIUS.into(),
        },
    }
}

/// Link and settings toggles use the plugin palette, not the Iced default blue theme.
pub fn check(_: &Theme, status: checkbox::Status) -> checkbox::Style {
    let checked = match status {
        checkbox::Status::Active { is_checked }
        | checkbox::Status::Hovered { is_checked }
        | checkbox::Status::Disabled { is_checked } => is_checked,
    };
    checkbox::Style {
        background: GREY_SEMI_DARK.into(),
        icon_color: if checked { BLUE } else { GREY_MIDDLE },
        border: Border {
            color: GREY_MIDDLE,
            width: 1.0,
            radius: FIELD_RADIUS.into(),
        },
        text_color: Some(if matches!(status, checkbox::Status::Disabled { .. }) {
            GREY_MIDDLE
        } else {
            GREY_LIGHT
        }),
    }
}

/// Compact values retain the slider's genuine input behavior without drawing tracks behind numeric text.
pub fn compact_slider(theme: &Theme, status: slider::Status) -> slider::Style {
    let mut style = parameter_slider(theme, status);
    style.rail.backgrounds = (Color::TRANSPARENT.into(), Color::TRANSPARENT.into());
    style.handle.background = Color::TRANSPARENT.into();
    style.handle.border_width = 0.0;
    style
}

/// Converts a shared opaque palette entry without a second floating-point source of truth.
const fn rgb(value: u32) -> Color {
    Color::from_rgb(
        (value >> 16 & 255) as f32 / 255.0,
        (value >> 8 & 255) as f32 / 255.0,
        (value & 255) as f32 / 255.0,
    )
}

/// Styles the editor's background and default text, including unavailable analysis states.
pub fn background(_: &Theme) -> container::Style {
    container::Style::default().background(GREY_DARK).color(GREY_LIGHT)
}

/// Gives analysis and band sections their quiet rounded surface.
pub fn panel(_: &Theme) -> container::Style {
    container::Style::default()
        .background(GREY_SEMI_DARK)
        .color(GREY_LIGHT)
        .border(Border {
            radius: PANEL_RADIUS.into(),
            ..Border::default()
        })
}

/// Styles a boxed numeric readout beside its slider.
pub fn value(_: &Theme) -> container::Style {
    container::Style::default()
        .background(GREY_MEDIUM_DARK)
        .color(GREY_LIGHT)
        .border(Border {
            radius: FIELD_RADIUS.into(),
            color: GREY_MEDIUM,
            width: 1.0,
        })
}

/// Distinguishes toggle selection without adding badges or additional status decoration.
pub fn control(status: button::Status, active: bool, accent: Color) -> button::Style {
    let disabled = matches!(status, button::Status::Disabled);
    let background = if active {
        accent
    } else if matches!(status, button::Status::Hovered) {
        GREY_MEDIUM.scale_alpha(0.85)
    } else {
        GREY_MEDIUM
    };
    button::Style {
        background: Some(background.into()),
        text_color: if disabled {
            GREY_MIDDLE
        } else if active && accent == YELLOW {
            GREY_DARK
        } else {
            GREY_SUPER_LIGHT
        },
        border: Border {
            color: GREY_MEDIUM,
            width: 1.0,
            radius: CONTROL_RADIUS.into(),
        },
        ..button::Style::default()
    }
}

/// Uses a quiet six-pixel track and twelve-pixel thumb with native hover and drag feedback.
pub fn parameter_slider(_: &Theme, status: slider::Status) -> slider::Style {
    slider::Style {
        rail: slider::Rail {
            backgrounds: (BLUE.scale_alpha(0.7).into(), GREY_MEDIUM.into()),
            width: 6.0,
            border: Border {
                radius: 2.0.into(),
                ..Border::default()
            },
        },
        handle: slider::Handle {
            shape: slider::HandleShape::Circle { radius: 6.0 },
            background: GREY_SUPER_LIGHT.into(),
            border_width: if matches!(status, slider::Status::Hovered | slider::Status::Dragged) {
                1.0
            } else {
                0.0
            },
            border_color: BLUE,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn embedded_faces_resolve_exact_family_and_weight_without_fallback() {
        use iced::advanced::graphics::text::{cosmic_text::fontdb, font_system};
        let mut system = font_system().write().expect("font system");
        for bytes in [ROBOTO, ROBOTO_SEMIBOLD, ROBOTO_BOLD, ROBOTO_MONO] {
            system.load_font(bytes.into());
        }
        let database = system.raw().db();
        for (family, weight) in [("Roboto", 400), ("Roboto", 600), ("Roboto", 700), ("Roboto Mono", 400)] {
            let id = database
                .query(&fontdb::Query {
                    families: &[fontdb::Family::Name(family)],
                    weight: fontdb::Weight(weight),
                    ..fontdb::Query::default()
                })
                .expect("embedded face");
            let face = database.face(id).expect("resolved face");
            assert_eq!(face.weight, fontdb::Weight(weight));
            assert!(face.families.iter().any(|(name, _)| name == family));
        }
        drop(system);
    }

    #[test]
    fn worst_case_values_and_headings_fit_the_minimum_editor_without_font_compression() {
        use iced::advanced::text::Paragraph as _;
        {
            let mut system = iced::advanced::graphics::text::font_system()
                .write()
                .expect("font system");
            for bytes in [ROBOTO, ROBOTO_SEMIBOLD, ROBOTO_BOLD, ROBOTO_MONO] {
                system.load_font(bytes.into());
            }
        }
        let width = |content: &str, font, size: f32| {
            let paragraph =
                <iced::Renderer as iced::advanced::text::Renderer>::Paragraph::with_text(iced::advanced::text::Text {
                    content,
                    bounds: iced::Size::new(f32::INFINITY, size),
                    size: size.into(),
                    line_height: iced::advanced::text::LineHeight::Relative(LINE_HEIGHT),
                    font,
                    align_x: iced::advanced::text::Alignment::Left,
                    align_y: iced::alignment::Vertical::Center,
                    shaping: iced::advanced::text::Shaping::Basic,
                    wrapping: iced::advanced::text::Wrapping::None,
                });
            paragraph.min_width()
        };
        assert!(width("ImageControl", BOLD, TITLE_SIZE) <= 112.0);
        for value in ["-60.0 dB", "12.0 dB", "-inf dB", "100%", "100L", "2.00"] {
            assert!(
                width(value, MONO, VALUE_SIZE) <= super::super::layout::VALUE_WIDTH - 8.0,
                "{value}"
            );
        }
        for name in super::super::layout::BAND_NAMES {
            assert!(
                width(name, SEMIBOLD, BAND_TITLE_SIZE) + 20.0 + 6.0 + 60.0 <= 160.0,
                "{name}"
            );
        }
        assert!(width("100 Hz to 20.0 kHz", TEXT, LABEL_SIZE) <= 160.0);
    }

    #[test]
    fn theme_radii_and_shader_mask_use_logical_units() {
        assert_eq!(
            panel(&Theme::Dark).border.radius,
            iced::border::Radius::from(PANEL_RADIUS)
        );
        assert_eq!(
            value(&Theme::Dark).border.radius,
            iced::border::Radius::from(FIELD_RADIUS)
        );
        assert!(include_str!("spectrogram.wgsl").contains("const CORNER_RADIUS_PIXELS: f32 = 6.0;"));
        assert!(include_str!("spectrogram.wgsl").contains("CORNER_RADIUS_PIXELS * parameters.display_scale"));
        const {
            assert!(PANEL_RADIUS > CONTROL_RADIUS);
        }
        assert_eq!(numeric_input(text_input::Status::Active, true).border.color, ORANGE);
        assert_eq!(
            numeric_input(text_input::Status::Focused { is_hovered: false }, false)
                .border
                .color,
            BLUE
        );
    }
}
