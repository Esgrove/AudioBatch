//! Parameter changes passed to and from `process`.
//!
//! All queues are allocated up front and store their points in atomics,
//! so neither the host nor the plugin allocates or locks on the processing thread.
//! The host adds at most one point per parameter, at the start of the block,
//! because edits come from an editor and have no sample position of their own.

#![expect(
    unsafe_code,
    reason = "implements the IParameterChanges and IParamValueQueue COM interfaces"
)]

use std::array;
use std::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, AtomicUsize, Ordering};

use vst3::Steinberg::Vst::{
    IParamValueQueue, IParamValueQueueTrait, IParameterChanges, IParameterChangesTrait, ParamID, ParamValue,
};
use vst3::Steinberg::{kInvalidArgument, kResultFalse, kResultOk, tresult};
use vst3::{Class, ComWrapper};

/// How many parameters can change in one block.
const MAXIMUM_PARAMETERS: usize = 256;

/// How many points one parameter queue holds.
const MAXIMUM_POINTS: usize = 16;

/// The changes of one parameter within a block.
pub struct ParamValueQueue {
    /// Parameter id.
    id: AtomicU32,
    /// Number of points in use.
    count: AtomicUsize,
    /// Sample offset of each point.
    offsets: [AtomicI32; MAXIMUM_POINTS],
    /// Value of each point, as `f64` bits.
    values: [AtomicU64; MAXIMUM_POINTS],
}

/// The parameter changes of one block.
pub struct ParameterChanges {
    /// Preallocated queues, the first `count` of which are in use.
    queues: Box<[ComWrapper<ParamValueQueue>]>,
    /// Number of queues in use.
    count: AtomicUsize,
}

impl Class for ParamValueQueue {
    type Interfaces = (IParamValueQueue,);
}

impl Class for ParameterChanges {
    type Interfaces = (IParameterChanges,);
}

impl ParamValueQueue {
    /// Creates an empty queue.
    fn new() -> Self {
        Self {
            id: AtomicU32::new(0),
            count: AtomicUsize::new(0),
            offsets: array::from_fn(|_| AtomicI32::new(0)),
            values: array::from_fn(|_| AtomicU64::new(0)),
        }
    }

    /// Starts the queue over for another parameter.
    fn reset(&self, id: ParamID) {
        self.id.store(id, Ordering::Relaxed);
        self.count.store(0, Ordering::Relaxed);
    }

    /// Replaces all points with a single point at the start of the block.
    fn set_single(&self, value: f64) {
        if let (Some(offset), Some(stored)) = (self.offsets.first(), self.values.first()) {
            offset.store(0, Ordering::Relaxed);
            stored.store(value.to_bits(), Ordering::Relaxed);
            self.count.store(1, Ordering::Relaxed);
        }
    }

    /// Returns the last point of the queue.
    #[cfg(test)]
    fn last(&self) -> Option<(i32, f64)> {
        let index = self.count.load(Ordering::Relaxed).checked_sub(1)?;
        let offset = self.offsets.get(index)?.load(Ordering::Relaxed);
        let value = f64::from_bits(self.values.get(index)?.load(Ordering::Relaxed));
        Some((offset, value))
    }
}

impl IParamValueQueueTrait for ParamValueQueue {
    unsafe fn getParameterId(&self) -> ParamID {
        self.id.load(Ordering::Relaxed)
    }

    unsafe fn getPointCount(&self) -> i32 {
        i32::try_from(self.count.load(Ordering::Relaxed)).unwrap_or(i32::MAX)
    }

    unsafe fn getPoint(&self, index: i32, sample_offset: *mut i32, value: *mut ParamValue) -> tresult {
        let Ok(index) = usize::try_from(index) else {
            return kInvalidArgument;
        };
        if index >= self.count.load(Ordering::Relaxed) {
            return kResultFalse;
        }
        let (Some(offset), Some(stored)) = (self.offsets.get(index), self.values.get(index)) else {
            return kResultFalse;
        };
        if !sample_offset.is_null() {
            // SAFETY: the offset slot is writable, checked for null above.
            unsafe { *sample_offset = offset.load(Ordering::Relaxed) };
        }
        if !value.is_null() {
            // SAFETY: the value slot is writable, checked for null above.
            unsafe { *value = f64::from_bits(stored.load(Ordering::Relaxed)) };
        }
        kResultOk
    }

