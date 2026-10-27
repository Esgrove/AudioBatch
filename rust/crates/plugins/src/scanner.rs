//! Scanning folders for VST3 plugins without loading plugin code in this process.
//!
//! Bundles that are unchanged since the last scan, and blacklisted ones, are skipped.
//! A bundle with a `moduleinfo.json` file is described from that file.
//! Every other bundle is loaded by a worker process, which is this executable started with `WORKER_FLAG`.
//! One worker serves the whole scan and is started again after it crashes or hangs.
//! A crash blacklists the bundle, so later scans skip it.
//! A bundle that takes longer than the timeout is skipped but not blacklisted,
//! because a copy-protected plugin may only have been waiting for a licensing prompt.
//! After a scan, plugins whose bundles are gone are removed, but only inside scanned folders that exist,
//! so plugins on a drive that is not mounted stay listed.
//!
//! The scan needs no crash log file, unlike scanners that load plugins in process:
//! this process never runs plugin code, so a crash cannot end the scan.

use std::ffi::OsString;
use std::fs::File;
use std::io::{BufRead as _, BufReader, Write as _};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, bail};
use crossbeam_channel::{Receiver, RecvTimeoutError};
use vst3_host::{find_bundles, read_module_info};

use crate::library::{KnownPlugins, PluginDescription, bundle_modified_ms};
use crate::scan_protocol::{ScanReply, ScanRequest, WORKER_FLAG};

/// Longest time the worker may take to scan one bundle.
/// Generous, because a copy-protected plugin can show a licensing prompt while it is scanned.
pub const SCAN_TIMEOUT: Duration = Duration::from_secs(180);

/// Message for a scan that cannot start because another one holds the lock.
pub const SCAN_BUSY_MESSAGE: &str =
    "Another plugin scan is running, for example in the AudioBatch plugin manager. Try again once it has finished.";

/// How often a scan checks for cancellation while it waits for the worker.
const POLL_INTERVAL: Duration = Duration::from_millis(50);

/// What waiting for a worker reply ended with.
enum WorkerOutcome {
    /// The worker replied.
    Reply(ScanReply),
    /// The worker exited before replying.
    Crashed,
    /// The worker did not reply in time and was stopped.
    TimedOut,
    /// The scan was cancelled and the worker was stopped.
    Cancelled,
}

/// An event from the thread that reads the worker's stdout.
enum WorkerEvent {
    /// A reply line.
    Reply(ScanReply),
    /// The worker's stdout closed.
    Exited,
}

/// The lock that only one scan at a time can hold, across processes.
/// It is released when dropped.
#[derive(Debug)]
pub struct ScanLock {
    /// The locked file.
    _file: File,
}

/// How to start the worker process.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkerCommand {
    /// The executable.
    pub program: PathBuf,
    /// Its arguments.
    pub arguments: Vec<OsString>,
    /// Extra environment variables.
    pub environment: Vec<(OsString, OsString)>,
}

/// The settings of a scan.
#[derive(Debug, Clone)]
pub struct ScanOptions {
    /// Folders to search for bundles.
    pub folders: Vec<PathBuf>,
    /// How to start the worker.
    pub worker: WorkerCommand,
    /// File that receives the worker's stderr, replaced at the start of each scan.
    pub worker_log: Option<PathBuf>,
    /// Longest time the worker may take for one bundle.
    pub timeout: Duration,
}

/// What a scan changed and which bundles it could not scan.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ScanReport {
    /// Plugins found for the first time.
    pub added: Vec<PluginDescription>,
    /// Plugins found again in a changed bundle.
    pub updated: Vec<PluginDescription>,
    /// Plugins removed because their bundle is gone or no longer contains them.
    pub removed: Vec<PluginDescription>,
    /// Bundles that crashed the worker and were blacklisted.
    pub crashed: Vec<PathBuf>,
    /// Bundles that took too long and were skipped.
    pub timed_out: Vec<PathBuf>,
    /// Bundles that contain no audio effects.
    pub no_plugins: Vec<PathBuf>,
    /// Bundles the worker could not load, with the reason it gave.
    pub failed: Vec<(PathBuf, String)>,
    /// Whether the scan was cancelled before it finished.
    pub cancelled: bool,
}

