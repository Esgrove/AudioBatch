//! Sorting of analysis records and natural string ordering.
//!
//! Failed records always sort after successful ones, regardless of the requested key,
//! and the other columns break ties, so the order is the same on every run.

use std::cmp::Ordering;
use std::str::FromStr;

use crate::record::AnalysisRecord;

/// Available sort keys for the CLI output and in-memory result lists.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SortMode {
    /// Sample peak magnitude.
    #[default]
    Peak,
    /// File name.
    Name,
    /// Full path.
    Path,
    /// Integrated loudness.
    Loudness,
}

impl FromStr for SortMode {
    type Err = String;

    /// Parses a sort mode name, accepting "loudness" as an alias for "lufs".
    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value.trim().to_ascii_lowercase().as_str() {
            "peak" => Ok(Self::Peak),
            "lufs" | "loudness" => Ok(Self::Loudness),
            "name" => Ok(Self::Name),
            "path" => Ok(Self::Path),
            _ => Err(String::from("Sort mode must be one of: peak, lufs, name, path")),
        }
    }
}

/// Sorts records by the requested key and direction, with failed records last.
pub fn sort_records(records: &mut [AnalysisRecord], mode: SortMode, ascending: bool) {
    records.sort_by(|first, second| {
        first.has_error().cmp(&second.has_error()).then_with(|| {
            let ordering = compare_by_mode(first, second, mode);
            if ascending { ordering } else { ordering.reverse() }
        })
    });
}

/// Orders two records ascending by the sort key, with the other columns breaking ties.
/// Floating point values are ordered exactly with `total_cmp`, which is transitive as sorting requires.
#[must_use]
pub fn compare_by_mode(first: &AnalysisRecord, second: &AnalysisRecord, mode: SortMode) -> Ordering {
    let by_name = || compare_natural(&first.file_name, &second.file_name);
    let by_path = || compare_natural(&first.path_string(), &second.path_string());
    let by_loudness = || first.integrated_lufs.total_cmp(&second.integrated_lufs);
    let by_peak = || first.overall_peak.abs().total_cmp(&second.overall_peak.abs());
    match mode {
        SortMode::Name => by_name().then_with(by_path),
        SortMode::Path => by_path().then_with(by_name),
        SortMode::Loudness => by_loudness().then_with(by_peak).then_with(by_name),
        SortMode::Peak => by_peak().then_with(by_loudness).then_with(by_name),
    }
}

/// Compares two strings in natural order, ignoring case, so "track 2" sorts before "track 10".
#[must_use]
pub fn compare_natural(first: &str, second: &str) -> Ordering {
    let mut first_chars = first.chars().peekable();
    let mut second_chars = second.chars().peekable();
    loop {
        match (first_chars.peek().copied(), second_chars.peek().copied()) {
            (None, None) => return first.cmp(second),
            (None, Some(_)) => return Ordering::Less,
            (Some(_), None) => return Ordering::Greater,
            (Some(first_char), Some(second_char)) => {
                if first_char.is_ascii_digit() && second_char.is_ascii_digit() {
                    let first_number = take_digits(&mut first_chars);
                    let second_number = take_digits(&mut second_chars);
                    let ordering = compare_digit_runs(&first_number, &second_number);
                    if ordering != Ordering::Equal {
                        return ordering;
                    }
                } else {
                    let ordering = first_char.to_lowercase().cmp(second_char.to_lowercase());
                    if ordering != Ordering::Equal {
                        return ordering;
                    }
                    first_chars.next();
                    second_chars.next();
                }
            }
        }
    }
}

/// Consumes a run of ASCII digits from the iterator.
fn take_digits(chars: &mut std::iter::Peekable<std::str::Chars<'_>>) -> String {
    let mut digits = String::new();
    while let Some(&character) = chars.peek() {
        if !character.is_ascii_digit() {
            break;
        }
        digits.push(character);
        chars.next();
    }
    digits
}