    unsafe fn addPoint(&self, sample_offset: i32, value: ParamValue, index: *mut i32) -> tresult {
        let count = self.count.load(Ordering::Relaxed);
        let (Some(offset), Some(stored)) = (self.offsets.get(count), self.values.get(count)) else {
            return kResultFalse;
        };
        offset.store(sample_offset, Ordering::Relaxed);
        stored.store(value.to_bits(), Ordering::Relaxed);
        self.count.store(count + 1, Ordering::Relaxed);
        if !index.is_null() {
            // SAFETY: the index slot is writable, checked for null above.
            unsafe { *index = i32::try_from(count).unwrap_or(i32::MAX) };
        }
        kResultOk
    }
}

impl ParameterChanges {
    /// Creates an empty set of changes with all queues allocated.
    pub fn create() -> ComWrapper<Self> {
        ComWrapper::new(Self {
            queues: (0..MAXIMUM_PARAMETERS)
                .map(|_| ComWrapper::new(ParamValueQueue::new()))
                .collect(),
            count: AtomicUsize::new(0),
        })
    }

    /// Removes all changes.
    pub fn clear(&self) {
        self.count.store(0, Ordering::Relaxed);
    }

    /// Returns true when there are no changes.
    #[cfg(test)]
    pub fn is_empty(&self) -> bool {
        self.count.load(Ordering::Relaxed) == 0
    }

    /// Sets a parameter to a value at the start of the block, replacing an earlier value for the same parameter.
    ///
    /// Returns false when every queue is already in use by other parameters.
    pub fn set(&self, id: ParamID, value: f64) -> bool {
        self.queue_for(id).is_some_and(|queue| {
            queue.set_single(value);
            true
        })
    }

    /// Returns the last value of every changed parameter.
    #[cfg(test)]
    pub fn last_values(&self) -> impl Iterator<Item = (ParamID, f64)> + '_ {
        self.active_queues()
            .iter()
            .filter_map(|queue| queue.last().map(|(_, value)| (queue.id.load(Ordering::Relaxed), value)))
    }

    /// Returns the queues in use.
    fn active_queues(&self) -> &[ComWrapper<ParamValueQueue>] {
        let count = self.count.load(Ordering::Relaxed).min(self.queues.len());
        self.queues.get(..count).unwrap_or_default()
    }

    /// Returns the queue of a parameter, taking a free one when the parameter has none yet.
    fn queue_for(&self, id: ParamID) -> Option<&ParamValueQueue> {
        self.queue_index_for(id)
            .and_then(|index| self.queues.get(index))
            .map(|queue| &**queue)
    }

    /// Returns the index of a parameter's queue, taking a free one when the parameter has none yet.
    fn queue_index_for(&self, id: ParamID) -> Option<usize> {
        let count = self.count.load(Ordering::Relaxed);
        if let Some(index) = self
            .active_queues()
            .iter()
            .position(|queue| queue.id.load(Ordering::Relaxed) == id)
        {
            return Some(index);
        }
        let queue = self.queues.get(count)?;
        queue.reset(id);
        self.count.store(count + 1, Ordering::Relaxed);
        Some(count)
    }

    /// Returns the COM pointer of the queue at an index.
    fn queue_pointer(&self, index: usize) -> *mut IParamValueQueue {
        self.queues
            .get(index)
            .and_then(ComWrapper::as_com_ref::<IParamValueQueue>)
            .map_or(std::ptr::null_mut(), |queue| queue.as_ptr())
    }
}

impl IParameterChangesTrait for ParameterChanges {
    unsafe fn getParameterCount(&self) -> i32 {
        i32::try_from(self.active_queues().len()).unwrap_or(i32::MAX)
    }