/// Progress of a scan.
#[derive(Debug, Clone, Copy)]
pub struct ScanProgress<'a> {
    /// Share of the bundles handled so far, between 0 and 1.
    pub fraction: f32,
    /// The bundle being scanned.
    pub bundle: &'a Path,
}

/// A running worker process.
struct WorkerProcess {
    /// The process.
    child: Child,
    /// Its stdin, for requests.
    stdin: ChildStdin,
    /// Replies read from its stdout.
    events: Receiver<WorkerEvent>,
}

impl ScanLock {
    /// Takes the lock without waiting, returning `None` when another scan holds it.
    ///
    /// # Errors
    ///
    /// Returns an error when the lock file cannot be created.
    pub fn try_acquire(path: &Path) -> Result<Option<Self>> {
        if let Some(directory) = path.parent() {
            std::fs::create_dir_all(directory)?;
        }
        let file = File::options()
            .create(true)
            .truncate(false)
            .write(true)
            .open(path)
            .with_context(|| format!("Could not open the plugin scan lock {}", path.display()))?;
        match file.try_lock() {
            Ok(()) => Ok(Some(Self { _file: file })),
            Err(std::fs::TryLockError::WouldBlock) => Ok(None),
            Err(std::fs::TryLockError::Error(error)) => {
                Err(error).with_context(|| format!("Could not lock {}", path.display()))
            }
        }
    }
}

impl WorkerCommand {
    /// Returns the command that starts this executable as a worker.
    ///
    /// # Errors
    ///
    /// Returns an error when the path of this executable is unknown.
    pub fn current_executable() -> Result<Self> {
        Ok(Self {
            program: std::env::current_exe().context("Could not find the AudioBatch executable")?,
            arguments: vec![OsString::from(WORKER_FLAG)],
            environment: Vec::new(),
        })
    }
}

impl ScanOptions {
    /// Returns the options for scanning folders with this executable as the worker and the default timeout.
    ///
    /// # Errors
    ///
    /// Returns an error when the path of this executable is unknown.
    pub fn new(folders: Vec<PathBuf>, worker_log: Option<PathBuf>) -> Result<Self> {
        Ok(Self {
            folders,
            worker: WorkerCommand::current_executable()?,
            worker_log,
            timeout: SCAN_TIMEOUT,
        })
    }
}

impl ScanReport {
    /// Returns a one-line summary of what changed and what could not be scanned,
    /// for example "Scan finished: 3 added, 1 blacklisted after a crash".
    #[must_use]
    pub fn summary(&self) -> String {
        let counts = [
            (self.added.len(), "added"),
            (self.updated.len(), "updated"),
            (self.removed.len(), "removed"),
            (self.crashed.len(), "blacklisted after a crash"),
            (self.timed_out.len(), "timed out"),
            (self.no_plugins.len(), "without audio effects"),
            (self.failed.len(), "could not be loaded"),
        ];
        let parts: Vec<String> = counts
            .iter()
            .filter(|(count, _)| *count > 0)
            .map(|(count, label)| format!("{count} {label}"))
            .collect();
        let outcome = if self.cancelled {
            "Scan cancelled"
        } else {
            "Scan finished"
        };
        if parts.is_empty() {
            format!("{outcome}: no changes")
        } else {
            format!("{outcome}: {}", parts.join(", "))
        }
    }
}