/// Compares two runs of digits by numeric value without parsing, so long runs cannot overflow.
fn compare_digit_runs(first: &str, second: &str) -> Ordering {
    let first_trimmed = first.trim_start_matches('0');
    let second_trimmed = second.trim_start_matches('0');
    first_trimmed
        .len()
        .cmp(&second_trimmed.len())
        .then_with(|| first_trimmed.cmp(second_trimmed))
        .then_with(|| first.len().cmp(&second.len()))
}

#[cfg(test)]
mod test_natural_order {
    use super::*;

    #[test]
    fn numbers_compare_by_value() {
        assert_eq!(compare_natural("track 2", "track 10"), Ordering::Less);
        assert_eq!(compare_natural("track 10", "track 2"), Ordering::Greater);
    }

    #[test]
    fn case_is_ignored() {
        assert_eq!(compare_natural("Alpha", "beta"), Ordering::Less);
        assert_eq!(compare_natural("alpha", "Beta"), Ordering::Less);
    }

    #[test]
    fn prefixes_sort_first() {
        assert_eq!(compare_natural("track", "track 1"), Ordering::Less);
        assert_eq!(compare_natural("", "a"), Ordering::Less);
    }

    #[test]
    fn long_digit_runs_do_not_overflow() {
        assert_eq!(
            compare_natural("a99999999999999999999999", "a100000000000000000000000"),
            Ordering::Less
        );
    }
}

#[cfg(test)]
mod test_sort_records {
    use super::*;

    use crate::record::AnalysisStatus;

    /// Returns a record with the name, sample peak, integrated loudness, and status.
    fn record(name: &str, peak: f32, loudness: f64, status: AnalysisStatus) -> AnalysisRecord {
        AnalysisRecord {
            path: format!("/music/{name}").into(),
            file_name: name.to_string(),
            overall_peak: peak,
            integrated_lufs: loudness,
            status,
            ..Default::default()
        }
    }

    /// Returns the file names of the records, in order.
    fn names(records: &[AnalysisRecord]) -> Vec<&str> {
        records.iter().map(|record| record.file_name.as_str()).collect()
    }

    #[test]
    fn failed_records_sort_last_in_both_directions() {
        let mut records = vec![
            record("failed.wav", 0.0, -1000.0, AnalysisStatus::Failed),
            record("loud.wav", -0.9, -8.0, AnalysisStatus::Analyzed),
            record("quiet.wav", 0.1, -20.0, AnalysisStatus::Cached),
        ];
        sort_records(&mut records, SortMode::Peak, true);
        assert_eq!(names(&records), ["quiet.wav", "loud.wav", "failed.wav"]);
        sort_records(&mut records, SortMode::Peak, false);
        assert_eq!(names(&records), ["loud.wav", "quiet.wav", "failed.wav"]);
    }

    #[test]
    fn loudness_sort_uses_integrated_loudness() {
        let mut records = vec![
            record("b.wav", 0.5, -10.0, AnalysisStatus::Analyzed),
            record("a.wav", 0.5, -20.0, AnalysisStatus::Analyzed),
        ];
        sort_records(&mut records, SortMode::Loudness, true);
        assert_eq!(names(&records), ["a.wav", "b.wav"]);
    }

    #[test]
    fn name_sort_is_natural() {
        let mut records = vec![
            record("track 10.wav", 0.5, -10.0, AnalysisStatus::Analyzed),
            record("track 2.wav", 0.5, -10.0, AnalysisStatus::Analyzed),
        ];
        sort_records(&mut records, SortMode::Name, true);
        assert_eq!(names(&records), ["track 2.wav", "track 10.wav"]);
    }

    #[test]
    fn sort_mode_parses_aliases() {
        assert_eq!("LUFS".parse::<SortMode>(), Ok(SortMode::Loudness));
        assert_eq!("loudness".parse::<SortMode>(), Ok(SortMode::Loudness));
        assert_eq!("path".parse::<SortMode>(), Ok(SortMode::Path));
        assert!("size".parse::<SortMode>().is_err());
    }
}