    unsafe fn getParameterData(&self, index: i32) -> *mut IParamValueQueue {
        match usize::try_from(index) {
            Ok(index) if index < self.active_queues().len() => self.queue_pointer(index),
            _ => std::ptr::null_mut(),
        }
    }

    unsafe fn addParameterData(&self, id: *const ParamID, index: *mut i32) -> *mut IParamValueQueue {
        if id.is_null() {
            return std::ptr::null_mut();
        }
        // SAFETY: the plugin passes a readable parameter id, checked for null above.
        let Some(queue_index) = self.queue_index_for(unsafe { *id }) else {
            return std::ptr::null_mut();
        };
        if !index.is_null() {
            // SAFETY: the index slot is writable, checked for null above.
            unsafe { *index = i32::try_from(queue_index).unwrap_or(i32::MAX) };
        }
        self.queue_pointer(queue_index)
    }
}

#[cfg(test)]
mod test_parameter_changes {
    use super::*;

    #[test]
    fn set_replaces_the_earlier_value_of_a_parameter() {
        let changes = ParameterChanges::create();
        assert!(changes.is_empty());
        assert!(changes.set(5, 0.1));
        assert!(changes.set(7, 0.2));
        assert!(changes.set(5, 0.3));
        let values: Vec<(u32, f64)> = changes.last_values().collect();
        assert_eq!(values, vec![(5, 0.3), (7, 0.2)]);
        // SAFETY: the changes object is live for the call.
        assert_eq!(unsafe { changes.getParameterCount() }, 2);
    }

    #[test]
    fn plugin_side_points_are_appended_and_read_back() {
        let changes = ParameterChanges::create();
        let id: ParamID = 9;
        let mut index = -1;
        // SAFETY: every pointer refers to live data, and the returned queue lives as long as `changes`.
        unsafe {
            let queue = changes.addParameterData(&raw const id, &raw mut index);
            assert!(!queue.is_null());
            assert_eq!(index, 0);
            let queue = vst3::ComRef::from_raw(queue).expect("queue pointer");
            let mut point_index = -1;
            assert_eq!(queue.addPoint(10, 0.5, &raw mut point_index), kResultOk);
            assert_eq!(queue.addPoint(20, 0.75, &raw mut point_index), kResultOk);
            assert_eq!(point_index, 1);
            let mut offset = 0;
            let mut value = 0.0;
            assert_eq!(queue.getPoint(1, &raw mut offset, &raw mut value), kResultOk);
            assert_eq!((offset, value), (20, 0.75));
            assert_eq!(queue.getPoint(2, &raw mut offset, &raw mut value), kResultFalse);
        }
        assert_eq!(changes.last_values().collect::<Vec<_>>(), vec![(9, 0.75)]);
    }

    #[test]
    fn full_sets_refuse_new_parameters() {
        let changes = ParameterChanges::create();
        for id in 0..MAXIMUM_PARAMETERS {
            assert!(changes.set(u32::try_from(id).expect("small id"), 0.0));
        }
        assert!(!changes.set(u32::MAX, 1.0));
        assert!(changes.set(0, 1.0));
        changes.clear();
        assert!(changes.is_empty());
        // SAFETY: the changes object is live for the call.
        assert!(unsafe { changes.getParameterData(0) }.is_null());
    }

    #[test]
    fn queues_hold_a_limited_number_of_points() {
        let queue = ParamValueQueue::new();
        queue.reset(1);
        for offset in 0..MAXIMUM_POINTS {
            // SAFETY: no index slot is passed.
            let result =
                unsafe { queue.addPoint(i32::try_from(offset).expect("small offset"), 0.0, std::ptr::null_mut()) };
            assert_eq!(result, kResultOk);
        }
        // SAFETY: no index slot is passed.
        assert_eq!(unsafe { queue.addPoint(99, 0.0, std::ptr::null_mut()) }, kResultFalse);
    }
}
