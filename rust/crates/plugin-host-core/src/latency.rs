//! Interprets the latency a plugin reports, shared by the VST3 and CLAP hosts.
//!
//! JUCE, which hosts the same VST3 plugins in the C++ application,
//! reads a reported latency as a signed 32-bit integer and clamps negative values to zero.
//! So a report of 2^31 samples or more, such as `u32::MAX` from a faulty plugin, means no latency there.
//! Both Rust hosts apply the same rule, so chain compensation, tail rendering,
//! and preview pre-roll stay equal to the C++ application instead of rendering billions of frames.

/// Largest reported latency taken as is, the largest positive value of a signed 32-bit integer.
pub const MAXIMUM_REPORTED_LATENCY: u32 = i32::MAX.cast_unsigned();

/// Returns the latency a host compensates for a plugin's raw report,
/// zero for reports of 2^31 samples or more, the way JUCE reads them.
#[must_use]
pub const fn compensated_latency(reported: u32) -> u32 {
    if reported > MAXIMUM_REPORTED_LATENCY {
        0
    } else {
        reported
    }
}

#[cfg(test)]
mod test_latency {
    use super::*;

    #[test]
    fn reports_below_two_to_the_31_are_kept() {
        assert_eq!(compensated_latency(0), 0);
        assert_eq!(compensated_latency(1024), 1024);
        assert_eq!(compensated_latency(MAXIMUM_REPORTED_LATENCY), MAXIMUM_REPORTED_LATENCY);
        assert_eq!(MAXIMUM_REPORTED_LATENCY, (1 << 31) - 1);
    }

    #[test]
    fn reports_that_are_negative_as_signed_integers_mean_no_latency() {
        assert_eq!(compensated_latency(1 << 31), 0);
        assert_eq!(compensated_latency(u32::MAX), 0);
    }
}
