//! Component-owned logical editor geometry, independent of DSP, native windows, and the editor feature.

use std::sync::Mutex;

use serde::{Deserialize, Serialize};

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

/// A coherent pair shared only by UI and non-audio state callbacks for one component lifetime.
#[derive(Default)]
pub struct EditorGeometry(Mutex<LogicalSize>);

impl EditorGeometry {
    /// Captures both dimensions under one short lock, without touching GUI ownership or dispatch.
    pub fn snapshot(&self) -> LogicalSize {
        *self.0.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    /// Publishes a complete validated pair without resizing or waiting for any native window.
    pub fn publish(&self, size: LogicalSize) {
        debug_assert!(size.is_valid());
        *self.0.lock().unwrap_or_else(std::sync::PoisonError::into_inner) = size;
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
}
