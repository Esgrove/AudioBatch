//! Progress of a run that rewrites files: normalization or plugin processing.
//!
//! A run counts its finished files and collects the failures,
//! so the status bar can show the progress and the end of the run can list what went wrong.
//! The two kinds differ only in their wording.

use std::path::Path;
use std::time::Instant;

use audiobatch_core::operation::OperationResult;

/// Most failures listed by name at the end of a run, so the summary dialog stays a readable size.
const MAXIMUM_LISTED_FAILURES: usize = 8;

/// What a run does to its files.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OperationKind {
    /// Normalizes files to a peak or loudness target.
    Normalize,
    /// Renders files through the plugin chain.
    Process,
}

/// Progress of one run.
#[derive(Debug, Clone)]
pub struct OperationRun {
    /// What the run does.
    pub kind: OperationKind,
    /// Coordinator run the events belong to.
    pub run_id: u64,
    /// Number of files in the run.
    pub total: usize,
    /// Number of files with a result.
    pub finished: usize,
    /// One line per failed file, its name and the reason.
    pub failures: Vec<String>,
    /// True once every file has a result, or the run was cancelled.
    pub completed: bool,
    /// True once the run was cancelled.
    /// Outputs committed after that are still applied, since their sources were already replaced.
    pub cancelled: bool,
    started: Instant,
}

impl OperationKind {
    /// Returns the activity shown while files are worked on, for example "Normalizing".
    #[must_use]
    pub const fn activity(self) -> &'static str {
        match self {
            Self::Normalize => "Normalizing",
            Self::Process => "Processing",
        }
    }

    /// Returns the title of the dialogs of a run.
    #[must_use]
    pub const fn dialog_title(self) -> &'static str {
        match self {
            Self::Normalize => "Normalize",
            Self::Process => "Plugin Processing",
        }
    }

    /// Returns the past tense used in the summary, for example "Normalized".
    const fn past_tense(self) -> &'static str {
        match self {
            Self::Normalize => "Normalized",
            Self::Process => "Processed",
        }
    }

    /// Returns the verb of the failure list, for example "normalized".
    const fn failure_verb(self) -> &'static str {
        match self {
            Self::Normalize => "normalized",
            Self::Process => "processed",
        }
    }

    /// Returns the name of the run in the log line.
    const fn log_name(self) -> &'static str {
        match self {
            Self::Normalize => "Normalization",
            Self::Process => "Plugin processing",
        }
    }
}

impl OperationRun {
    /// Starts tracking a run of the given number of files.
    #[must_use]
    pub fn new(kind: OperationKind, run_id: u64, total: usize) -> Self {
        Self {
            kind,
            run_id,
            total,
            finished: 0,
            failures: Vec::new(),
            completed: false,
            cancelled: false,
            started: Instant::now(),
        }
    }

    /// Counts the result of one file and remembers its failure.
    pub fn record(&mut self, result: &OperationResult) {
        self.finished += 1;
        if let Some(error) = result.error() {
            self.failures.push(format!("{}: {error}", file_name(&result.source)));
        }
    }

    /// Returns the status bar text while the run is in progress.
    #[must_use]
    pub fn progress_text(&self) -> String {
        format!("{} {}/{}", self.kind.activity(), self.finished, self.total)
    }

    /// Returns the status bar text once the run is complete.
    #[must_use]
    pub fn summary(&self) -> String {
        let files = if self.total == 1 { "file" } else { "files" };
        let done = self.kind.past_tense();
        match self.failures.len() {
            0 => format!("{done} {} {files}", self.total),
            failed => format!("{done} {} {files}, {failed} failed", self.total),
        }
    }

    /// Returns the log line of a completed run, with its duration and counts.
    #[must_use]
    pub fn log_line(&self) -> String {
        format!(
            "{} complete: {:.2} s, {} files ({} succeeded, {} failed)",
            self.kind.log_name(),
            self.started.elapsed().as_secs_f64(),
            self.total,
            self.total.saturating_sub(self.failures.len()),
            self.failures.len()
        )
    }

    /// Returns the text of the failure dialog, or none when every file succeeded.
    /// A single failure is shown on its own, and a longer list names the first few files.
    #[must_use]
    pub fn failure_message(&self) -> Option<String> {
        match self.failures.as_slice() {
            [] => None,
            [single] => Some(single.clone()),
            failures => {
                let mut lines: Vec<String> = failures
                    .iter()
                    .take(MAXIMUM_LISTED_FAILURES)
                    .map(|failure| format!("- {failure}"))
                    .collect();
                if failures.len() > MAXIMUM_LISTED_FAILURES {
                    lines.push(format!("- ...and {} more", failures.len() - MAXIMUM_LISTED_FAILURES));
                }
                Some(format!(
                    "Some files could not be {}:\n\n{}",
                    self.kind.failure_verb(),
                    lines.join("\n")
                ))
            }
        }
    }
}

/// Returns the file name of a path for messages.
fn file_name(path: &Path) -> String {
    path.file_name().map_or_else(
        || path.display().to_string(),
        |name| name.to_string_lossy().into_owned(),
    )
}

#[cfg(test)]
mod test_run {
    use std::path::PathBuf;

    use super::*;

    /// Returns a failed result for a file name.
    fn failure(name: &str) -> OperationResult {
        OperationResult::failure(&PathBuf::from("/music").join(name), "Could not read")
    }

    #[test]
    fn progress_and_summary_count_the_results() {
        let mut run = OperationRun::new(OperationKind::Normalize, 1, 3);
        assert_eq!(run.progress_text(), "Normalizing 0/3");
        run.record(&failure("a.wav"));
        assert_eq!(run.progress_text(), "Normalizing 1/3");
        assert_eq!(run.summary(), "Normalized 3 files, 1 failed");
        assert!(run.log_line().contains("3 files (2 succeeded, 1 failed)"));
    }

    #[test]
    fn summary_of_a_clean_run_has_no_failures() {
        let run = OperationRun::new(OperationKind::Normalize, 1, 1);
        assert_eq!(run.summary(), "Normalized 1 file");
        assert_eq!(run.failure_message(), None);
    }

    #[test]
    fn single_failure_is_shown_on_its_own() {
        let mut run = OperationRun::new(OperationKind::Normalize, 1, 2);
        run.record(&failure("a.wav"));
        assert_eq!(run.failure_message().as_deref(), Some("a.wav: Could not read"));
    }

    #[test]
    fn long_failure_list_is_shortened() {
        let mut run = OperationRun::new(OperationKind::Normalize, 1, 10);
        for index in 0..10 {
            run.record(&failure(&format!("{index}.wav")));
        }
        let message = run.failure_message().expect("has failures");
        assert!(message.starts_with("Some files could not be normalized:\n\n- 0.wav: Could not read"));
        assert!(message.ends_with("- ...and 2 more"));
        assert_eq!(message.lines().filter(|line| line.starts_with("- ")).count(), 9);
    }

    #[test]
    fn processing_runs_use_their_own_wording() {
        let mut run = OperationRun::new(OperationKind::Process, 2, 3);
        assert_eq!(run.progress_text(), "Processing 0/3");
        run.record(&failure("a.wav"));
        run.record(&failure("b.wav"));
        assert_eq!(run.summary(), "Processed 3 files, 2 failed");
        assert!(run.log_line().starts_with("Plugin processing complete: "));
        let message = run.failure_message().expect("has failures");
        assert!(message.starts_with("Some files could not be processed:"));
        assert_eq!(run.kind.dialog_title(), "Plugin Processing");
    }
}
