//! UI-thread native child detachment independent of baseview's borrowed `Window` owner.
//!
//! Detachment only removes the child from the host. The bridge must keep its baseview window alive
//! until active native operations unwind, then destroy that window before releasing the lease.
//! No native procedure, callback, timer, or host-owned window is installed or replaced here.

use std::cell::Cell;
use std::collections::HashSet;
use std::ffi::c_void;

#[cfg(target_os = "macos")]
use objc2::{rc::Retained, runtime::AnyObject};

use super::EditorError;

/// A retained `NSView` on macOS, or a borrowed child `HWND` on Windows.
#[cfg(target_os = "macos")]
type NativeChild = Retained<AnyObject>;

/// A window handle whose baseview owner must remain alive through the last detach call.
#[cfg(target_os = "windows")]
type NativeChild = windows_sys::Win32::Foundation::HWND;

/// Direct children present before the synchronous Iced/baseview window creation.
/// Existing macOS children are retained until comparison so their identities cannot be recycled.
pub(super) struct ChildrenBefore {
    /// The host parent, retained during capture/comparison on macOS and borrowed on Windows.
    parent: NativeChild,
    /// Only immediate children, including unrelated host children that must remain untouched.
    children: Vec<NativeChild>,
}

/// A UI-only capability to detach exactly one editor child without borrowing its baseview window.
/// It neither destroys the window nor joins workers. The bridge owns those lifecycle operations.
/// Its macOS retain is released when the lease is dropped, after baseview destroys its handler.
pub(super) struct ChildLease {
    /// The one new direct child, retained on macOS and borrowed from the live window on Windows.
    child: NativeChild,
    /// The borrowed host HWND, used only to verify detachment while the host is still alive.
    #[cfg(target_os = "windows")]
    parent: NativeChild,
    /// Records the creator for an explicit error before any off-thread native operation.
    owner_thread: std::thread::ThreadId,
    /// Set before native calls because removing or hiding a child may reenter editor close.
    detached: Cell<bool>,
}

impl ChildrenBefore {
    /// Snapshots direct children on the parent's native UI thread immediately before window creation.
    /// This allocates and retains native objects, so it must never run in an audio callback.
    ///
    /// # Safety
    /// A non-null `parent` must be a live `NSView` on macOS or `HWND` on Windows.
    /// It must remain valid on its owning UI thread through `finish` and editor detachment.
    /// Between capture and finish,
    /// synchronous editor creation must be the only operation adding direct children to this parent.
    /// Unrelated children must not be destroyed and replaced.
    /// The resulting lease must only be used while the created baseview window is still alive.
    /// Destroy that window after active operations unwind, before dropping the lease.
    #[expect(
        unsafe_code,
        reason = "Validates the borrowed native parent and retains its NSView at the attachment boundary"
    )]
    pub(super) unsafe fn capture(parent: *mut c_void) -> Result<Self, EditorError> {
        // SAFETY: The caller guarantees the native type and lifetime. Validation rejects null and wrong-thread calls.
        let _validated = unsafe { super::native::NativeParent::new(parent)? };
        #[cfg(target_os = "macos")]
        // SAFETY: Validation established the main thread, and the caller supplies a live NSView.
        let parent = unsafe { Retained::retain(parent.cast::<AnyObject>()) }.ok_or(EditorError::InvalidParent)?;
        let children = native_children(&parent);
        Ok(Self { parent, children })
    }

    /// Identifies exactly one newly attached direct child without disturbing pre-existing children.
    /// Zero or multiple additions fail attachment rather than guessing which native object to remove.
    /// The bridge must drop the just-created baseview window itself if this comparison fails.
    pub(super) fn finish(self) -> Result<ChildLease, EditorError> {
        let before = self.children.iter().map(native_identity).collect();
        let child = only_added_child(&before, native_children(&self.parent))?;
        Ok(ChildLease {
            child,
            #[cfg(target_os = "windows")]
            parent: self.parent,
            owner_thread: std::thread::current().id(),
            detached: Cell::new(false),
        })
    }
}

impl ChildLease {
    /// Synchronously removes this editor child from its host, once, on its owning UI thread.
    /// The lease and baseview window must both remain alive through any reentrant native callbacks.
    /// This does not destroy an active native handler or consume a borrowed baseview window.
    /// On Windows, the bridge must reject removal during an active Iced callback or geometry flush first.
    /// Hiding or reparenting can synchronously deliver focus or DPI events to the child's native handler.
    /// Windows failures are reported and permit a later retry. No host child other than this one is touched.
    #[expect(
        unsafe_code,
        reason = "Detaches only the child captured under the attachment caller's native lifetime guarantee"
    )]
    pub(super) fn detach(&self) -> Result<(), EditorError> {
        if self.owner_thread != std::thread::current().id() {
            return Err(EditorError::WrongThread);
        }
        if self.detached.replace(true) {
            return Ok(());
        }

        #[cfg(target_os = "macos")]
        {
            // SAFETY: This is the retained NSView found among the validated parent's direct children.
            // Capture and the non-Send lease restrict use to the native UI thread.
            let (): () = unsafe { objc2::msg_send![&*self.child, removeFromSuperview] };
        }

        #[cfg(target_os = "windows")]
        {
            use windows_sys::Win32::UI::WindowsAndMessaging::{
                HWND_MESSAGE, IsChild, IsWindow, SW_HIDE, SetParent, ShowWindow,
            };

            // SAFETY: The bridge keeps the baseview owner alive through this call on its HWND's UI thread.
            // A native host may already have destroyed the child, in which case nothing remains to detach.
            if unsafe { IsWindow(self.child) } == 0 {
                return Ok(());
            }
            // SAFETY: The bridge admits this Win32 operation only outside active Iced/geometry callbacks.
            // Focus-loss notifications may synchronously enter the now-unborrowed native handler.
            unsafe { ShowWindow(self.child, SW_HIDE) };
            // SAFETY: Reparenting this child to the system's message-only root removes host ancestry
            // without creating a replacement window or modifying the host's procedure or styles.
            unsafe { SetParent(self.child, HWND_MESSAGE) };
            // SAFETY: The same UI thread owns the child and the bridge retains its baseview owner.
            // Check host ancestry directly because message-only roots need not be exposed by GetParent.
            if unsafe { IsChild(self.parent, self.child) } != 0 {
                self.detached.set(false);
                return Err(native_child_error(
                    "could not detach the editor child from its native host",
                ));
            }
        }

        Ok(())
    }
}

