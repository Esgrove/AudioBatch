//! SQLite-backed cache for analysis results.
//!
//! Stores one row per file with the measured levels, loudness and overload histories, and the waveform overview,
//! keyed by path and validated against the file size and modification time,
//! so unchanged files are not decoded again on later runs.
//! Each row also keeps the custom gain the user set for that version of the file.
//! A second table, in the `serato` module, keeps what reading each file's Serato Overview tag found.
//! The database file and its schema are separate from the C++ application's cache,
//! because the two applications must never write to each other's cache.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, PoisonError};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use rusqlite::{Connection, OptionalExtension, Row, TransactionBehavior, params};

use crate::overload::OverloadHistory;
use crate::record::{
    AnalysisError, AnalysisRecord, AnalysisStatus, ShortTermLoudnessHistory, custom_gain_from_decibels, file_identity,
};
use crate::waveform::{self, WaveformOverview};

mod serato;

#[cfg(test)]
mod test_overload;

/// Directory name of the application data, kept apart from the C++ application's `AudioBatch` directory.
pub(crate) const DATA_DIRECTORY_NAME: &str = "AudioBatch-rs";

/// Version of the analysis rules. Rows written with another version are re-analyzed.
const ANALYSIS_VERSION: i64 = 4;

/// Columns added to `file_analysis` after its first version, with their definitions,
/// which databases created before them gain when they are opened.
const ADDED_ANALYSIS_COLUMNS: &[(&str, &str)] = &[("custom_gain_db", "REAL"), ("overload_history", "BLOB")];

/// How long SQLite retries a locked database before failing a statement.
const BUSY_TIMEOUT: Duration = Duration::from_secs(2);

/// Identifies a serialized loudness history blob.
const HISTORY_MAGIC: i32 = 0x5346_554c;

/// Version of the serialized loudness history layout.
const HISTORY_VERSION: i32 = 1;

/// Size of the serialized loudness history header in bytes.
const HISTORY_HEADER_BYTES: usize = 32;

/// Highest plausible short-term loudness, deliberately above what finite float PCM can reach.
const MAXIMUM_LOUDNESS: f32 = 1000.0;

