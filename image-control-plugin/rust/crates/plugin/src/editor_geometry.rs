//! Component-owned logical geometry and layout preferences, independent of DSP, native windows, and the editor feature.

use std::sync::Arc;

use arc_swap::ArcSwap;
use serde::{Deserialize, Serialize};

use crate::editor_preferences::LayoutPreferences;

/// Fixed logical dimensions for a new instance and legacy host states.
pub const DEFAULT_EDITOR_SIZE: (u32, u32) = (1000, 764);
/// Minimum logical dimensions that keep the four bands usable.
pub const MIN_EDITOR_SIZE: (u32, u32) = (760, 600);
/// Maximum logical dimensions before platform-native DPI conversion.
pub const MAX_EDITOR_SIZE: (u32, u32) = (4096, 4096);

/// Detached logical geometry retains fractional Windows dimensions without repeated rounding.
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct LogicalSize {
    /// Width in macOS points or DPI-independent Windows units.
    pub width: f64,
    /// Height in macOS points or DPI-independent Windows units.
    pub height: f64,
}

impl LogicalSize {
    /// Checks the entire detached pair before state publication or native conversion.
    pub fn is_valid(self) -> bool {
        self.width.is_finite()
            && self.height.is_finite()
            && (f64::from(MIN_EDITOR_SIZE.0)..=f64::from(MAX_EDITOR_SIZE.0)).contains(&self.width)
            && (f64::from(MIN_EDITOR_SIZE.1)..=f64::from(MAX_EDITOR_SIZE.1)).contains(&self.height)
    }

    /// Converts once into native host units. macOS callers supply one regardless of backing scale.
    #[cfg(any(feature = "editor", test))]
    pub fn native(self, scale: f64) -> (i32, i32) {
        (
            (self.width * scale).round() as i32,
            (self.height * scale).round() as i32,
        )
    }

    /// Converts an already validated native acknowledgement, preserving exact logical geometry on repeated echoes.
    #[cfg(any(feature = "editor", test))]
    pub fn acknowledged(self, width: i32, height: i32, scale: f64) -> Self {
        if self.native(scale) == (width, height) {
            return self;
        }
        Self {
            width: (f64::from(width) / scale).clamp(f64::from(MIN_EDITOR_SIZE.0), f64::from(MAX_EDITOR_SIZE.0)),
            height: (f64::from(height) / scale).clamp(f64::from(MIN_EDITOR_SIZE.1), f64::from(MAX_EDITOR_SIZE.1)),
        }
    }
}

impl Default for LogicalSize {
    /// Uses fixed defaults rather than sampling another component or a live window.
    fn default() -> Self {
        Self {
            width: f64::from(DEFAULT_EDITOR_SIZE.0),
            height: f64::from(DEFAULT_EDITOR_SIZE.1),
        }
    }
}

/// Component-lifetime editor choices shared only by UI and non-audio state callbacks.
/// Size and layout snapshots are individually coherent and never require GUI ownership or dispatch.
#[derive(Default)]
pub struct EditorGeometry {
    /// Last host-accepted logical dimensions, independent of section layout choices.
    size: ArcSwap<LogicalSize>,
    /// Last validated section layout and interaction choices, independent of native view lifetime.
    preferences: ArcSwap<LayoutPreferences>,
}

impl EditorGeometry {
    /// Copies both dimensions from one immutable non-audio snapshot, without touching GUI ownership or dispatch.
    pub fn snapshot(&self) -> LogicalSize {
        **self.size.load()
    }

    /// Publishes a complete validated pair without resizing or waiting for any native window.
    /// Invalid input leaves the last accepted dimensions untouched, including in release builds.
    pub fn publish(&self, size: LogicalSize) {
        if size.is_valid() {
            self.size.store(Arc::new(size));
        }
    }

    /// Copies one immutable non-audio layout snapshot, independently of the logical size.
    #[must_use]
    pub fn preferences(&self) -> LayoutPreferences {
        **self.preferences.load()
    }