impl WorkerProcess {
    /// Starts a worker, sending its stderr, and the lines of its stdout that are not replies, to `log` when given.
    fn start(command: &WorkerCommand, log: Option<&File>) -> Result<Self> {
        let stderr = match log {
            Some(file) => Stdio::from(file.try_clone()?),
            None => Stdio::null(),
        };
        let mut stdout_log = log.map(File::try_clone).transpose()?;
        let mut child = Command::new(&command.program)
            .args(&command.arguments)
            .envs(command.environment.iter().map(|(name, value)| (name, value)))
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(stderr)
            .spawn()
            .with_context(|| format!("Could not start the plugin scan worker {}", command.program.display()))?;
        let (Some(stdin), Some(stdout)) = (child.stdin.take(), child.stdout.take()) else {
            let _ = child.kill();
            bail!("Could not connect to the plugin scan worker");
        };
        let (sender, events) = crossbeam_channel::unbounded();
        std::thread::spawn(move || {
            // Plugins may print anything to stdout while they load, including bytes that are not UTF-8,
            // so lines are read as bytes and only a read error of the pipe ends the loop.
            for line in BufReader::new(stdout).split(b'\n') {
                let Ok(line) = line else {
                    break;
                };
                let line = String::from_utf8_lossy(&line);
                match ScanReply::from_line(&line) {
                    Some(reply) => {
                        if sender.send(WorkerEvent::Reply(reply)).is_err() {
                            return;
                        }
                    }
                    None => {
                        if let Some(file) = stdout_log.as_mut() {
                            let _ = writeln!(file, "{}", line.trim_end_matches('\r'));
                        }
                    }
                }
            }
            let _ = sender.send(WorkerEvent::Exited);
        });
        Ok(Self { child, stdin, events })
    }

    /// Sends a request. Returns false when the worker no longer reads requests.
    fn send(&mut self, bundle: &Path) -> bool {
        let request = ScanRequest {
            bundle: bundle.to_path_buf(),
        };
        let Ok(line) = serde_json::to_string(&request) else {
            return false;
        };
        writeln!(self.stdin, "{line}").and_then(|()| self.stdin.flush()).is_ok()
    }

    /// Waits for the reply to the last request.
    fn wait(&self, timeout: Duration, is_cancelled: &dyn Fn() -> bool) -> WorkerOutcome {
        let deadline = Instant::now() + timeout;
        loop {
            if is_cancelled() {
                return WorkerOutcome::Cancelled;
            }
            match self.events.recv_timeout(POLL_INTERVAL) {
                Ok(WorkerEvent::Reply(reply)) => return WorkerOutcome::Reply(reply),
                Ok(WorkerEvent::Exited) | Err(RecvTimeoutError::Disconnected) => return WorkerOutcome::Crashed,
                Err(RecvTimeoutError::Timeout) if Instant::now() >= deadline => return WorkerOutcome::TimedOut,
                Err(RecvTimeoutError::Timeout) => {}
            }
        }
    }
}

impl Drop for WorkerProcess {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// Scans the folders for plugins and updates the known plugins.
///
/// The caller holds the scan lock from loading the known plugins until saving them,
/// so two scans cannot overwrite each other's results.
/// A cancelled scan keeps the results it has so far.
///
/// # Errors
///
/// Returns an error when the worker cannot be started.
pub fn scan_plugins(
    _lock: &ScanLock,
    known: &mut KnownPlugins,
    options: &ScanOptions,
    is_cancelled: &dyn Fn() -> bool,
    progress: &mut dyn FnMut(ScanProgress<'_>),
) -> Result<ScanReport> {
    let mut report = ScanReport::default();
    let log = match &options.worker_log {
        Some(path) => {
            if let Some(directory) = path.parent() {
                std::fs::create_dir_all(directory)?;
            }
            Some(File::create(path).with_context(|| format!("Could not create {}", path.display()))?)
        }
        None => None,
    };
    let bundles = find_bundles(&options.folders);
    let mut worker: Option<WorkerProcess> = None;
    for (index, bundle) in bundles.iter().enumerate() {
        if is_cancelled() {
            report.cancelled = true;
            break;
        }
        #[expect(
            clippy::cast_precision_loss,
            reason = "bundle counts are far below the f32 precision limit"
        )]
        let fraction = index as f32 / bundles.len() as f32;
        progress(ScanProgress { fraction, bundle });
        let modified = bundle_modified_ms(bundle);
        if known.is_blacklisted(bundle) || is_up_to_date(known, bundle, modified) {
            continue;
        }
        if let Some(classes) = read_module_info(bundle) {
            let plugins: Vec<PluginDescription> = classes
                .iter()
                .filter(|class| class.is_audio_effect())
                .map(|class| PluginDescription::from_class(class, bundle, modified))
                .collect();
            if plugins.is_empty() {
                report.no_plugins.push(bundle.clone());
            }
            apply_results(known, &mut report, bundle, plugins);
            continue;
        }
        let outcome = scan_in_worker(&mut worker, options, log.as_ref(), bundle, is_cancelled)?;
        match outcome {
            WorkerOutcome::Reply(reply) => {
                let plugins: Vec<PluginDescription> = reply
                    .plugins
                    .into_iter()
                    .map(|mut plugin| {
                        plugin.bundle.clone_from(bundle);
                        plugin.bundle_modified_ms = modified;
                        plugin
                    })
                    .collect();
                if plugins.is_empty() {
                    match reply.error {
                        Some(error) => report.failed.push((bundle.clone(), error)),
                        None => report.no_plugins.push(bundle.clone()),
                    }
                }
                apply_results(known, &mut report, bundle, plugins);
            }
            WorkerOutcome::Crashed => {
                worker = None;
                known.plugins.retain(|plugin| plugin.bundle != *bundle);
                known.blacklist.push(bundle.clone());
                report.crashed.push(bundle.clone());
            }
            WorkerOutcome::TimedOut => {
                worker = None;
                report.timed_out.push(bundle.clone());
            }
            WorkerOutcome::Cancelled => {
                report.cancelled = true;
                break;
            }
        }
    }
    drop(worker);
    if !report.cancelled {
        remove_missing(known, &mut report, &options.folders);
    }
    known.sort();
    Ok(report)
}