/// Compares native identities, rejecting ambiguity instead of selecting an unrelated host child.
fn only_added_child(before: &HashSet<usize>, after: Vec<NativeChild>) -> Result<NativeChild, EditorError> {
    let mut added = after
        .into_iter()
        .filter(|child| !before.contains(&native_identity(child)));
    let child = added
        .next()
        .ok_or_else(|| native_child_error("editor creation did not attach a new direct native child"))?;
    if added.next().is_some() {
        return Err(native_child_error(
            "editor creation attached more than one new direct native child",
        ));
    }
    Ok(child)
}

/// Preserves child discovery/detachment failures through the bridge's existing native error channel.
fn native_child_error(message: &'static str) -> EditorError {
    EditorError::Native(baseview::Error::from(baseview::HandlerError::from(
        std::io::Error::other(message),
    )))
}

/// Reads only immediate subviews and retains them while checking identities on the main thread.
#[cfg(target_os = "macos")]
#[expect(
    unsafe_code,
    reason = "The native parent was validated and retained during capture on the AppKit main thread"
)]
fn native_children(parent: &NativeChild) -> Vec<NativeChild> {
    // SAFETY: Capture guarantees this retained AnyObject is an NSView on the main thread.
    // NSView's subviews property returns an NSArray containing only NSView objects.
    let children: Retained<objc2_foundation::NSArray<AnyObject>> = unsafe { objc2::msg_send![&**parent, subviews] };
    children.to_vec()
}

/// Reads direct siblings only, never recursively enumerating unrelated host descendants.
#[cfg(target_os = "windows")]
#[expect(
    unsafe_code,
    reason = "The attachment caller keeps the validated host HWND alive on its owning UI thread"
)]
fn native_children(parent: &NativeChild) -> Vec<NativeChild> {
    use windows_sys::Win32::UI::WindowsAndMessaging::{GW_CHILD, GW_HWNDNEXT, GetWindow};

    let mut children = Vec::new();
    // SAFETY: The validated parent is alive on this UI thread, and GW_CHILD reads its first direct child.
    let mut child = unsafe { GetWindow(*parent, GW_CHILD) };
    while !child.is_null() {
        children.push(child);
        // SAFETY: GetWindow does not dispatch callbacks. Siblings remain live during this UI-thread snapshot.
        child = unsafe { GetWindow(child, GW_HWNDNEXT) };
    }
    children
}

/// Uses pointer identity instead of `NSView`'s overridable Objective-C equality operation.
#[cfg(target_os = "macos")]
fn native_identity(child: &NativeChild) -> usize {
    Retained::as_ptr(child) as usize
}

/// Identifies a borrowed HWND without claiming ownership or installing native callbacks.
#[cfg(target_os = "windows")]
fn native_identity(child: &NativeChild) -> usize {
    *child as usize
}

#[cfg(test)]
mod tests {
    use super::{ChildrenBefore, EditorError, NativeChild, native_identity, only_added_child};

    /// Supplies distinct retained native identities without constructing UI objects on a test worker.
    #[cfg(target_os = "macos")]
    fn test_child(_identity: usize) -> NativeChild {
        objc2::runtime::NSObject::new().into_super()
    }

    /// Supplies opaque comparison-only identities that are never passed to native Windows APIs.
    #[cfg(target_os = "windows")]
    fn test_child(identity: usize) -> NativeChild {
        std::ptr::without_provenance_mut(identity)
    }

    /// An existing foreign child never becomes the editor lease, regardless of sibling order.
    #[test]
    fn selects_only_the_new_direct_child() {
        let foreign = test_child(1);
        let editor = test_child(2);
        let expected = native_identity(&editor);
        let before = std::iter::once(native_identity(&foreign)).collect();
        let child = only_added_child(&before, vec![editor, foreign]).expect("one new child");
        assert_eq!(native_identity(&child), expected);
    }

    /// Attachment fails rather than leasing an existing child when no editor child appeared.
    #[test]
    fn rejects_a_snapshot_without_a_new_child() {
        let foreign = test_child(1);
        let before = std::iter::once(native_identity(&foreign)).collect();
        assert!(only_added_child(&before, vec![foreign]).is_err());
    }

    /// Multiple additions are ambiguous even if an unrelated child is also present.
    #[test]
    fn rejects_multiple_new_direct_children() {
        let foreign = test_child(1);
        let first = test_child(2);
        let second = test_child(3);
        let before = std::iter::once(native_identity(&foreign)).collect();
        assert!(only_added_child(&before, vec![first, foreign, second]).is_err());
    }

    /// Null attachment fails before native hierarchy access on either supported platform.
    #[test]
    #[expect(
        unsafe_code,
        reason = "A null parent is rejected before any native object API is called"
    )]
    fn null_parent_is_rejected_before_snapshot() {
        // SAFETY: Null is the explicitly rejected input, so no native object lifetime is required.
        let result = unsafe { ChildrenBefore::capture(std::ptr::null_mut()) };
        assert!(matches!(result, Err(EditorError::InvalidParent)));
    }
}
