//! Centralized `ImageControl` palette and the shared Roboto faces used by labels and numeric values.

use iced::widget::{button, container, slider};
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
            radius: 3.0.into(),
            ..Border::default()
        })
}

/// Distinguishes toggle selection without adding badges or additional status decoration.
pub fn control(status: button::Status, active: bool, accent: Color) -> button::Style {
    let disabled = matches!(status, button::Status::Disabled);
    let background = if active {
        accent
    } else if matches!(status, button::Status::Hovered) {
        GREY_MEDIUM
    } else {
        GREY_MEDIUM_DARK
    };
    button::Style {
        background: Some(background.into()),
        text_color: if disabled {
            GREY_MIDDLE
        } else if active {
            GREY_DARK
        } else {
            GREY_LIGHT
        },
        border: Border {
            color: GREY_MEDIUM,
            width: 1.0,
            radius: 4.0.into(),
        },
        ..button::Style::default()
    }
}

/// Uses the same grey track, blue fill, and compact handle for every continuous parameter.
pub fn parameter_slider(_: &Theme, status: slider::Status) -> slider::Style {
    slider::Style {
        rail: slider::Rail {
            backgrounds: (BLUE.into(), GREY_MEDIUM.into()),
            width: 4.0,
            border: Border {
                radius: 2.0.into(),
                ..Border::default()
            },
        },
        handle: slider::Handle {
            shape: slider::HandleShape::Circle { radius: 5.0 },
            background: if matches!(status, slider::Status::Active) {
                BLUE.into()
            } else {
                GREY_SUPER_LIGHT.into()
            },
            border_width: 0.0,
            border_color: GREY_DARK,
        },
    }
}