/// Returns true when the known plugins of a bundle were scanned from the bundle as it is now.
fn is_up_to_date(known: &KnownPlugins, bundle: &Path, modified: i64) -> bool {
    let mut plugins = known.plugins.iter().filter(|plugin| plugin.bundle == bundle).peekable();
    plugins.peek().is_some() && plugins.all(|plugin| plugin.bundle_modified_ms == modified)
}

/// Sends a bundle to the worker, starting one when none runs, and waits for the outcome.
fn scan_in_worker(
    worker: &mut Option<WorkerProcess>,
    options: &ScanOptions,
    log: Option<&File>,
    bundle: &Path,
    is_cancelled: &dyn Fn() -> bool,
) -> Result<WorkerOutcome> {
    let mut sent = false;
    for _ in 0..2 {
        let process = match worker {
            Some(process) => process,
            None => worker.insert(WorkerProcess::start(&options.worker, log)?),
        };
        if process.send(bundle) {
            sent = true;
            break;
        }
        *worker = None;
    }
    if !sent {
        bail!("The plugin scan worker stopped accepting requests");
    }
    let Some(process) = worker.as_ref() else {
        bail!("The plugin scan worker stopped accepting requests");
    };
    let outcome = process.wait(options.timeout, is_cancelled);
    if matches!(outcome, WorkerOutcome::Cancelled | WorkerOutcome::TimedOut) {
        *worker = None;
    }
    Ok(outcome)
}

/// Replaces the known plugins of a bundle with the ones a scan found.
fn apply_results(known: &mut KnownPlugins, report: &mut ScanReport, bundle: &Path, plugins: Vec<PluginDescription>) {
    let (previous, kept): (Vec<PluginDescription>, Vec<PluginDescription>) = std::mem::take(&mut known.plugins)
        .into_iter()
        .partition(|plugin| plugin.bundle == bundle);
    known.plugins = kept;
    for plugin in &previous {
        if !plugins.iter().any(|found| found.class_id == plugin.class_id) {
            report.removed.push(plugin.clone());
        }
    }
    for plugin in plugins {
        if previous.iter().any(|old| old.class_id == plugin.class_id) {
            report.updated.push(plugin.clone());
        } else {
            report.added.push(plugin.clone());
        }
        known.plugins.push(plugin);
    }
}

