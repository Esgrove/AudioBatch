//! SQLite-backed cache for analysis results.
//!
//! Stores one row per file with the measured levels, the short-term loudness history, and the waveform overview,
//! keyed by path and validated against the file size and modification time,
//! so unchanged files are not decoded again on later runs.
//! Ports the analysis part of the C++ `AnalysisCache`, with its own database file and schema,
//! because the Rust and C++ versions must never write to each other's cache.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, PoisonError};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, anyhow};
use rusqlite::{Connection, OptionalExtension, Row, params};

use crate::record::{AnalysisRecord, AnalysisStatus, ShortTermLoudnessHistory, file_identity};
use crate::waveform::{self, WaveformOverview};

/// Directory name of the Rust port's data, kept apart from the C++ application's `AudioBatch` directory.
pub const DATA_DIRECTORY_NAME: &str = "AudioBatch-rs";

/// Version of the analysis rules. Rows written with another version are re-analyzed.
const ANALYSIS_VERSION: i64 = 1;

/// How long SQLite retries a locked database before failing a statement.
const BUSY_TIMEOUT: Duration = Duration::from_secs(2);

/// Identifies a serialized loudness history blob, matching the C++ layout.
const HISTORY_MAGIC: i32 = 0x5346_554c;

/// Version of the serialized loudness history layout.
const HISTORY_VERSION: i32 = 1;

/// Size of the serialized loudness history header in bytes.
const HISTORY_HEADER_BYTES: usize = 32;

/// Highest plausible short-term loudness, deliberately above what finite float PCM can reach.
const MAXIMUM_LOUDNESS: f32 = 1000.0;

/// Persistent SQLite cache of analysis results.
///
/// The connection is opened lazily and shared behind a mutex, so the cache can be used from worker threads.
pub struct AnalysisCache {
    database_path: PathBuf,
    connection: Mutex<Option<Connection>>,
}

/// Identity of the file version a cached row describes.
struct RowIdentity {
    file_size: i64,
    modified_time_ms: i64,
    analysis_version: i64,
}

impl AnalysisCache {
    /// Creates a cache at the default location in the user data directory.
    #[must_use]
    pub fn new() -> Self {
        Self::with_path(default_database_path())
    }

    /// Creates a cache backed by the given SQLite file, for example in tests.
    #[must_use]
    pub fn with_path(database_path: impl Into<PathBuf>) -> Self {
        Self {
            database_path: database_path.into(),
            connection: Mutex::new(None),
        }
    }

    /// Opens the default cache, ready to share between threads.
    ///
    /// # Errors
    ///
    /// Returns an error when the database cannot be opened or its schema created.
    pub fn open_default() -> Result<Arc<Self>> {
        let cache = Self::new();
        cache.open()?;
        Ok(Arc::new(cache))
    }

    /// Returns the on-disk SQLite file used by the cache.
    #[must_use]
    pub fn database_path(&self) -> &Path {
        &self.database_path
    }

    /// Opens the database and creates the schema when needed.
    /// Calling it again on an open cache does nothing.
    ///
    /// # Errors
    ///
    /// Returns an error when the directory or the database cannot be created.
    pub fn open(&self) -> Result<()> {
        self.with_connection(|_| Ok(()))
    }

    /// Returns the cached record for a file, or `None` when the file changed, the row is from another version,
    /// or the stored history or waveform is invalid.
    /// A cached successful record has the `Cached` status, and a cached failure keeps the `Failed` status.
    #[must_use]
    pub fn get_analysis(&self, path: &Path) -> Option<AnalysisRecord> {
        let (file_size, modified_time_ms) = file_identity(path)?;
        let mut record = AnalysisRecord::from_path(path);
        let row = self
            .with_connection(|connection| {
                Ok(connection
                    .query_row(
                "SELECT file_size, modified_time_ms, analysis_version, file_name, format_name, length_in_samples,
                        duration_seconds, peak_left, peak_right, overall_peak, true_peak_left, true_peak_right,
                        overall_true_peak, max_short_term_lufs, integrated_lufs, sample_rate, channels,
                        bits_per_sample, status, error_message, short_term_loudness, waveform_data, waveform_version
                 FROM file_analysis WHERE file_path = ?1",
                params![path_key(path)],
                |row| {
                    let identity = RowIdentity {
                        file_size: row.get(0)?,
                        modified_time_ms: row.get(1)?,
                        analysis_version: row.get(2)?,
                    };
                    read_record_row(row, &mut record)?;
                    let history: Option<Vec<u8>> = row.get(20)?;
                    let waveform: Option<Vec<u8>> = row.get(21)?;
                    let waveform_version: i64 = row.get(22)?;
                    Ok((identity, history, waveform, waveform_version))
                },
                    )
                    .optional()?)
            })
            .ok()??;

        let (identity, history, waveform_data, waveform_version) = row;
        if identity.file_size != i64::try_from(file_size).ok()?
            || identity.modified_time_ms != modified_time_ms
            || identity.analysis_version != ANALYSIS_VERSION
        {
            return None;
        }
        record.from_cache = true;
        if record.has_error() {
            return Some(record);
        }
        record.short_term_loudness = Some(Arc::new(deserialize_history(&history?, &record)?));
        if waveform_version != i64::from(waveform::SERIALIZATION_VERSION) {
            return None;
        }
        record.waveform = Some(Arc::new(WaveformOverview::from_bytes(&waveform_data?)?));
        record.status = AnalysisStatus::Cached;
        Some(record)
    }

