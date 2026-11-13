//! SQLite-backed cache for analysis results.
//!
//! Measurements are shared with the C++ application and validated against source identity and a common contract.
//! Optional previews live independently, so measurement reads never retain waveform or spectral blobs.
//! Gain and artifact updates preserve the original measurement producer.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, PoisonError};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use rusqlite::types::ValueRef;
use rusqlite::{Connection, OptionalExtension, Row, TransactionBehavior, params};

use crate::overload::OverloadHistory;
use crate::record::{
    AnalysisError, AnalysisRecord, AnalysisStatus, FileIdentity, MeasurementProducer, ShortTermLoudnessHistory,
    file_identity,
};

mod schema;
mod serato;
mod spectrogram;
mod waveform_cache;

/// Version of the analysis rules. Rows written with another version are re-analyzed.
const ANALYSIS_VERSION: i64 = 16;

/// Largest accepted loudness history payload, limiting allocations from untrusted databases to 16 MiB.
const MAXIMUM_HISTORY_BYTES: usize = 16 * 1024 * 1024;

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

/// Stored source version retained while pruning without holding the database connection lock.
struct CachedFileVersion {
    path: PathBuf,
    file_size: i64,
    modified_time_ms: i64,
}

/// Required measurement histories and signal-metric blobs serialized before taking the cache write lock.
struct MeasurementPayloads {
    loudness: Option<Vec<u8>>,
    overloads: Option<Vec<u8>>,
    dc_offsets: Vec<u8>,
    clip_counts: Vec<u8>,
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

    /// Returns shared successful measurements without loading either application's waveform.
    /// Invalid measurements miss independently of preview data, and failures are reusable only from Rust.
    #[must_use]
    pub fn get_analysis(&self, path: &Path) -> Option<AnalysisRecord> {
        let FileIdentity {
            file_size,
            modified_time_ms,
        } = file_identity(path)?;
        let mut record = AnalysisRecord {
            path: path.to_path_buf(),
            file_size,
            modified_time_ms,
            ..Default::default()
        };
        let file_size = i64::try_from(file_size).ok()?;
        self.with_connection(|connection| {
            Ok(connection
                .prepare_cached(
                    "SELECT file_size, modified_time_ms, analysis_version, file_name, format_name, length_in_samples,
                        duration_seconds, peak_left, peak_right, overall_peak, true_peak_left, true_peak_right,
                        overall_true_peak, max_short_term_lufs, integrated_lufs, sample_rate, channels,
                        bits_per_sample, status, error_message, custom_gain_db, short_term_loudness,
                        has_custom_gain, analysis_producer, overload_history,
                        plr_db, psr_db, crest_factor_db, dc_offsets, clip_counts, stereo_correlation
                 FROM file_analysis WHERE file_path = ?1 AND file_size = ?2 AND modified_time_ms = ?3
                    AND analysis_version = ?4 AND analysis_producer IN ('cpp', 'rust')
                    AND (status IN (1, 2) OR (status = 3 AND analysis_producer = 'rust'))",
                )?
                .query_row(
                    params![path_key(path), file_size, modified_time_ms, ANALYSIS_VERSION],
                    |row| {
                        if !valid_record_columns(row)? {
                            return Ok(None);
                        }
                        read_record_row(row, &mut record)?;
                        record.measurement_producer = match row.get_ref(23)?.as_str()? {
                            "cpp" => MeasurementProducer::Cpp,
                            "rust" => MeasurementProducer::Rust,
                            _ => return Ok(None),
                        };
                        record.from_cache = true;
                        if record.has_error() {
                            record.error = Some(AnalysisError::Unsupported);
                            return Ok(Some(record));
                        }
                        if !read_signal_metrics(row, &mut record)? || !valid_measurements(&record) {
                            return Ok(None);
                        }
                        let Some(history) = row
                            .get_ref(21)?
                            .as_blob()
                            .ok()
                            .and_then(|bytes| deserialize_history(bytes, &record))
                        else {
                            return Ok(None);
                        };
                        let Some(overloads) = row.get_ref(24)?.as_blob().ok().and_then(OverloadHistory::from_bytes)
                        else {
                            return Ok(None);
                        };
                        if !valid_overload_timing(&overloads, &record) {
                            return Ok(None);
                        }
                        record.short_term_loudness = Some(Arc::new(history));
                        record.overloads = Some(Arc::new(overloads));
                        record.status = AnalysisStatus::Cached;
                        Ok(Some(record))
                    },
                )
                .optional()?
                .flatten())
        })
        .ok()
        .flatten()
    }