/// Removes the plugins and blacklist entries of bundles that are gone from scanned folders that exist.
fn remove_missing(known: &mut KnownPlugins, report: &mut ScanReport, folders: &[PathBuf]) {
    let existing_folders: Vec<&PathBuf> = folders.iter().filter(|folder| folder.is_dir()).collect();
    let is_missing =
        |bundle: &Path| !bundle.exists() && existing_folders.iter().any(|folder| bundle.starts_with(folder));
    let (removed, kept): (Vec<PluginDescription>, Vec<PluginDescription>) = std::mem::take(&mut known.plugins)
        .into_iter()
        .partition(|plugin| is_missing(&plugin.bundle));
    known.plugins = kept;
    report.removed.extend(removed);
    known.blacklist.retain(|bundle| !is_missing(bundle));
}

#[cfg(test)]
mod test_scanner {
    use audiobatch_test_plugin::{Kind, create_bundle};
    use tempfile::TempDir;

    use super::*;

    /// Environment variable that makes the test plugin crash while it loads.
    const CRASH: &str = "AUDIOBATCH_TEST_PLUGIN_CRASH";

    /// Environment variable that makes the test plugin wait while it loads.
    const SLEEP: &str = "AUDIOBATCH_TEST_PLUGIN_SLEEP_MS";

    /// A plugin folder with one test plugin bundle and the scan files.
    struct Setup {
        /// Holds the plugin folder and the data files.
        directory: TempDir,
        /// The scanned folder.
        folder: PathBuf,
        /// The test plugin bundle.
        bundle: PathBuf,
    }

    impl Setup {
        /// Creates the folder with one bundle.
        fn new() -> Self {
            let directory = tempfile::tempdir().expect("temporary folder");
            let folder = directory.path().join("plugins");
            std::fs::create_dir_all(&folder).expect("plugin folder");
            let bundle = create_bundle(&folder, "Effects").expect("test plugin bundle");
            Self {
                directory,
                folder,
                bundle,
            }
        }

        /// Scans the folder with extra worker environment variables and a timeout.
        fn scan_with(
            &self,
            known: &mut KnownPlugins,
            environment: &[(&str, &str)],
            timeout: Duration,
            cancel: bool,
        ) -> ScanReport {
            let worker = WorkerCommand {
                program: worker_path(),
                arguments: vec![OsString::from(WORKER_FLAG)],
                environment: environment
                    .iter()
                    .map(|(name, value)| (OsString::from(name), OsString::from(value)))
                    .collect(),
            };
            self.scan_with_worker(known, worker, timeout, cancel)
        }

        /// Scans the folder with the given worker command and a timeout.
        fn scan_with_worker(
            &self,
            known: &mut KnownPlugins,
            worker: WorkerCommand,
            timeout: Duration,
            cancel: bool,
        ) -> ScanReport {
            let lock = ScanLock::try_acquire(&self.directory.path().join("scan.lock"))
                .expect("lock file")
                .expect("no other scan");
            let options = ScanOptions {
                folders: vec![self.folder.clone()],
                worker,
                worker_log: Some(self.log_path()),
                timeout,
            };
            scan_plugins(&lock, known, &options, &|| cancel, &mut |_| {}).expect("scan runs")
        }

        /// Scans the folder with a shell script as the worker,
        /// which reads the request first so the scan can always send it.
        #[cfg(unix)]
        fn scan_with_script(&self, known: &mut KnownPlugins, script: &str) -> ScanReport {
            let worker = WorkerCommand {
                program: PathBuf::from("/bin/sh"),
                arguments: vec![OsString::from("-c"), OsString::from(format!("read request\n{script}"))],
                environment: Vec::new(),
            };
            self.scan_with_worker(known, worker, Duration::from_secs(20), false)
        }

        /// Returns the path of the worker log.
        fn log_path(&self) -> PathBuf {
            self.directory.path().join("worker.log")
        }

        /// Scans the folder with extra worker environment variables.
        fn scan(&self, known: &mut KnownPlugins, environment: &[(&str, &str)]) -> ScanReport {
            self.scan_with(known, environment, Duration::from_secs(20), false)
        }
    }