    /// Stores a completed analysis record, replacing any earlier row for the file.
    /// Failed records are stored too, so an unchanged broken file is not decoded again.
    ///
    /// # Errors
    ///
    /// Returns an error when the record is a successful one without a valid history or waveform,
    /// or when the database write fails.
    pub fn store_analysis(&self, record: &AnalysisRecord) -> Result<()> {
        let (history, waveform_data) = if record.has_error() {
            (None, None)
        } else {
            let history = record
                .short_term_loudness
                .as_deref()
                .and_then(|history| serialize_history(history, record))
                .ok_or_else(|| anyhow!("Cannot cache analysis without valid loudness history"))?;
            let waveform_data = record
                .waveform
                .as_deref()
                .map(WaveformOverview::to_bytes)
                .ok_or_else(|| anyhow!("Cannot cache analysis without a waveform overview"))?;
            (Some(history), Some(waveform_data))
        };
        self.with_connection(|connection| {
            connection
                .execute(
                    "INSERT OR REPLACE INTO file_analysis (
                    file_path, file_name, format_name, file_size, modified_time_ms, length_in_samples,
                    duration_seconds, peak_left, peak_right, overall_peak, true_peak_left, true_peak_right,
                    overall_true_peak, max_short_term_lufs, integrated_lufs, sample_rate, channels, bits_per_sample,
                    status, error_message, short_term_loudness, waveform_data, waveform_version, analysis_version,
                    updated_at_ms
                ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19,
                          ?20, ?21, ?22, ?23, ?24, ?25)",
                    params![
                        path_key(&record.path),
                        record.file_name,
                        record.format_name,
                        i64::try_from(record.file_size).unwrap_or(i64::MAX),
                        record.modified_time_ms,
                        i64::try_from(record.length_in_samples).unwrap_or(i64::MAX),
                        record.duration_seconds,
                        f64::from(record.peak_left),
                        f64::from(record.peak_right),
                        f64::from(record.overall_peak),
                        record.true_peak_left,
                        record.true_peak_right,
                        record.overall_true_peak,
                        record.max_short_term_lufs,
                        record.integrated_lufs,
                        record.sample_rate,
                        record.channels,
                        record.bits_per_sample,
                        status_code(record.status),
                        record.error_message,
                        history,
                        waveform_data,
                        i64::from(waveform::SERIALIZATION_VERSION),
                        ANALYSIS_VERSION,
                        now_ms(),
                    ],
                )
                .context("SQLite could not store the analysis")?;
            Ok(())
        })
    }

    /// Removes the cached row of a file. Returns true when a row was removed.
    ///
    /// # Errors
    ///
    /// Returns an error when the database write fails.
    pub fn remove_analysis(&self, path: &Path) -> Result<bool> {
        self.with_connection(|connection| {
            let removed = connection.execute(
                "DELETE FROM file_analysis WHERE file_path = ?1",
                params![path_key(path)],
            )?;
            Ok(removed > 0)
        })
    }

    /// Removes the rows of files that no longer exist, and returns how many were removed.
    ///
    /// # Errors
    ///
    /// Returns an error when the database cannot be read or written.
    pub fn prune_missing_files(&self) -> Result<usize> {
        let paths: Vec<String> = self.with_connection(|connection| {
            let mut statement = connection.prepare("SELECT file_path FROM file_analysis")?;
            let paths = statement
                .query_map([], |row| row.get(0))?
                .collect::<rusqlite::Result<_>>()?;
            Ok(paths)
        })?;
        let mut removed = 0;
        for path in paths.iter().map(PathBuf::from).filter(|path| !path.is_file()) {
            if self.remove_analysis(&path)? {
                removed += 1;
            }
        }
        Ok(removed)
    }

    /// Opens a connection and ensures the schema is current.
    fn connect(&self) -> Result<Connection> {
        if let Some(parent) = self.database_path.parent() {
            std::fs::create_dir_all(parent)
                .with_context(|| format!("Failed to create cache directory {}", parent.display()))?;
        }
        let connection = Connection::open(&self.database_path)
            .with_context(|| format!("Failed to open analysis cache {}", self.database_path.display()))?;
        connection.busy_timeout(BUSY_TIMEOUT)?;
        connection.pragma_update(None, "journal_mode", "WAL")?;
        connection.pragma_update(None, "synchronous", "NORMAL")?;
        connection.execute_batch(
            "CREATE TABLE IF NOT EXISTS file_analysis (
                file_path TEXT PRIMARY KEY,
                file_name TEXT NOT NULL,
                format_name TEXT,
                file_size INTEGER NOT NULL,
                modified_time_ms INTEGER NOT NULL,
                length_in_samples INTEGER NOT NULL,
                duration_seconds REAL NOT NULL,
                peak_left REAL NOT NULL,
                peak_right REAL NOT NULL,
                overall_peak REAL NOT NULL,
                true_peak_left REAL NOT NULL,
                true_peak_right REAL NOT NULL,
                overall_true_peak REAL NOT NULL,
                max_short_term_lufs REAL NOT NULL,
                integrated_lufs REAL NOT NULL,
                sample_rate INTEGER NOT NULL,
                channels INTEGER NOT NULL,
                bits_per_sample INTEGER NOT NULL,
                status INTEGER NOT NULL,
                error_message TEXT,
                short_term_loudness BLOB,
                waveform_data BLOB,
                waveform_version INTEGER NOT NULL DEFAULT 0,
                analysis_version INTEGER NOT NULL,
                updated_at_ms INTEGER NOT NULL
            );",
        )?;
        Ok(connection)
    }

    /// Runs a closure with the connection, opening the database first when needed.
    ///
    /// The lock is held only for the closure.
    /// A poisoned mutex is recovered, since a panic in another thread does not leave the connection invalid.
    fn with_connection<T>(&self, operation: impl FnOnce(&mut Connection) -> Result<T>) -> Result<T> {
        let mut guard = self.connection.lock().unwrap_or_else(PoisonError::into_inner);
        if guard.is_none() {
            *guard = Some(self.connect()?);
        }
        let result = guard
            .as_mut()
            .map_or_else(|| Err(anyhow!("Analysis cache is not open")), operation);
        drop(guard);
        result
    }
}