    /// Atomically stores measurements and an optional native waveform for the captured source identity.
    /// Same-source writes preserve the latest explicit gain, including a clear, and compatible native previews.
    /// Repeatable failures never replace a same-source success.
    /// Stale work is ignored when the source no longer matches the record.
    /// Incompatible spectral artifacts are removed best effort only after required measurements commit.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid required histories or a failed database write.
    pub fn store_analysis(&self, record: &AnalysisRecord) -> Result<()> {
        if record.has_error()
            && (record.measurement_producer != MeasurementProducer::Rust
                || !record.error.is_some_and(AnalysisError::is_repeatable))
        {
            return Ok(());
        }
        if !record.is_ready() && !record.has_error() {
            return Ok(());
        }
        let observed_identity =
            self.with_connection(|connection| Ok(stored_measurement_identity(connection, &record.path)?))?;
        if !record.matches_file_on_disk() {
            return Ok(());
        }
        let payloads = measurement_payloads(record)?;
        let file_size = i64::try_from(record.file_size).unwrap_or(i64::MAX);
        self.with_connection(|connection| {
            // Taking the write lock up front keeps a concurrent writer from failing the upgrade from the read.
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            if !record.matches_file_on_disk() {
                return Ok(());
            }
            let current_identity = stored_measurement_identity(&transaction, &record.path)?;
            if current_identity != observed_identity
                && current_identity
                    != Some(FileIdentity {
                        file_size: record.file_size,
                        modified_time_ms: record.modified_time_ms,
                    })
            {
                return Ok(());
            }
            let updated = execute_store_analysis(&transaction, record, file_size, &payloads)?;
            if updated == 0 {
                transaction.commit()?;
                return Ok(());
            }
            waveform_cache::prune_incompatible(&transaction, record)?;
            transaction
                .prepare_cached(
                    "DELETE FROM serato_tag_cache WHERE file_path = ?1
                    AND (file_size != ?2 OR modified_time_ms != ?3)
                    AND EXISTS (SELECT 1 FROM file_analysis WHERE file_path = ?1
                        AND file_size = ?2 AND modified_time_ms = ?3)",
                )?
                .execute(params![path_key(&record.path), file_size, record.modified_time_ms])?;
            if !record.has_error()
                && let Some(waveform) = record.waveform.as_deref()
            {
                waveform_cache::store(&transaction, record, waveform)?;
            }
            if !record.matches_file_on_disk() {
                return Ok(());
            }
            transaction.commit()?;
            // Optional spectral cleanup cannot undo the required measurement commit.
            if let Err(error) = spectrogram::prune_incompatible(connection, record) {
                tracing::warn!(path = %record.path.display(), %error, "Could not remove obsolete spectrogram artifacts");
            }
            Ok(())
        })
    }

    /// Sets or clears the custom gain of a file in its cached row.
    /// Returns true when a row was updated.
    /// A file without a cached row, such as one still waiting for its analysis, keeps no gain in the cache.
    ///
    /// # Errors
    ///
    /// Returns an error when the database write fails.
    pub fn store_custom_gain(&self, path: &Path, custom_gain_db: Option<f32>) -> Result<bool> {
        let Some(FileIdentity {
            file_size,
            modified_time_ms,
        }) = file_identity(path)
        else {
            return Ok(false);
        };
        self.store_custom_gain_for_record(
            &AnalysisRecord {
                path: path.to_path_buf(),
                file_size,
                modified_time_ms,
                ..Default::default()
            },
            custom_gain_db,
        )
    }

    /// Publishes an explicit gain edit only to the captured source version.
    /// Callers must not use this to replay a gain inherited from an earlier measurement record.
    ///
    /// # Errors
    ///
    /// Returns an error when SQLite cannot update the row.
    pub fn store_custom_gain_for_record(&self, record: &AnalysisRecord, custom_gain_db: Option<f32>) -> Result<bool> {
        if custom_gain_db.is_some_and(|gain| !gain.is_finite()) {
            return Ok(false);
        }
        self.with_connection(|connection| {
            Ok(connection
                .prepare_cached(
                    "UPDATE file_analysis SET custom_gain_db = ?1, has_custom_gain = ?2, updated_at_ms = ?3
                    WHERE file_path = ?4 AND file_size = ?5 AND modified_time_ms = ?6",
                )
                .and_then(|mut statement| {
                    statement.execute(params![
                        f64::from(custom_gain_db.unwrap_or_default()),
                        custom_gain_db.is_some(),
                        now_ms(),
                        path_key(&record.path),
                        i64::try_from(record.file_size).unwrap_or(-1),
                        record.modified_time_ms
                    ])
                })
                .map_err(CacheError::StoreCustomGain)?
                > 0)
        })
    }