/// Cache validation, filesystem, or database failures.
#[derive(Debug, thiserror::Error)]
pub enum CacheError {
    /// The cache directory could not be created.
    #[error("Failed to create cache directory {}", path.display())]
    CreateDirectory {
        /// Directory to create.
        path: PathBuf,
        /// Filesystem failure.
        #[source]
        source: std::io::Error,
    },
    /// The cache database could not be opened.
    #[error("Failed to open analysis cache {}", path.display())]
    Open {
        /// Database file to open.
        path: PathBuf,
        /// SQLite failure.
        #[source]
        source: rusqlite::Error,
    },
    /// A database statement or transaction failed.
    #[error(transparent)]
    Sqlite(#[from] rusqlite::Error),
    /// Writing an analysis row failed.
    #[error("SQLite could not store the analysis")]
    StoreAnalysis(#[source] rusqlite::Error),
    /// Updating a custom gain failed.
    #[error("SQLite could not store the custom gain")]
    StoreCustomGain(#[source] rusqlite::Error),
    /// Writing a Serato tag row failed.
    #[error("SQLite could not store the Serato tags")]
    StoreSerato(#[source] rusqlite::Error),
    /// Adding a column to an older schema failed.
    #[error("SQLite could not add the {column} column to {table}")]
    AddColumn {
        /// Table being updated.
        table: String,
        /// Missing column.
        column: String,
        /// SQLite failure.
        #[source]
        source: rusqlite::Error,
    },
    /// A successful analysis has no valid loudness history.
    #[error("Cannot cache analysis without valid loudness history")]
    InvalidHistory,
    /// A successful analysis has no valid overload history matching its stream and duration.
    #[error("Cannot cache analysis without valid overload history")]
    InvalidOverloadHistory,
    /// A successful analysis has no waveform.
    #[error("Cannot cache analysis without a waveform overview")]
    MissingWaveform,
    /// The file identity needed for caching is unavailable.
    #[error("Cannot cache Serato tags of missing file {}", path.display())]
    MissingSeratoFile {
        /// File whose identity could not be read.
        path: PathBuf,
    },
    /// An overview has no valid cache representation.
    #[error("Cannot cache a Serato overview with {overview}")]
    InvalidSeratoOverview {
        /// Overview that could not be serialized.
        overview: Arc<serato_tags::Overview>,
    },
    /// No connection is available after opening the cache.
    #[error("Analysis cache is not open")]
    NotOpen,
}

/// Result of a cache operation, retaining its database or filesystem cause.
type Result<T> = std::result::Result<T, CacheError>;

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

/// The custom gain column of a cached row, which is empty when the user has not set a gain for the file.
struct StoredGain {
    custom_gain_db: Option<f32>,
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

    /// Creates a cache that keeps results only while the process runs,
    /// for when the database file cannot be opened.
    #[must_use]
    pub fn in_memory() -> Self {
        Self::with_path(":memory:")
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
    /// or a stored loudness history, overload history, or waveform is invalid.
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
                        bits_per_sample, status, error_message, custom_gain_db, short_term_loudness, waveform_data,
                        waveform_version, overload_history
                 FROM file_analysis WHERE file_path = ?1",
                params![path_key(path)],
                |row| {
                    let identity = RowIdentity {
                        file_size: row.get(0)?,
                        modified_time_ms: row.get(1)?,
                        analysis_version: row.get(2)?,
                    };
                    read_record_row(row, &mut record)?;
                    let history: Option<Vec<u8>> = row.get(21)?;
                    let waveform: Option<Vec<u8>> = row.get(22)?;
                    let waveform_version: i64 = row.get(23)?;
                    let overload_history: Option<Vec<u8>> = row.get(24)?;
                    Ok((identity, history, waveform, waveform_version, overload_history))
                },
                    )
                    .optional()?)
            })
            .ok()??;

        let (identity, history, waveform_data, waveform_version, overload_history) = row;
        if identity.file_size != i64::try_from(file_size).ok()?
            || identity.modified_time_ms != modified_time_ms
            || identity.analysis_version != ANALYSIS_VERSION
        {
            return None;
        }
        record.from_cache = true;
        if record.has_error() {
            // Only failures that repeat for the same bytes are stored.
            record.error = Some(AnalysisError::Unsupported);
            return Some(record);
        }
        let overloads = OverloadHistory::from_bytes(&overload_history?)?;
        if !valid_overload_timing(&overloads, &record) {
            return None;
        }
        record.overloads = Some(Arc::new(overloads));
        record.short_term_loudness = Some(Arc::new(deserialize_history(&history?, &record)?));
        if waveform_version != i64::from(waveform::SERIALIZATION_VERSION) {
            return None;
        }
        record.waveform = Some(Arc::new(WaveformOverview::from_bytes(&waveform_data?)?));
        record.status = AnalysisStatus::Cached;
        Some(record)
    }