impl Default for AnalysisCache {
    fn default() -> Self {
        Self::new()
    }
}

/// Returns the directory for the cache and the settings of the Rust version, in the user data directory.
/// Falls back to the temporary directory when the platform has no user data directory.
#[must_use]
pub fn data_directory() -> PathBuf {
    dirs::data_dir()
        .unwrap_or_else(std::env::temp_dir)
        .join(DATA_DIRECTORY_NAME)
}

/// Returns the default database path in the user data directory.
#[must_use]
pub fn default_database_path() -> PathBuf {
    data_directory().join("analysis.db")
}

/// Fills the record fields stored in the row, starting from column 3.
fn read_record_row(row: &Row<'_>, record: &mut AnalysisRecord) -> rusqlite::Result<()> {
    record.file_name = row.get(3)?;
    record.format_name = row.get::<_, Option<String>>(4)?.unwrap_or_default();
    record.length_in_samples = u64::try_from(row.get::<_, i64>(5)?).unwrap_or(0);
    record.duration_seconds = row.get(6)?;
    record.peak_left = row.get::<_, f64>(7)? as f32;
    record.peak_right = row.get::<_, f64>(8)? as f32;
    record.overall_peak = row.get::<_, f64>(9)? as f32;
    record.true_peak_left = row.get(10)?;
    record.true_peak_right = row.get(11)?;
    record.overall_true_peak = row.get(12)?;
    record.max_short_term_lufs = row.get(13)?;
    record.integrated_lufs = row.get(14)?;
    record.sample_rate = row.get(15)?;
    record.channels = row.get(16)?;
    record.bits_per_sample = row.get(17)?;
    record.status = status_from_code(row.get(18)?);
    record.error_message = row.get::<_, Option<String>>(19)?.unwrap_or_default();
    Ok(())
}