    /// Returns the path of the scan worker example, which Cargo builds with the tests of this crate.
    fn worker_path() -> PathBuf {
        let executable = std::env::current_exe().expect("test executable");
        let profile_folder = executable.parent().and_then(Path::parent).expect("Cargo target folder");
        profile_folder
            .join("examples")
            .join(format!("scan_worker{}", std::env::consts::EXE_SUFFIX))
    }

    #[test]
    fn finds_every_plugin_and_skips_unchanged_bundles() {
        let setup = Setup::new();
        let mut known = KnownPlugins::default();
        let report = setup.scan(&mut known, &[]);
        let mut names: Vec<&str> = report.added.iter().map(|plugin| plugin.name.as_str()).collect();
        names.sort_unstable();
        let mut expected: Vec<&str> = Kind::ALL.iter().map(|kind| kind.name()).collect();
        expected.sort_unstable();
        assert_eq!(names, expected);
        assert!(report.crashed.is_empty() && report.timed_out.is_empty() && report.no_plugins.is_empty());
        let mono = known
            .plugins
            .iter()
            .find(|plugin| plugin.name == Kind::MonoOnly.name())
            .expect("mono plugin");
        assert_eq!(
            (mono.main_input_channels, mono.main_output_channels),
            (Some(1), Some(1))
        );
        assert_eq!(mono.bundle, setup.bundle);
        assert_eq!(mono.category, "Fx|Dynamics");

        let report = setup.scan(&mut known, &[(CRASH, "1")]);
        assert_eq!(report, ScanReport::default());
        assert_eq!(known.plugins.len(), Kind::ALL.len());
    }

    #[test]
    fn a_crash_blacklists_the_bundle() {
        let setup = Setup::new();
        let mut known = KnownPlugins::default();
        let report = setup.scan(&mut known, &[(CRASH, "1")]);
        assert_eq!(report.crashed, vec![setup.bundle.clone()]);
        assert_eq!(known.blacklist, vec![setup.bundle.clone()]);
        assert_eq!(known.plugins, Vec::new());
        let report = setup.scan(&mut known, &[]);
        assert_eq!(report.added, Vec::new());
    }

    #[test]
    fn a_timeout_skips_the_bundle_without_blacklisting_it() {
        let setup = Setup::new();
        let mut known = KnownPlugins::default();
        let report = setup.scan_with(&mut known, &[(SLEEP, "5000")], Duration::from_millis(300), false);
        assert_eq!(report.timed_out, vec![setup.bundle.clone()]);
        assert_eq!(known.blacklist, Vec::<PathBuf>::new());
        assert_eq!(known.plugins, Vec::new());
    }

    #[test]
    fn a_busy_lock_refuses_a_second_scan() {
        let directory = tempfile::tempdir().expect("temporary folder");
        let path = directory.path().join("data").join("plugin-scan.lock");
        let first = ScanLock::try_acquire(&path).expect("lock file");
        assert!(first.is_some());
        assert!(ScanLock::try_acquire(&path).expect("lock file").is_none());
        drop(first);
        assert!(ScanLock::try_acquire(&path).expect("lock file").is_some());
    }

    #[test]
    fn missing_bundles_are_removed_only_inside_scanned_folders_that_exist() {
        let setup = Setup::new();
        let mut known = KnownPlugins::default();
        setup.scan(&mut known, &[]);
        let unmounted = PluginDescription {
            bundle: PathBuf::from("/Volumes/Unmounted/Plugins/Other.vst3"),
            ..known.plugins.first().expect("a plugin").clone()
        };
        known.plugins.push(unmounted.clone());
        known.blacklist.push(setup.folder.join("Gone.vst3"));
        std::fs::remove_dir_all(&setup.bundle).expect("bundle removed");
        let report = setup.scan(&mut known, &[]);
        assert_eq!(report.removed.len(), Kind::ALL.len());
        assert_eq!(known.plugins, vec![unmounted]);
        assert_eq!(known.blacklist, Vec::<PathBuf>::new());
    }