    /// Stores a completed analysis record, replacing any earlier row for the file.
    /// A failure that reading the same bytes again would repeat is stored too,
    /// so an unchanged broken file is not decoded again.
    /// Other failures, such as a file that could not be opened, are skipped, so the next run tries the file again.
    /// The custom gain of an earlier row is kept while the file has the same size and modification time,
    /// and replaced by the record's gain otherwise,
    /// because a custom gain only applies to the version of the file it was set for.
    ///
    /// # Errors
    ///
    /// Returns an error when a successful record has no valid loudness history, overload history, or waveform,
    /// or when the database write fails.
    pub fn store_analysis(&self, record: &AnalysisRecord) -> Result<()> {
        if record.has_error() && !record.error.is_some_and(AnalysisError::is_repeatable) {
            return Ok(());
        }
        let (history, waveform_data, overload_history) = if record.has_error() {
            (None, None, None)
        } else {
            let overload_history = record
                .overloads
                .as_deref()
                .filter(|history| valid_overload_timing(history, record))
                .and_then(OverloadHistory::to_bytes)
                .ok_or(CacheError::InvalidOverloadHistory)?;
            let history = record
                .short_term_loudness
                .as_deref()
                .and_then(|history| serialize_history(history, record))
                .ok_or(CacheError::InvalidHistory)?;
            let waveform_data = record
                .waveform
                .as_deref()
                .map(WaveformOverview::to_bytes)
                .ok_or(CacheError::MissingWaveform)?;
            (Some(history), Some(waveform_data), Some(overload_history))
        };
        let file_size = i64::try_from(record.file_size).unwrap_or(i64::MAX);
        self.with_connection(|connection| {
            // Taking the write lock up front keeps a concurrent writer from failing the upgrade from the read.
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            let custom_gain = stored_custom_gain(&transaction, &record.path, file_size, record.modified_time_ms)?
                .map_or(record.custom_gain_db, |stored| stored.custom_gain_db);
            transaction
                .execute(
                    "INSERT OR REPLACE INTO file_analysis (
                    file_path, file_name, format_name, file_size, modified_time_ms, length_in_samples,
                    duration_seconds, peak_left, peak_right, overall_peak, true_peak_left, true_peak_right,
                    overall_true_peak, max_short_term_lufs, integrated_lufs, sample_rate, channels, bits_per_sample,
                    status, error_message, short_term_loudness, waveform_data, waveform_version, analysis_version,
                    updated_at_ms, custom_gain_db, overload_history
                ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19,
                          ?20, ?21, ?22, ?23, ?24, ?25, ?26, ?27)",
                    params![
                        path_key(&record.path),
                        record.file_name,
                        record.format_name,
                        file_size,
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
                        custom_gain.map(f64::from),
                        overload_history,
                    ],
                )
                .map_err(CacheError::StoreAnalysis)?;
            transaction.commit()?;
            Ok(())
        })
    }

    /// Returns the custom gain stored for a file when its row describes the given version of the file,
    /// so a fresh analysis of an unchanged file keeps the gain the user set.
    #[must_use]
    pub(crate) fn get_custom_gain(&self, path: &Path, file_size: u64, modified_time_ms: i64) -> Option<f32> {
        let file_size = i64::try_from(file_size).ok()?;
        self.with_connection(|connection| Ok(stored_custom_gain(connection, path, file_size, modified_time_ms)?))
            .ok()
            .flatten()
            .and_then(|stored| stored.custom_gain_db)
    }

    /// Sets or clears the custom gain of a file in its cached row.
    /// Returns true when a row was updated.
    /// A file without a cached row, such as one still waiting for its analysis, keeps no gain in the cache.
    ///
    /// # Errors
    ///
    /// Returns an error when the database write fails.
    pub fn store_custom_gain(&self, path: &Path, custom_gain_db: Option<f32>) -> Result<bool> {
        self.with_connection(|connection| {
            let updated = connection
                .execute(
                    "UPDATE file_analysis SET custom_gain_db = ?1, updated_at_ms = ?2 WHERE file_path = ?3",
                    params![custom_gain_db.map(f64::from), now_ms(), path_key(path)],
                )
                .map_err(CacheError::StoreCustomGain)?;
            Ok(updated > 0)
        })
    }

    /// Removes the cached analysis and Serato tag rows of a file. Returns true when a row was removed.
    ///
    /// # Errors
    ///
    /// Returns an error when the database write fails.
    pub fn remove_analysis(&self, path: &Path) -> Result<bool> {
        self.with_connection(|connection| {
            let transaction = connection.transaction()?;
            let removed = transaction.execute(
                "DELETE FROM file_analysis WHERE file_path = ?1",
                params![path_key(path)],
            )? + serato::remove_rows(&transaction, path)?;
            transaction.commit()?;
            Ok(removed > 0)
        })
    }

    /// Removes the cached analysis and Serato tag rows of several files in one transaction.
    ///
    /// # Errors
    ///
    /// Returns an error when the database write fails, in which case no row is removed.
    pub fn remove_analyses(&self, paths: &[PathBuf]) -> Result<()> {
        self.with_connection(|connection| {
            let transaction = connection.transaction()?;
            for path in paths {
                transaction.execute(
                    "DELETE FROM file_analysis WHERE file_path = ?1",
                    params![path_key(path)],
                )?;
                serato::remove_rows(&transaction, path)?;
            }
            transaction.commit()?;
            Ok(())
        })
    }

    /// Opens a connection and ensures the schema is current.
    fn connect(&self) -> Result<Connection> {
        if let Some(parent) = self.database_path.parent() {
            std::fs::create_dir_all(parent).map_err(|source| CacheError::CreateDirectory {
                path: parent.to_path_buf(),
                source,
            })?;
        }
        let connection = Connection::open(&self.database_path).map_err(|source| CacheError::Open {
            path: self.database_path.clone(),
            source,
        })?;
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
                updated_at_ms INTEGER NOT NULL,
                custom_gain_db REAL,
                overload_history BLOB
            );",
        )?;
        add_missing_columns(&connection, "file_analysis", ADDED_ANALYSIS_COLUMNS)?;
        connection.execute_batch(serato::SCHEMA)?;
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
        let result = guard.as_mut().map_or_else(|| Err(CacheError::NotOpen), operation);
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
pub(crate) fn default_database_path() -> PathBuf {
    data_directory().join("analysis.db")
}