    /// Publishes validated layout choices without resizing or dispatching to a native editor.
    /// Returns true for accepted values, including unchanged ones. Invalid input leaves every stored value untouched.
    pub fn publish_preferences(&self, preferences: LayoutPreferences) -> bool {
        if !preferences.is_valid() {
            return false;
        }
        self.preferences.store(Arc::new(preferences));
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_acknowledgement_and_repeated_dpi_echoes_do_not_drift() {
        let initial = LogicalSize::default().acknowledged(1501, 1201, 1.5);
        assert!(initial.is_valid());
        assert_eq!(initial.native(1.5), (1501, 1201));
        let mut remembered = initial;
        for _ in 0..32 {
            for scale in [1.0, 1.25, 1.5, 2.0, 8.0] {
                let (width, height) = remembered.native(scale);
                remembered = remembered.acknowledged(width, height, scale);
                assert_eq!(remembered, initial);
            }
        }
    }

    #[test]
    fn rounded_native_minimum_retains_valid_logical_geometry() {
        let size = LogicalSize::default().acknowledged(950, 749, 1.249);
        assert!(size.is_valid());
        assert_eq!(size.native(1.249), (950, 749));
    }

    #[test]
    fn concurrent_snapshots_never_tear_and_instances_remain_independent() {
        let geometry = std::sync::Arc::new(EditorGeometry::default());
        let other = EditorGeometry::default();
        let resized = LogicalSize {
            width: 1200.0,
            height: 900.0,
        };
        std::thread::scope(|scope| {
            scope.spawn(|| {
                for _ in 0..128 {
                    geometry.publish(resized);
                    geometry.publish(LogicalSize::default());
                }
            });
            for _ in 0..128 {
                let size = geometry.snapshot();
                assert!(size == resized || size == LogicalSize::default());
            }
        });
        assert_eq!(other.snapshot(), LogicalSize::default());
    }

    #[test]
    fn invalid_size_publication_preserves_exact_fractional_dimensions() {
        let geometry = EditorGeometry::default();
        let retained = LogicalSize {
            width: 1500.125,
            height: 1100.0625,
        };
        geometry.publish(retained);
        for invalid in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, 0.0, 4096.0_f64.next_up()] {
            geometry.publish(LogicalSize {
                width: invalid,
                ..retained
            });
            assert_eq!(geometry.snapshot(), retained);
            geometry.publish(LogicalSize {
                height: invalid,
                ..retained
            });
            assert_eq!(geometry.snapshot(), retained);
        }
    }

    #[test]
    fn retained_immutable_size_and_layout_records_survive_multiwriter_replacement() {
        let geometry = EditorGeometry::default();
        let retained_size = geometry.size.load_full();
        let retained_preferences = geometry.preferences.load_full();
        let sizes = [
            LogicalSize::default(),
            LogicalSize {
                width: 1500.125,
                height: 1100.0625,
            },
        ];
        let preferences = [
            LayoutPreferences::default(),
            LayoutPreferences {
                linked: [false, true, false, true],
                toolbar_shown: false,
                correlation_shown: false,
                compact_bands: true,
                band_height: 900.125,
                spectrogram_hidden: true,
                analysis_stopped: true,
            },
        ];
        let start = std::sync::Barrier::new(3);
        std::thread::scope(|scope| {
            for (size, preferences) in sizes.into_iter().zip(preferences) {
                let geometry = &geometry;
                let start = &start;
                scope.spawn(move || {
                    start.wait();
                    for _ in 0..1024 {
                        geometry.publish(size);
                        assert!(geometry.publish_preferences(preferences));
                    }
                });
            }
            start.wait();
            for _ in 0..2048 {
                assert!(sizes.contains(&geometry.snapshot()));
                assert!(preferences.contains(&geometry.preferences()));
                assert_eq!(*retained_size, LogicalSize::default());
                assert_eq!(*retained_preferences, LayoutPreferences::default());
            }
        });
    }

    #[test]
    fn new_instances_have_fixed_layout_defaults() {
        let geometry = EditorGeometry::default();
        assert_eq!(geometry.preferences(), LayoutPreferences::default());
        assert_eq!(geometry.snapshot(), LogicalSize::default());
    }

