//! Centralized `ImageControl` palette and the shared Roboto faces used by labels and numeric values.

use iced::widget::{button, checkbox, container, pick_list, slider};
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
pub const BOLD: Font = Font {
    weight: iced::font::Weight::Bold,
    ..TEXT
};
pub const ROBOTO: &[u8] = include_bytes!("../../../../../assets/Roboto-Regular.ttf");
pub const ROBOTO_MONO: &[u8] = include_bytes!("../../../../../assets/RobotoMono-Regular.ttf");

/// Settings use a bounded surface over the editor, rather than an extra layout row.
pub fn callout(theme: &Theme) -> container::Style {
    panel(theme).border(Border {
        color: GREY_MEDIUM,
        width: 1.0,
        radius: 6.0.into(),
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
            radius: 3.0.into(),
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
            radius: 3.0.into(),
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
            radius: 6.0.into(),
            ..Border::default()
        })
}

/// Styles a boxed numeric readout beside its slider.
pub fn value(_: &Theme) -> container::Style {
    container::Style::default()
        .background(GREY_MEDIUM_DARK)
        .color(GREY_LIGHT)
        .border(Border {
            radius: 0.0.into(),
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
            radius: 4.0.into(),
        },
        ..button::Style::default()
    }
}

/// Uses the C++ six-pixel track, translucent blue fill, and twelve-pixel white thumb.
pub fn parameter_slider(_: &Theme, _: slider::Status) -> slider::Style {
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
            border_width: 0.0,
            border_color: GREY_DARK,
        },
    }
}