/// Fills the record fields stored in the row, from column 3 to column 20.
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
    record.custom_gain_db = row.get::<_, Option<f64>>(20)?.and_then(stored_gain_to_decibels);
    Ok(())
}

/// Returns a stored custom gain as decibels, dropping a value that is not a usable gain.
fn stored_gain_to_decibels(stored: f64) -> Option<f32> {
    custom_gain_from_decibels(stored as f32)
}

/// Returns the custom gain column of the row for a file when the row describes the given version of the file,
/// or `None` when there is no such row.
fn stored_custom_gain(
    connection: &Connection,
    path: &Path,
    file_size: i64,
    modified_time_ms: i64,
) -> rusqlite::Result<Option<StoredGain>> {
    connection
        .query_row(
            "SELECT custom_gain_db FROM file_analysis
             WHERE file_path = ?1 AND file_size = ?2 AND modified_time_ms = ?3",
            params![path_key(path), file_size, modified_time_ms],
            |row| row.get::<_, Option<f64>>(0),
        )
        .optional()
        .map(|stored| {
            stored.map(|gain| StoredGain {
                custom_gain_db: gain.and_then(stored_gain_to_decibels),
            })
        })
}

/// Adds the listed columns a table is missing, so a database created by an earlier version keeps working.
fn add_missing_columns(connection: &Connection, table: &str, columns: &[(&str, &str)]) -> Result<()> {
    let existing: Vec<String> = connection
        .prepare(&format!("PRAGMA table_info({table})"))?
        .query_map([], |row| row.get(1))?
        .collect::<rusqlite::Result<_>>()?;
    for (name, definition) in columns {
        if !existing.iter().any(|column| column == name) {
            connection
                .execute_batch(&format!("ALTER TABLE {table} ADD COLUMN {name} {definition};"))
                .map_err(|source| CacheError::AddColumn {
                    table: table.to_owned(),
                    column: (*name).to_owned(),
                    source,
                })?;
        }
    }
    Ok(())
}