    #[test]
    fn layout_and_size_publication_do_not_overwrite_each_other_or_other_instances() {
        let geometry = EditorGeometry::default();
        let other = EditorGeometry::default();
        let preferences = LayoutPreferences {
            linked: [false, true, false, true],
            toolbar_shown: false,
            correlation_shown: false,
            compact_bands: true,
            band_height: 512.25,
            spectrogram_hidden: true,
            analysis_stopped: true,
        };
        assert!(geometry.publish_preferences(preferences));
        assert_eq!(geometry.preferences(), preferences);
        assert_eq!(geometry.snapshot(), LogicalSize::default());

        let resized = LogicalSize {
            width: 1400.5,
            height: 1024.25,
        };
        geometry.publish(resized);
        assert_eq!(geometry.snapshot(), resized);
        assert_eq!(geometry.preferences(), preferences);

        assert!(geometry.publish_preferences(LayoutPreferences::default()));
        assert_eq!(geometry.snapshot(), resized);
        assert_eq!(geometry.preferences(), LayoutPreferences::default());
        assert_eq!(other.snapshot(), LogicalSize::default());
        assert_eq!(other.preferences(), LayoutPreferences::default());
    }

    #[test]
    fn detached_layout_edits_require_explicit_publication_and_unchanged_values_are_accepted() {
        let geometry = EditorGeometry::default();
        let mut detached = geometry.preferences();
        detached.linked = [false; 4];
        detached.band_height = 600.5;
        assert_eq!(geometry.preferences(), LayoutPreferences::default());
        assert!(geometry.publish_preferences(detached));
        assert!(geometry.publish_preferences(detached));
        assert_eq!(geometry.preferences(), detached);
    }

    #[test]
    fn invalid_layout_publication_keeps_every_preference_and_dimension_unchanged() {
        let geometry = EditorGeometry::default();
        let retained = LayoutPreferences {
            linked: [false, true, true, false],
            toolbar_shown: false,
            correlation_shown: false,
            compact_bands: true,
            band_height: 700.25,
            spectrogram_hidden: true,
            analysis_stopped: true,
        };
        let resized = LogicalSize {
            width: 1500.5,
            height: 1100.25,
        };
        assert!(geometry.publish_preferences(retained));
        geometry.publish(resized);

        for band_height in [
            f32::NAN,
            f32::INFINITY,
            f32::NEG_INFINITY,
            f32::MIN,
            f32::MAX,
            0.0,
            crate::editor_preferences::MIN_BAND_HEIGHT.next_down(),
            crate::editor_preferences::MAX_BAND_HEIGHT.next_up(),
        ] {
            let invalid = LayoutPreferences {
                band_height,
                ..LayoutPreferences::default()
            };
            assert!(!geometry.publish_preferences(invalid));
            assert_eq!(geometry.preferences(), retained);
            assert_eq!(geometry.snapshot(), resized);
        }
    }

    #[test]
    fn concurrent_size_and_layout_snapshots_remain_individually_coherent() {
        let geometry = EditorGeometry::default();
        let other = EditorGeometry::default();
        let first = LayoutPreferences::default();
        let second = LayoutPreferences {
            linked: [false; 4],
            toolbar_shown: false,
            correlation_shown: false,
            compact_bands: true,
            band_height: 4096.0,
            spectrogram_hidden: true,
            analysis_stopped: true,
        };
        let resized = LogicalSize {
            width: 1300.5,
            height: 900.25,
        };
        let start = std::sync::Barrier::new(4);
        let read_snapshots = || {
            start.wait();
            for _ in 0..1024 {
                let preferences = geometry.preferences();
                assert!(preferences == first || preferences == second);
                let size = geometry.snapshot();
                assert!(size == LogicalSize::default() || size == resized);
                assert_eq!(other.preferences(), LayoutPreferences::default());
                assert_eq!(other.snapshot(), LogicalSize::default());
            }
        };
        std::thread::scope(|scope| {
            scope.spawn(|| {
                start.wait();
                for _ in 0..1024 {
                    geometry.publish(resized);
                    geometry.publish(LogicalSize::default());
                }
            });
            scope.spawn(|| {
                start.wait();
                for _ in 0..1024 {
                    assert!(geometry.publish_preferences(second));
                    assert!(geometry.publish_preferences(first));
                }
            });
            scope.spawn(read_snapshots);
            read_snapshots();
        });
        assert_eq!(geometry.preferences(), first);
        assert_eq!(geometry.snapshot(), LogicalSize::default());
    }
}
