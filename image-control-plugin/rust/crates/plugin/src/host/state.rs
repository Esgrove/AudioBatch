//! Bounded length-prefixed JSON persistence with complete detached validation before publication.

#![expect(unsafe_code, reason = "transfers bounded bytes through the host's VST3 stream ABI")]

use serde::{Deserialize, Serialize};
use thiserror::Error;
use vst3::ComRef;
use vst3::Steinberg::{IBStream, IBStreamTrait as _, kResultOk};

use crate::params::{Parameters, Snapshot};

/// Largest accepted payload regardless of a hostile host's length prefix.
const MAXIMUM_STATE_BYTES: usize = 4096;
/// Independent first-generation state schema for this component identity.
const STATE_VERSION: u32 = 1;

/// Failure to decode or transfer a bounded state, always outside the audio callback.
#[derive(Debug, Error)]
pub(super) enum StateError {
    /// A stream is missing, failed, or made no progress.
    #[error("The state stream could not transfer the requested bytes")]
    Stream,
    /// A length, version, or normalized value violates the state contract.
    #[error("The state does not satisfy the ImageControl RS format")]
    Invalid,
    /// The JSON payload is malformed.
    #[error("The state JSON is invalid")]
    Json(#[from] serde_json::Error),
}

/// Detached envelope that never retains a live editor or DSP reference.
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct State {
    /// Independent schema generation.
    version: u32,
    /// Normalized sound values with fixed defaults for missing fields.
    parameters: Snapshot,
}

/// Restores parameters without entering audio-owned history or waiting for the GUI.
///
/// # Safety
///
/// A non-null stream must reference a live host `IBStream` throughout this call.
pub(super) unsafe fn restore(stream: *mut IBStream, parameters: &Parameters) -> Result<(), StateError> {
    // SAFETY: caller guarantees a valid stream or null, rejected here.
    let stream = unsafe { ComRef::from_raw(stream) }.ok_or(StateError::Stream)?;
    let mut length = [0_u8; 4];
    transfer_read(&stream, &mut length)?;
    let length = usize::try_from(u32::from_le_bytes(length)).map_err(|_| StateError::Invalid)?;
    if length == 0 || length > MAXIMUM_STATE_BYTES {
        return Err(StateError::Invalid);
    }
    let mut bytes = vec![0; length];
    transfer_read(&stream, &mut bytes)?;
    let state: State = serde_json::from_slice(&bytes)?;
    if state.version != STATE_VERSION || !parameters.restore(state.parameters) {
        return Err(StateError::Invalid);
    }
    Ok(())
}

/// Serializes an independent atomic snapshot on a non-audio state thread.
///
/// # Safety
///
/// A non-null stream must reference a live writable host `IBStream` throughout this call.
pub(super) unsafe fn save(stream: *mut IBStream, parameters: &Parameters) -> Result<(), StateError> {
    // SAFETY: caller guarantees a valid stream or null, rejected here.
    let stream = unsafe { ComRef::from_raw(stream) }.ok_or(StateError::Stream)?;
    let mut bytes = serde_json::to_vec(&State {
        version: STATE_VERSION,
        parameters: parameters.snapshot(),
    })?;
    if bytes.len() > MAXIMUM_STATE_BYTES {
        return Err(StateError::Invalid);
    }
    let mut length = u32::try_from(bytes.len())
        .map_err(|_| StateError::Invalid)?
        .to_le_bytes();
    transfer_write(&stream, &mut length)?;
    transfer_write(&stream, &mut bytes)
}

/// Reads exactly the requested bounded bytes, rejecting EOF and impossible progress reports.
fn transfer_read(stream: &ComRef<'_, IBStream>, mut bytes: &mut [u8]) -> Result<(), StateError> {
    while !bytes.is_empty() {
        let count = i32::try_from(bytes.len()).map_err(|_| StateError::Invalid)?;
        let mut transferred = 0;
        // SAFETY: the bounded writable buffer covers count bytes, and the stream remains alive.
        let result = unsafe { stream.read(bytes.as_mut_ptr().cast(), count, &raw mut transferred) };
        if result != kResultOk || transferred <= 0 || transferred > count {
            return Err(StateError::Stream);
        }
        bytes = bytes.get_mut(transferred as usize..).ok_or(StateError::Stream)?;
    }
    Ok(())
}

/// Writes all bytes while rejecting a failed stream or one that makes no progress.
fn transfer_write(stream: &ComRef<'_, IBStream>, mut bytes: &mut [u8]) -> Result<(), StateError> {
    while !bytes.is_empty() {
        let count = i32::try_from(bytes.len()).map_err(|_| StateError::Invalid)?;
        let mut transferred = 0;
        // SAFETY: the bounded readable buffer covers count bytes, and the stream remains alive.
        let result = unsafe { stream.write(bytes.as_mut_ptr().cast(), count, &raw mut transferred) };
        if result != kResultOk || transferred <= 0 || transferred > count {
            return Err(StateError::Stream);
        }
        bytes = bytes.get_mut(transferred as usize..).ok_or(StateError::Stream)?;
    }
    Ok(())
}
