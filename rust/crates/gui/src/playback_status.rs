//! Source-time formatting for the independently rendered playback clock in the status bar.

/// Upper bound that keeps malformed timing values within an ordinary integer display range.
const MAXIMUM_DISPLAY_SECONDS: f64 = 999_999_999.0;
/// Seconds in one displayed minute.
const SECONDS_PER_MINUTE: u64 = 60;
/// Seconds at which both displayed clocks gain an hours field.
const SECONDS_PER_HOUR: u64 = 3_600;

/// Formats elapsed and total source time, using hours for both when the total reaches one hour.
#[must_use]
pub fn format_playback_time(position_seconds: f64, duration_seconds: f64) -> String {
    let duration = if duration_seconds.is_finite() {
        duration_seconds.clamp(0.0, MAXIMUM_DISPLAY_SECONDS)
    } else {
        0.0
    };
    let position = if position_seconds.is_finite() {
        position_seconds.clamp(0.0, duration)
    } else {
        0.0
    };
    let current = position.floor() as u64;
    let total = duration.floor() as u64;
    if total >= SECONDS_PER_HOUR {
        format!(
            "{}:{:02}:{:02} / {}:{:02}:{:02}",
            current / SECONDS_PER_HOUR,
            (current / SECONDS_PER_MINUTE) % SECONDS_PER_MINUTE,
            current % SECONDS_PER_MINUTE,
            total / SECONDS_PER_HOUR,
            (total / SECONDS_PER_MINUTE) % SECONDS_PER_MINUTE,
            total % SECONDS_PER_MINUTE,
        )
    } else {
        format!(
            "{:02}:{:02} / {:02}:{:02}",
            current / SECONDS_PER_MINUTE,
            current % SECONDS_PER_MINUTE,
            total / SECONDS_PER_MINUTE,
            total % SECONDS_PER_MINUTE,
        )
    }
}

#[cfg(test)]
mod test_playback_status {
    use super::format_playback_time;

    #[test]
    fn short_sources_use_whole_minutes_and_seconds() {
        assert_eq!(format_playback_time(61.9, 245.9), "01:01 / 04:05");
        assert_eq!(format_playback_time(0.0, 3599.999), "00:00 / 59:59");
    }

    #[test]
    fn long_sources_use_hours_on_both_clocks() {
        assert_eq!(format_playback_time(0.0, 3600.0), "0:00:00 / 1:00:00");
        assert_eq!(format_playback_time(312.0, 4860.0), "0:05:12 / 1:21:00");
        assert_eq!(format_playback_time(4860.0, 4860.0), "1:21:00 / 1:21:00");
    }

    #[test]
    fn malformed_and_outside_positions_are_bounded() {
        assert_eq!(format_playback_time(-1.0, 65.0), "00:00 / 01:05");
        assert_eq!(format_playback_time(100.0, 2.0), "00:02 / 00:02");
        assert_eq!(format_playback_time(f64::NAN, 65.0), "00:00 / 01:05");
        assert_eq!(format_playback_time(1.0, f64::INFINITY), "00:00 / 00:00");
    }
}