    /// Removes captured-identity measurements, tags, native previews, and all spectral revisions for one file.
    /// Returns true when at least one row was removed.
    ///
    /// # Errors
    ///
    /// Returns an error when the database write fails.
    pub fn remove_analysis(&self, path: &Path) -> Result<bool> {
        let identity =
            file_identity(path).or(self.with_connection(|connection| Ok(stored_identity(connection, path)?))?);
        self.with_connection(|connection| {
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            let removed = identity.map_or(Ok(0), |identity| {
                remove_identity(&transaction, path, identity.file_size, identity.modified_time_ms)
            })?;
            transaction.commit()?;
            Ok(removed > 0)
        })
    }

    /// Removes only the captured source version, leaving a newer measurement or preview untouched.
    ///
    /// # Errors
    ///
    /// Returns an error when the atomic database deletion fails.
    pub fn remove_analysis_for_record(&self, record: &AnalysisRecord) -> Result<bool> {
        self.with_connection(|connection| {
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            let removed = remove_identity(&transaction, &record.path, record.file_size, record.modified_time_ms)?;
            transaction.commit()?;
            Ok(removed > 0)
        })
    }

    /// Removes captured-identity measurements and optional artifacts of several files in one transaction.
    ///
    /// # Errors
    ///
    /// Returns an error when the database write fails, in which case no row is removed.
    pub fn remove_analyses(&self, paths: &[PathBuf]) -> Result<()> {
        let mut identities: Vec<_> = paths.iter().map(|path| (path, file_identity(path))).collect();
        self.with_connection(|connection| {
            for (path, identity) in &mut identities {
                if identity.is_none() {
                    *identity = stored_identity(connection, path)?;
                }
            }
            Ok(())
        })?;
        self.with_connection(|connection| {
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            for (path, identity) in identities {
                if let Some(identity) = identity {
                    remove_identity(&transaction, path, identity.file_size, identity.modified_time_ms)?;
                }
            }
            transaction.commit()?;
            Ok(())
        })
    }

    /// Deletes the rows of files that no longer exist, in every measurement and artifact table.
    ///
    /// Rows whose folder is missing are kept, since that usually means an unmounted drive, not deleted files,
    /// and those rows hold custom gains that would be lost for good.
    /// A row is only deleted while it still describes the checked version,
    /// so a file recreated in the meantime keeps its new row.
    /// Filesystem checks run without holding the connection lock, and `should_stop` ends the pass early.
    /// Returns the number of deleted rows.
    ///
    /// # Errors
    ///
    /// Returns an error when the database cannot be read or written.
    pub fn prune_missing_files(&self, should_stop: &dyn Fn() -> bool) -> Result<usize> {
        let versions = self.with_connection(|connection| {
            let mut statement = connection.prepare(
                "SELECT DISTINCT file_path, file_size, modified_time_ms FROM (
                    SELECT file_path, file_size, modified_time_ms FROM file_analysis
                    UNION ALL SELECT file_path, file_size, modified_time_ms FROM serato_tag_cache
                    UNION ALL SELECT file_path, file_size, modified_time_ms FROM rust_waveform_cache
                    UNION ALL SELECT file_path, file_size, modified_time_ms FROM spectrogram_cache)
                WHERE typeof(file_path) = 'text' AND typeof(file_size) = 'integer'
                    AND typeof(modified_time_ms) = 'integer'",
            )?;
            let rows = statement.query_map([], |row| {
                Ok(CachedFileVersion {
                    path: PathBuf::from(row.get::<_, String>(0)?),
                    file_size: row.get(1)?,
                    modified_time_ms: row.get(2)?,
                })
            })?;
            Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
        })?;
        let mut missing = Vec::new();
        for CachedFileVersion {
            path,
            file_size: size,
            modified_time_ms: modified,
        } in versions
        {
            if should_stop() {
                break;
            }
            let parent_exists = path.parent().is_some_and(Path::is_dir);
            if !path.is_absolute() || path.is_file() || !parent_exists {
                continue;
            }
            let Ok(size) = u64::try_from(size) else {
                continue;
            };
            missing.push((path, size, modified));
        }
        let removed = if !missing.is_empty() && !should_stop() {
            self.with_connection(|connection| {
                let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
                let mut total = 0;
                for (path, size, modified) in missing {
                    if should_stop() {
                        break;
                    }
                    total += remove_identity(&transaction, &path, size, modified)?;
                }
                transaction.commit()?;
                Ok(total)
            })?
        } else {
            0
        };
        if removed > 0 {
            tracing::info!("Removed {removed} cached rows for files that no longer exist");
        }
        Ok(removed)
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