/// Returns the cache key of a file.
fn path_key(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

/// Returns the stored code of a status.
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

/// Checks overload metadata and the record's decoded duration before storing or accepting a cache hit.
fn valid_overload_timing(history: &OverloadHistory, record: &AnalysisRecord) -> bool {
    history.matches_stream(record.sample_rate, record.length_in_samples, record.channels)
        && record.duration_seconds.is_finite()
        && (record.duration_seconds - record.length_in_samples as f64 / f64::from(record.sample_rate)).abs() <= 1e-9
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

/// Writes the history with an explicit little-endian header, so the blob reads the same on every platform.
pub(crate) fn serialize_history(history: &ShortTermLoudnessHistory, record: &AnalysisRecord) -> Option<Vec<u8>> {
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

    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;

    use tempfile::TempDir;

    #[cfg(unix)]
    use crate::analysis::analyze_file;
    use crate::test_helpers::{analyze_fixture_copy, temporary_cache};

    #[test]
    fn stored_record_round_trips() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let record = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
        cache.store_analysis(&record).expect("stores");
        let cached = cache.get_analysis(&record.path).expect("cache hit");
        assert_eq!(cached.status, AnalysisStatus::Cached);
        assert!(cached.from_cache);
        assert!((cached.integrated_lufs - record.integrated_lufs).abs() < 1e-9);
        assert!((cached.overall_peak - record.overall_peak).abs() < f32::EPSILON);
        assert_eq!(cached.short_term_loudness, record.short_term_loudness);
        assert_eq!(cached.overloads, record.overloads);
        assert_eq!(cached.waveform, record.waveform);
        assert_eq!(cached.format_name, record.format_name);
    }

    #[test]
    fn in_memory_cache_stores_records() {
        let directory = TempDir::new().expect("temp dir");
        let cache = AnalysisCache::in_memory();
        let record = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
        cache.store_analysis(&record).expect("stores");
        assert!(cache.get_analysis(&record.path).is_some());
    }

    #[test]
    fn database_without_the_custom_gain_column_gains_it() {
        let directory = TempDir::new().expect("temp dir");
        let database_path = directory.path().join("analysis.db");
        AnalysisCache::with_path(database_path.clone())
            .open()
            .expect("creates the schema");
        Connection::open(&database_path)
            .and_then(|connection| connection.execute_batch("ALTER TABLE file_analysis DROP COLUMN custom_gain_db;"))
            .expect("drops the column");

        let cache = AnalysisCache::with_path(database_path);
        let record = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
        cache.store_analysis(&record).expect("stores");
        assert!(cache.store_custom_gain(&record.path, Some(-3.0)).expect("stores gain"));
        assert!(cache.get_analysis(&record.path).is_some());
    }

    #[test]
    fn changed_file_misses() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let record = analyze_fixture_copy(&directory, "sine_mono_44100_16.wav");
        cache.store_analysis(&record).expect("stores");
        std::fs::write(&record.path, b"changed").expect("overwrite");
        assert!(cache.get_analysis(&record.path).is_none());
    }

    #[test]
    fn failed_record_is_cached_as_failed() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let record = analyze_fixture_copy(&directory, "broken.wav");
        assert!(record.has_error());
        cache.store_analysis(&record).expect("stores");
        let cached = cache.get_analysis(&record.path).expect("cache hit");
        assert_eq!(cached.status, AnalysisStatus::Failed);
        assert_eq!(cached.error, Some(AnalysisError::Unsupported));
        assert_eq!(cached.error_message, record.error_message);
    }

    #[test]
    fn failures_that_may_not_repeat_are_not_cached() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let mut record = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
        record.status = AnalysisStatus::Failed;
        for kind in [
            AnalysisError::Unreadable,
            AnalysisError::DecodeFailed,
            AnalysisError::MeasurementFailed,
            AnalysisError::Panicked,
        ] {
            record.error = Some(kind);
            record.error_message = format!("{kind:?}");
            cache.store_analysis(&record).expect("skips the record");
            assert!(cache.get_analysis(&record.path).is_none(), "{kind:?} was cached");
        }
    }

    #[cfg(unix)]
    #[test]
    fn a_file_that_cannot_be_opened_is_not_cached() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let path = test_fixtures::copy_fixture(&directory, "sine_stereo_44100_16.wav", None);
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o000)).expect("removes permissions");
        let record = analyze_file(&path, &|| false, &mut |_| {});
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644)).expect("restores permissions");
        if std::fs::File::open(&path).is_ok() && record.error.is_none() {
            // Running as root ignores the permissions, so there is nothing to check.
            return;
        }
        assert_eq!(record.error, Some(AnalysisError::Unreadable));
        assert_eq!(record.error_message, "File could not be opened for reading");
        cache.store_analysis(&record).expect("skips the record");
        assert!(cache.get_analysis(&path).is_none());
    }

    #[test]
    fn record_without_history_is_rejected() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let mut record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
        record.short_term_loudness = None;
        assert!(cache.store_analysis(&record).is_err());
    }

    #[test]
    fn remove_deletes_only_the_given_row() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let first = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
        let second = analyze_fixture_copy(&directory, "sine_mono_44100_16.wav");
        cache.store_analysis(&first).expect("stores");
        cache.store_analysis(&second).expect("stores");
        assert!(cache.remove_analysis(&first.path).expect("removes"));
        assert!(!cache.remove_analysis(&first.path).expect("second remove"));
        assert!(cache.get_analysis(&second.path).is_some());
    }

    #[test]
    fn remove_many_deletes_every_given_row_in_one_transaction() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let first = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
        let second = analyze_fixture_copy(&directory, "sine_mono_44100_16.wav");
        let kept = analyze_fixture_copy(&directory, "sine_stereo_44100_16.flac");
        for record in [&first, &second, &kept] {
            cache.store_analysis(record).expect("stores");
        }
        cache
            .remove_analyses(&[first.path.clone(), second.path.clone()])
            .expect("removes");
        assert!(cache.get_analysis(&first.path).is_none());
        assert!(cache.get_analysis(&second.path).is_none());
        assert!(cache.get_analysis(&kept.path).is_some());
    }
}