/// Returns the cache key of a file.
fn path_key(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

/// Returns the stored code of a status, the same values the C++ cache uses.
const fn status_code(status: AnalysisStatus) -> i64 {
    match status {
        AnalysisStatus::Pending => 0,
        AnalysisStatus::Cached => 1,
        AnalysisStatus::Analyzed => 2,
        AnalysisStatus::Failed => 3,
    }
}

/// Returns the status of a stored code.
const fn status_from_code(code: i64) -> AnalysisStatus {
    match code {
        1 => AnalysisStatus::Cached,
        2 => AnalysisStatus::Analyzed,
        3 => AnalysisStatus::Failed,
        _ => AnalysisStatus::Pending,
    }
}

/// Returns the current time in milliseconds since the Unix epoch.
fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .ok()
        .and_then(|elapsed| i64::try_from(elapsed.as_millis()).ok())
        .unwrap_or_default()
}

/// Checks the history timing against its record before serializing or after deserializing.
fn valid_history_timing(history: &ShortTermLoudnessHistory, record: &AnalysisRecord, count: usize) -> bool {
    if !history.sample_rate.is_finite()
        || history.sample_rate < 1.0
        || (history.sample_rate.round() as u64) != u64::from(record.sample_rate)
        || history.length_in_samples != record.length_in_samples
        || history.samples_per_point != ShortTermLoudnessHistory::samples_per_point_for_rate(record.sample_rate)
    {
        return false;
    }
    count as u64 == history.length_in_samples.div_ceil(u64::from(history.samples_per_point))
}

/// Rejects non-finite readings and values outside the possible loudness range.
fn valid_history_value(value: f32) -> bool {
    value.is_finite() && f64::from(value) >= crate::record::NEGATIVE_INFINITY_LOUDNESS && value <= MAXIMUM_LOUDNESS
}

/// Writes the history with an explicit little-endian header, in the same layout as the C++ cache.
fn serialize_history(history: &ShortTermLoudnessHistory, record: &AnalysisRecord) -> Option<Vec<u8>> {
    if !valid_history_timing(history, record, history.values.len())
        || !history.values.iter().copied().all(valid_history_value)
    {
        return None;
    }
    let mut bytes = Vec::with_capacity(HISTORY_HEADER_BYTES + history.values.len() * 4);
    bytes.extend_from_slice(&HISTORY_MAGIC.to_le_bytes());
    bytes.extend_from_slice(&HISTORY_VERSION.to_le_bytes());
    bytes.extend_from_slice(&history.sample_rate.to_le_bytes());
    bytes.extend_from_slice(&i64::try_from(history.length_in_samples).ok()?.to_le_bytes());
    bytes.extend_from_slice(&i32::try_from(history.samples_per_point).ok()?.to_le_bytes());
    bytes.extend_from_slice(&i32::try_from(history.values.len()).ok()?.to_le_bytes());
    for value in &history.values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    Some(bytes)
}

/// Restores a history blob after validating it against its record.
fn deserialize_history(bytes: &[u8], record: &AnalysisRecord) -> Option<ShortTermLoudnessHistory> {
    let (header, payload) = bytes.split_at_checked(HISTORY_HEADER_BYTES)?;
    let (magic, header) = header.split_first_chunk::<4>()?;
    let (version, header) = header.split_first_chunk::<4>()?;
    let (sample_rate, header) = header.split_first_chunk::<8>()?;
    let (length, header) = header.split_first_chunk::<8>()?;
    let (samples_per_point, header) = header.split_first_chunk::<4>()?;
    let (count, _) = header.split_first_chunk::<4>()?;
    if i32::from_le_bytes(*magic) != HISTORY_MAGIC || i32::from_le_bytes(*version) != HISTORY_VERSION {
        return None;
    }
    let count = usize::try_from(i32::from_le_bytes(*count)).ok()?;
    let mut history = ShortTermLoudnessHistory {
        values: Vec::new(),
        sample_rate: f64::from_le_bytes(*sample_rate),
        length_in_samples: u64::try_from(i64::from_le_bytes(*length)).ok()?,
        samples_per_point: u32::try_from(i32::from_le_bytes(*samples_per_point)).ok()?,
    };
    if !valid_history_timing(&history, record, count) || payload.len() != count.checked_mul(4)? {
        return None;
    }
    history.values = payload
        .as_chunks::<4>()
        .0
        .iter()
        .map(|chunk| f32::from_le_bytes(*chunk))
        .collect();
    history
        .values
        .iter()
        .copied()
        .all(valid_history_value)
        .then_some(history)
}

#[cfg(test)]
mod test_analysis_cache {
    use super::*;

    use tempfile::TempDir;

    use crate::analysis::analyze_file;

    fn analyzed_copy(directory: &TempDir, fixture: &str) -> AnalysisRecord {
        let path = test_fixtures::copy_fixture(directory, fixture, None);
        analyze_file(&path, &|| false, &mut |_| {})
    }