    #[test]
    fn module_info_is_read_without_the_worker_and_cancelling_stops_the_scan() {
        let setup = Setup::new();
        let described = setup.folder.join("Described.vst3");
        let resources = described.join("Contents").join("Resources");
        std::fs::create_dir_all(&resources).expect("bundle folders");
        let module_info = r#"{
            "Factory Info": { "Vendor": "Described Audio" },
            "Classes": [
                { "CID": "0123456789ABCDEF0123456789ABCDEF", "Category": "Audio Module Class",
                  "Name": "Described", "Sub Categories": ["Fx", "EQ"] }
            ]
        }"#;
        std::fs::write(resources.join("moduleinfo.json"), module_info).expect("module info");
        std::fs::remove_dir_all(&setup.bundle).expect("worker bundle removed");
        let mut known = KnownPlugins::default();
        let report = setup.scan(&mut known, &[(CRASH, "1")]);
        assert_eq!(report.added.len(), 1);
        let plugin = report.added.first().expect("described plugin");
        assert_eq!(
            (plugin.vendor.as_str(), plugin.category.as_str()),
            ("Described Audio", "Fx|EQ")
        );
        assert_eq!(plugin.main_input_channels, None);

        let mut fresh = KnownPlugins::default();
        let report = setup.scan_with(&mut fresh, &[], Duration::from_secs(20), true);
        assert!(report.cancelled);
        assert_eq!(fresh.plugins, Vec::new());
    }

    #[cfg(unix)]
    #[test]
    fn worker_output_around_the_reply_is_logged_and_ignored() {
        let setup = Setup::new();
        let mut known = KnownPlugins::default();
        let report = setup.scan_with_script(
            &mut known,
            r#"printf 'plugin noise\n\377\376 not text\n\n{"plugins": []}\n'
printf 'AUDIOBATCH-SCAN-REPLY {"plugins": [], "error": "Could not load"}\r\n'
exec cat > /dev/null"#,
        );
        assert_eq!(report.failed, vec![(setup.bundle.clone(), "Could not load".to_owned())]);
        assert!(report.crashed.is_empty() && known.blacklist.is_empty());
        let log = std::fs::read_to_string(setup.log_path()).expect("worker log");
        assert!(log.contains("plugin noise"), "{log}");
        assert!(log.contains("not text"), "{log}");
        assert!(log.contains(r#"{"plugins": []}"#), "{log}");
        assert!(!log.contains("AUDIOBATCH-SCAN-REPLY"), "{log}");
    }

    #[cfg(unix)]
    #[test]
    fn a_worker_that_exits_without_a_valid_reply_counts_as_a_crash() {
        for script in [
            "exit 0",
            "printf 'loading\\n'",
            r#"printf 'AUDIOBATCH-SCAN-REPLY {"plugins": [{"name": "Cut'"#,
            r#"printf 'AUDIOBATCH-SCAN-REPLY {"plugins": 5}\n'"#,
        ] {
            let setup = Setup::new();
            let mut known = KnownPlugins::default();
            let report = setup.scan_with_script(&mut known, script);
            assert_eq!(report.crashed, vec![setup.bundle.clone()], "{script}");
            assert_eq!(known.blacklist, vec![setup.bundle.clone()], "{script}");
            assert!(known.plugins.is_empty(), "{script}");
        }
    }

    #[cfg(unix)]
    #[test]
    fn a_reply_at_the_end_of_the_output_without_a_line_break_is_read() {
        let setup = Setup::new();
        let mut known = KnownPlugins::default();
        let report = setup.scan_with_script(&mut known, "printf 'AUDIOBATCH-SCAN-REPLY {}'");
        assert_eq!(report.no_plugins, vec![setup.bundle.clone()]);
        assert!(report.crashed.is_empty() && known.blacklist.is_empty());
    }

    #[test]
    fn the_summary_lists_only_what_happened() {
        assert_eq!(ScanReport::default().summary(), "Scan finished: no changes");
        let report = ScanReport {
            crashed: vec![PathBuf::from("/Crash.vst3")],
            timed_out: vec![PathBuf::from("/Slow.vst3"), PathBuf::from("/Slower.vst3")],
            cancelled: true,
            ..ScanReport::default()
        };
        assert_eq!(
            report.summary(),
            "Scan cancelled: 1 blacklisted after a crash, 2 timed out"
        );
    }
}