#[cfg(test)]
mod test_custom_gain {
    use tempfile::TempDir;

    use crate::record::same_custom_gain;
    use crate::test_helpers::{analyze_fixture_copy, temporary_cache};

    #[test]
    fn stored_gain_comes_back_with_the_record() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let record = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
        cache.store_analysis(&record).expect("stores");
        assert_eq!(
            cache
                .get_analysis(&record.path)
                .and_then(|cached| cached.custom_gain_db),
            None
        );
        assert!(cache.store_custom_gain(&record.path, Some(-2.5)).expect("stores gain"));
        let cached = cache
            .get_analysis(&record.path)
            .and_then(|cached| cached.custom_gain_db);
        assert!(same_custom_gain(cached, Some(-2.5)), "{cached:?}");
        let stored = cache.get_custom_gain(&record.path, record.file_size, record.modified_time_ms);
        assert!(same_custom_gain(stored, Some(-2.5)), "{stored:?}");
        assert!(cache.store_custom_gain(&record.path, None).expect("clears gain"));
        assert_eq!(
            cache
                .get_analysis(&record.path)
                .and_then(|cached| cached.custom_gain_db),
            None
        );
    }

    #[test]
    fn file_without_a_row_keeps_no_gain() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let path = directory.path().join("missing.wav");
        assert!(!cache.store_custom_gain(&path, Some(1.0)).expect("updates nothing"));
        assert_eq!(cache.get_custom_gain(&path, 0, 0), None);
    }

    #[test]
    fn new_analysis_of_an_unchanged_file_keeps_the_gain() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let record = analyze_fixture_copy(&directory, "sine_mono_44100_16.wav");
        cache.store_analysis(&record).expect("stores");
        cache.store_custom_gain(&record.path, Some(3.0)).expect("stores gain");
        cache.store_analysis(&record).expect("stores again");
        let cached = cache
            .get_analysis(&record.path)
            .and_then(|cached| cached.custom_gain_db);
        assert!(same_custom_gain(cached, Some(3.0)), "{cached:?}");
    }

    #[test]
    fn explicit_clear_survives_a_stale_analysis_write() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let mut stale = analyze_fixture_copy(&directory, "sine_mono_44100_16.wav");
        stale.custom_gain_db = Some(-3.0);
        cache.store_analysis(&stale).expect("stores old gain");
        cache.store_custom_gain(&stale.path, None).expect("clears gain");
        cache.store_analysis(&stale).expect("stores stale result");
        let cached = cache.get_analysis(&stale.path).expect("cached analysis");
        assert_eq!(cached.custom_gain_db, None);
    }

    #[test]
    fn new_analysis_of_a_changed_file_drops_the_gain() {
        let directory = TempDir::new().expect("temp dir");
        let cache = temporary_cache(&directory);
        let record = analyze_fixture_copy(&directory, "sine_mono_44100_16.wav");
        cache.store_analysis(&record).expect("stores");
        cache.store_custom_gain(&record.path, Some(3.0)).expect("stores gain");
        let mut changed = record;
        changed.modified_time_ms += 1000;
        cache.store_analysis(&changed).expect("stores the changed file");
        assert_eq!(
            cache.get_custom_gain(&changed.path, changed.file_size, changed.modified_time_ms),
            None
        );
    }
}

#[cfg(test)]
mod test_history_serialization {
    use super::*;

    /// Returns an analyzed record with a short-term loudness history of the values, and that history.
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