    fn cache_in(directory: &TempDir) -> AnalysisCache {
        let cache = AnalysisCache::with_path(directory.path().join("cache/analysis.db"));
        cache.open().expect("cache opens");
        cache
    }

    #[test]
    fn stored_record_round_trips() {
        let directory = TempDir::new().expect("temp dir");
        let cache = cache_in(&directory);
        let record = analyzed_copy(&directory, "sine_stereo_44100_16.wav");
        cache.store_analysis(&record).expect("stores");
        let cached = cache.get_analysis(&record.path).expect("cache hit");
        assert_eq!(cached.status, AnalysisStatus::Cached);
        assert!(cached.from_cache);
        assert!((cached.integrated_lufs - record.integrated_lufs).abs() < 1e-9);
        assert!((cached.overall_peak - record.overall_peak).abs() < f32::EPSILON);
        assert_eq!(cached.short_term_loudness, record.short_term_loudness);
        assert_eq!(cached.waveform, record.waveform);
        assert_eq!(cached.format_name, record.format_name);
    }

    #[test]
    fn changed_file_misses() {
        let directory = TempDir::new().expect("temp dir");
        let cache = cache_in(&directory);
        let record = analyzed_copy(&directory, "sine_mono_44100_16.wav");
        cache.store_analysis(&record).expect("stores");
        std::fs::write(&record.path, b"changed").expect("overwrite");
        assert!(cache.get_analysis(&record.path).is_none());
    }

    #[test]
    fn failed_record_is_cached_as_failed() {
        let directory = TempDir::new().expect("temp dir");
        let cache = cache_in(&directory);
        let record = analyzed_copy(&directory, "broken.wav");
        assert!(record.has_error());
        cache.store_analysis(&record).expect("stores");
        let cached = cache.get_analysis(&record.path).expect("cache hit");
        assert_eq!(cached.status, AnalysisStatus::Failed);
        assert_eq!(cached.error_message, record.error_message);
    }

    #[test]
    fn record_without_history_is_rejected() {
        let directory = TempDir::new().expect("temp dir");
        let cache = cache_in(&directory);
        let mut record = analyzed_copy(&directory, "sine_mono_24000_16.wav");
        record.short_term_loudness = None;
        assert!(cache.store_analysis(&record).is_err());
    }

    #[test]
    fn remove_and_prune_delete_rows() {
        let directory = TempDir::new().expect("temp dir");
        let cache = cache_in(&directory);
        let first = analyzed_copy(&directory, "sine_stereo_44100_16.wav");
        let second = analyzed_copy(&directory, "sine_mono_44100_16.wav");
        cache.store_analysis(&first).expect("stores");
        cache.store_analysis(&second).expect("stores");
        assert!(cache.remove_analysis(&first.path).expect("removes"));
        assert!(!cache.remove_analysis(&first.path).expect("second remove"));
        std::fs::remove_file(&second.path).expect("delete file");
        assert_eq!(cache.prune_missing_files().expect("prunes"), 1);
    }
}

#[cfg(test)]
mod test_history_serialization {
    use super::*;

    fn record_with_history(values: Vec<f32>) -> (AnalysisRecord, ShortTermLoudnessHistory) {
        let history = ShortTermLoudnessHistory {
            values,
            sample_rate: 44_100.0,
            length_in_samples: 8820,
            samples_per_point: 4410,
        };
        let record = AnalysisRecord {
            sample_rate: 44_100,
            length_in_samples: 8820,
            ..Default::default()
        };
        (record, history)
    }

    #[test]
    fn history_round_trips() {
        let (record, history) = record_with_history(vec![-20.0, -10.0]);
        let bytes = serialize_history(&history, &record).expect("serializes");
        assert_eq!(bytes.len(), HISTORY_HEADER_BYTES + 8);
        assert_eq!(deserialize_history(&bytes, &record), Some(history));
    }

    #[test]
    fn wrong_count_is_rejected() {
        let (record, history) = record_with_history(vec![-20.0]);
        assert!(serialize_history(&history, &record).is_none());
    }

    #[test]
    fn invalid_values_are_rejected() {
        let (record, history) = record_with_history(vec![f32::NAN, -10.0]);
        assert!(serialize_history(&history, &record).is_none());
    }

    #[test]
    fn truncated_blob_is_rejected() {
        let (record, history) = record_with_history(vec![-20.0, -10.0]);
        let mut bytes = serialize_history(&history, &record).expect("serializes");
        bytes.pop();
        assert!(deserialize_history(&bytes, &record).is_none());
    }
}