    /// Opens a connection and ensures the schema is current.
    fn connect(&self) -> Result<Connection> {
        if let Some(parent) = self.database_path.parent() {
            std::fs::create_dir_all(parent).map_err(|source| CacheError::CreateDirectory {
                path: parent.to_path_buf(),
                source,
            })?;
        }
        let mut connection = Connection::open(&self.database_path).map_err(|source| CacheError::Open {
            path: self.database_path.clone(),
            source,
        })?;
        connection.busy_timeout(BUSY_TIMEOUT)?;
        connection.pragma_update(None, "journal_mode", "WAL")?;
        connection.pragma_update(None, "synchronous", "NORMAL")?;
        schema::initialize(&mut connection)?;
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

/// Returns the directory for Rust settings, logs, and plugin data, separate from the shared measurement database.
///
/// An explicit absolute `AUDIOBATCH_DATA_ROOT` takes precedence over platform paths.
/// Falls back to the temporary directory only when no override or platform user directory exists.
///
/// # Panics
/// Panics for an invalid explicit data root instead of falling back to production storage.
#[must_use]
pub fn data_directory() -> PathBuf {
    crate::storage::StoragePaths::from_environment()
        .expect("valid AUDIOBATCH_DATA_ROOT")
        .data()
        .to_path_buf()
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

/// Returns the default database path in the user data directory.
#[must_use]
fn default_database_path() -> PathBuf {
    crate::storage::StoragePaths::from_environment()
        .expect("valid AUDIOBATCH_DATA_ROOT")
        .database()
        .to_path_buf()
}

/// Reads a signed SQLite integer without allowing a negative source length to become an empty stream.
fn read_nonnegative_integer(row: &Row<'_>, column: usize) -> rusqlite::Result<u64> {
    let value: i64 = row.get(column)?;
    u64::try_from(value).map_err(|_| rusqlite::Error::IntegralValueOutOfRange(column, value))
}

/// Validates SQLite storage types and text budgets before creating owned strings or measurement arrays.
fn valid_record_columns(row: &Row<'_>) -> rusqlite::Result<bool> {
    for column in [0, 1, 2, 5, 15, 16, 17, 18, 22] {
        if !matches!(row.get_ref(column)?, ValueRef::Integer(_)) {
            return Ok(false);
        }
    }
    for column in [6, 7, 8, 9, 10, 11, 12, 13, 14, 20] {
        let value = match row.get_ref(column)? {
            ValueRef::Integer(value) => value as f64,
            ValueRef::Real(value) => value,
            _ => return Ok(false),
        };
        if !value.is_finite() {
            return Ok(false);
        }
    }
    for column in [7, 8, 9, 20] {
        if row.get::<_, f64>(column)?.abs() > f64::from(f32::MAX) {
            return Ok(false);
        }
    }
    for (column, maximum, nullable) in [(3, 4096, false), (4, 4096, true), (19, 65_536, true), (23, 4, false)] {
        match row.get_ref(column)? {
            ValueRef::Text(bytes) if bytes.len() <= maximum => {}
            ValueRef::Null if nullable => {}
            _ => return Ok(false),
        }
    }
    let has_gain: i64 = row.get(22)?;
    let gain = row.get::<_, f64>(20)? as f32;
    Ok((0..=1).contains(&has_gain) && (has_gain == 0 || gain.is_finite()))
}

/// Fills the record fields stored in the row, from column 3 to column 20.
fn read_record_row(row: &Row<'_>, record: &mut AnalysisRecord) -> rusqlite::Result<()> {
    record.file_name = row.get(3)?;
    record.format_name = row.get::<_, Option<String>>(4)?.unwrap_or_default();
    record.length_in_samples = read_nonnegative_integer(row, 5)?;
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
    record.custom_gain_db = if row.get::<_, i64>(22)? == 1 {
        row.get::<_, Option<f64>>(20)?.and_then(stored_gain_to_decibels)
    } else {
        None
    };
    Ok(())
}

/// Reads required bounded source statistics without accepting SQLite text coercions or oversized arrays.
fn read_signal_metrics(row: &Row<'_>, record: &mut AnalysisRecord) -> rusqlite::Result<bool> {
    let metrics = &mut record.signal_metrics;
    for (column, destination) in [
        (25, &mut metrics.plr_db),
        (26, &mut metrics.psr_db),
        (27, &mut metrics.crest_factor_db),
        (30, &mut metrics.stereo_correlation),
    ] {
        *destination = match row.get_ref(column)? {
            ValueRef::Null => None,
            ValueRef::Real(value) if value.is_finite() => Some(value),
            ValueRef::Integer(value) => Some(value as f64),
            _ => return Ok(false),
        };
    }
    let channels = record.channels as usize;
    if !(1..=64).contains(&channels) {
        return Ok(false);
    }
    let Ok(offsets) = row.get_ref(28)?.as_blob() else {
        return Ok(false);
    };
    let Ok(counts) = row.get_ref(29)?.as_blob() else {
        return Ok(false);
    };
    let mean_channels = if record.length_in_samples == 0 { 0 } else { channels };
    if offsets.len() != mean_channels * 8 || counts.len() != channels * 8 {
        return Ok(false);
    }
    metrics.dc_offsets = offsets
        .as_chunks::<8>()
        .0
        .iter()
        .map(|bytes| f64::from_le_bytes(*bytes))
        .collect();
    metrics.clip_counts = counts
        .as_chunks::<8>()
        .0
        .iter()
        .map(|bytes| u64::from_le_bytes(*bytes))
        .collect();
    Ok(metrics
        .dc_offsets
        .iter()
        .all(|value| value.is_finite() && value.abs() <= f64::from(f32::MAX))
        && metrics
            .clip_counts
            .iter()
            .all(|value| *value <= record.length_in_samples)
        && metrics
            .stereo_correlation
            .is_none_or(|value| value.abs() <= 1.0 && channels <= 2)
        && (channels != 1
            || metrics
                .stereo_correlation
                .is_some_and(|value| (value - 1.0).abs() < f64::EPSILON)))
}

/// Returns a stored custom gain as decibels, dropping a value that is not a usable gain.
fn stored_gain_to_decibels(stored: f64) -> Option<f32> {
    let gain = stored as f32;
    gain.is_finite().then_some(gain)
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
        .prepare_cached(
            "SELECT CASE WHEN has_custom_gain = 1 THEN custom_gain_db ELSE NULL END FROM file_analysis
             WHERE file_path = ?1 AND file_size = ?2 AND modified_time_ms = ?3",
        )?
        .query_row(params![path_key(path), file_size, modified_time_ms], |row| {
            row.get::<_, Option<f64>>(0)
        })
        .optional()
        .map(|stored| {
            stored.map(|gain| StoredGain {
                custom_gain_db: gain.and_then(stored_gain_to_decibels),
            })
        })
}

/// Reads the stored measurement identity from `file_analysis` for optimistic concurrency checks.
fn stored_measurement_identity(connection: &Connection, path: &Path) -> rusqlite::Result<Option<FileIdentity>> {
    connection
        .prepare_cached(
            "SELECT file_size, modified_time_ms FROM file_analysis
             WHERE file_path = ?1 AND typeof(file_size) = 'integer' AND file_size >= 0
               AND typeof(modified_time_ms) = 'integer'",
        )?
        .query_row([path_key(path)], |row| {
            Ok(FileIdentity {
                file_size: read_nonnegative_integer(row, 0)?,
                modified_time_ms: row.get(1)?,
            })
        })
        .optional()
}

/// Reads a stored source identity when a removed file no longer has filesystem metadata.
fn stored_identity(connection: &Connection, path: &Path) -> rusqlite::Result<Option<FileIdentity>> {
    connection
        .prepare_cached(
            "SELECT file_size, modified_time_ms FROM (
                SELECT file_size, modified_time_ms FROM file_analysis WHERE file_path = ?1
                UNION ALL SELECT file_size, modified_time_ms FROM serato_tag_cache WHERE file_path = ?1
                UNION ALL SELECT file_size, modified_time_ms FROM rust_waveform_cache WHERE file_path = ?1
                UNION ALL SELECT file_size, modified_time_ms FROM spectrogram_cache WHERE file_path = ?1
            )
            WHERE typeof(file_size) = 'integer' AND file_size >= 0 AND typeof(modified_time_ms) = 'integer'
            LIMIT 1",
        )?
        .query_row([path_key(path)], |row| {
            Ok(FileIdentity {
                file_size: read_nonnegative_integer(row, 0)?,
                modified_time_ms: row.get(1)?,
            })
        })
        .optional()
}

/// Deletes each artifact with its own identity predicate in the caller's short transaction.
fn remove_identity(connection: &Connection, path: &Path, size: u64, modified: i64) -> rusqlite::Result<usize> {
    let key = path_key(path);
    let size = i64::try_from(size).unwrap_or(-1);
    let mut removed = 0;
    for query in [
        "DELETE FROM file_analysis WHERE file_path = ?1 AND file_size = ?2 AND modified_time_ms = ?3",
        "DELETE FROM serato_tag_cache WHERE file_path = ?1 AND file_size = ?2 AND modified_time_ms = ?3",
        "DELETE FROM rust_waveform_cache WHERE file_path = ?1 AND file_size = ?2 AND modified_time_ms = ?3",
        "DELETE FROM spectrogram_cache WHERE file_path = ?1 AND file_size = ?2 AND modified_time_ms = ?3",
    ] {
        removed += connection
            .prepare_cached(query)?
            .execute(params![&key, size, modified])?;
    }
    Ok(removed)
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

/// Returns the stable shared database spelling of a measurement origin.
const fn producer_name(producer: MeasurementProducer) -> &'static str {
    match producer {
        MeasurementProducer::Cpp => "cpp",
        MeasurementProducer::Rust => "rust",
    }
}

/// Executes the cached `STORE_ANALYSIS_SQL` upsert statement inside the caller's write transaction.
fn execute_store_analysis(
    connection: &Connection,
    record: &AnalysisRecord,
    file_size: i64,
    payloads: &MeasurementPayloads,
) -> Result<usize> {
    let custom_gain = record.custom_gain_db.filter(|gain| gain.is_finite());
    connection
        .prepare_cached(schema::STORE_ANALYSIS_SQL)
        .and_then(|mut statement| {
            statement.execute(params![
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
                payloads.loudness,
                ANALYSIS_VERSION,
                now_ms(),
                f64::from(custom_gain.unwrap_or_default()),
                custom_gain.is_some(),
                payloads.overloads,
                producer_name(record.measurement_producer),
                record.signal_metrics.plr_db,
                record.signal_metrics.psr_db,
                record.signal_metrics.crest_factor_db,
                payloads.dc_offsets,
                payloads.clip_counts,
                record.signal_metrics.stereo_correlation,
            ])
        })
        .map_err(CacheError::StoreAnalysis)
}

/// Validates and serializes required histories and signal-metric blobs before taking the measurement write lock.
fn measurement_payloads(record: &AnalysisRecord) -> Result<MeasurementPayloads> {
    let dc_offsets = record
        .signal_metrics
        .dc_offsets
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect::<Vec<_>>();
    let clip_counts = record
        .signal_metrics
        .clip_counts
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect::<Vec<_>>();
    if record.has_error() {
        return Ok(MeasurementPayloads {
            loudness: None,
            overloads: None,
            dc_offsets,
            clip_counts,
        });
    }
    let overloads = record
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
    if !valid_measurements(record) {
        return Err(CacheError::InvalidHistory);
    }
    Ok(MeasurementPayloads {
        loudness: Some(history),
        overloads: Some(overloads),
        dc_offsets,
        clip_counts,
    })
}

/// Rejects invalid geometry and nonfinite measurements without confusing digital silence with corruption.
fn valid_measurements(record: &AnalysisRecord) -> bool {
    (16..=2_822_400).contains(&record.sample_rate)
        && (1..=64).contains(&record.channels)
        && record.bits_per_sample <= 64
        && i64::try_from(record.length_in_samples).is_ok()
        && record.duration_seconds.is_finite()
        && record.duration_seconds >= 0.0
        && [record.peak_left, record.peak_right, record.overall_peak]
            .iter()
            .all(|value| value.is_finite())
        && [record.true_peak_left, record.true_peak_right, record.overall_true_peak]
            .iter()
            .all(|value| value.is_finite() && *value >= 0.0)
        && [record.max_short_term_lufs, record.integrated_lufs]
            .iter()
            .all(|value| {
                value.is_finite()
                    && *value >= crate::record::NEGATIVE_INFINITY_LOUDNESS
                    && *value <= f64::from(MAXIMUM_LOUDNESS)
            })
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
        || history.sample_rate.to_bits() != f64::from(record.sample_rate).to_bits()
        || history.length_in_samples != record.length_in_samples
        || history.samples_per_point != ShortTermLoudnessHistory::samples_per_point_for_rate(record.sample_rate)
    {
        return false;
    }
    history.samples_per_point != 0
        && count <= (MAXIMUM_HISTORY_BYTES - HISTORY_HEADER_BYTES) / 4
        && count as u64 == history.length_in_samples.div_ceil(u64::from(history.samples_per_point))
}

/// Rejects non-finite readings and values outside the possible loudness range.
fn valid_history_value(value: f32) -> bool {
    value.is_finite() && f64::from(value) >= crate::record::NEGATIVE_INFINITY_LOUDNESS && value <= MAXIMUM_LOUDNESS
}

/// Restores a history blob after validating it against its record.
fn deserialize_history(bytes: &[u8], record: &AnalysisRecord) -> Option<ShortTermLoudnessHistory> {
    if bytes.len() > MAXIMUM_HISTORY_BYTES {
        return None;
    }
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
    if !payload
        .as_chunks::<4>()
        .0
        .iter()
        .all(|chunk| valid_history_value(f32::from_le_bytes(*chunk)))
    {
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
mod test_migration;
#[cfg(test)]
mod test_overload;
#[cfg(test)]
mod test_publication;
#[cfg(test)]
mod test_shared;
#[cfg(test)]
mod test_spectrogram;
#[cfg(test)]
mod test_spectrogram_cleanup;

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
        assert!(cached.waveform.is_none());
        assert_eq!(cache.get_waveform(&record.path), record.waveform);
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

    /// Measurement geometry and independently encoded loudness history used by serialization tests.
    struct HistoryFixture {
        record: AnalysisRecord,
        history: ShortTermLoudnessHistory,
    }

    /// Returns measurement geometry with a short-term loudness history of the values.
    fn record_with_history(values: Vec<f32>) -> HistoryFixture {
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
        HistoryFixture { record, history }
    }

    #[test]
    fn history_round_trips() {
        let HistoryFixture { record, history } = record_with_history(vec![-20.0, -10.0]);
        let bytes = serialize_history(&history, &record).expect("serializes");
        assert_eq!(bytes.len(), HISTORY_HEADER_BYTES + 8);
        assert_eq!(deserialize_history(&bytes, &record), Some(history));
    }

    /// Verifies history encoding against independent literal bytes rather than only a round trip.
    #[test]
    fn independent_shared_history_bytes_decode_and_serialize_exactly() {
        let HistoryFixture { record, history } = record_with_history(vec![-20.0, -10.0]);
        let bytes = [
            b'L', b'U', b'F', b'S', 1, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x88, 0xe5, 0x40, 0x74, 0x22, 0, 0, 0, 0, 0, 0, 0x3a,
            0x11, 0, 0, 2, 0, 0, 0, 0, 0, 0xa0, 0xc1, 0, 0, 0x20, 0xc1,
        ];
        assert_eq!(deserialize_history(&bytes, &record), Some(history.clone()));
        assert_eq!(serialize_history(&history, &record).expect("serializes"), bytes);
    }

    #[test]
    fn wrong_count_is_rejected() {
        let HistoryFixture { record, history } = record_with_history(vec![-20.0]);
        assert!(serialize_history(&history, &record).is_none());
    }

    #[test]
    fn invalid_values_are_rejected() {
        let HistoryFixture { record, history } = record_with_history(vec![f32::NAN, -10.0]);
        assert!(serialize_history(&history, &record).is_none());
    }

    #[test]
    fn truncated_blob_is_rejected() {
        let HistoryFixture { record, history } = record_with_history(vec![-20.0, -10.0]);
        let mut bytes = serialize_history(&history, &record).expect("serializes");
        bytes.pop();
        assert!(deserialize_history(&bytes, &record).is_none());
    }
}
