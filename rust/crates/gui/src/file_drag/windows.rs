//! The OLE drag source that drags selected files out of the results table on Windows.
//!
//! This module exists only because gpui-pre 0.3.10 returns false from `can_start_external_drag` on Windows,
//! so the `external_drag_payload` of the result rows never starts a platform drag there.
//! Once gpui implements `start_external_drag` for its Windows window,
//! delete this module and the `cfg(windows)` trigger in `app/render.rs`,
//! and the `external_drag_payload` path covers Windows as well.
//!
//! The drag offers a `CF_HDROP` list of the files with only `DROPEFFECT_COPY`, so other applications always copy.
//! `DataObject` and `DropSource` are minimal COM objects with hand-written vtables.
//! `DoDragDrop` runs its modal loop on the UI thread, where gpui already called `OleInitialize`.

#![expect(
    unsafe_code,
    reason = "OLE drag source for the file list, which gpui does not offer on Windows"
)]

use std::ffi::c_void;
use std::os::windows::ffi::OsStrExt as _;
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::atomic::{AtomicU32, Ordering};

use windows_sys::Win32::Foundation::{
    DRAGDROP_S_CANCEL, DRAGDROP_S_DROP, DRAGDROP_S_USEDEFAULTCURSORS, DV_E_FORMATETC, E_NOINTERFACE, E_NOTIMPL,
    E_OUTOFMEMORY, E_POINTER, GlobalFree, HGLOBAL, OLE_E_ADVISENOTSUPPORTED, POINT, S_OK,
};
use windows_sys::Win32::System::Com::{DATADIR_GET, DVASPECT_CONTENT, FORMATETC, STGMEDIUM, TYMED_HGLOBAL};
use windows_sys::Win32::System::Memory::{GHND, GlobalAlloc, GlobalLock, GlobalUnlock};
use windows_sys::Win32::System::Ole::{CF_HDROP, DROPEFFECT, DROPEFFECT_COPY, DoDragDrop};
use windows_sys::Win32::System::SystemServices::{MK_LBUTTON, MODIFIERKEYS_FLAGS};
use windows_sys::Win32::UI::Input::KeyboardAndMouse::ReleaseCapture;
use windows_sys::Win32::UI::Shell::{DROPFILES, SHCreateStdEnumFmtEtc};
use windows_sys::core::{BOOL, GUID, HRESULT, IID_IUnknown};

/// Interface id of `IDataObject`.
const IID_IDATA_OBJECT: GUID = GUID::from_u128(0x0000_010e_0000_0000_c000_0000_0000_0046);

/// Interface id of `IDropSource`.
const IID_IDROP_SOURCE: GUID = GUID::from_u128(0x0000_0121_0000_0000_c000_0000_0000_0046);

/// The vtable all drop sources share.
static DROP_SOURCE_VTBL: DropSourceVtbl = DropSourceVtbl {
    query_interface: drop_source_query_interface,
    add_ref: drop_source_add_ref,
    release: drop_source_release,
    query_continue_drag: drop_source_query_continue_drag,
    give_feedback: drop_source_give_feedback,
};

/// The vtable all data objects share.
static DATA_OBJECT_VTBL: DataObjectVtbl = DataObjectVtbl {
    query_interface: data_object_query_interface,
    add_ref: data_object_add_ref,
    release: data_object_release,
    get_data: data_object_get_data,
    get_data_here: data_object_not_implemented_medium,
    query_get_data: data_object_query_get_data,
    get_canonical_format_etc: data_object_get_canonical_format_etc,
    set_data: data_object_set_data,
    enum_format_etc: data_object_enum_format_etc,
    d_advise: data_object_d_advise,
    d_unadvise: data_object_d_unadvise,
    enum_d_advise: data_object_enum_d_advise,
};

/// Vtable of `IDropSource`, in the order of its methods.
#[repr(C)]
struct DropSourceVtbl {
    query_interface: unsafe extern "system" fn(*mut DropSource, *const GUID, *mut *mut c_void) -> HRESULT,
    add_ref: unsafe extern "system" fn(*mut DropSource) -> u32,
    release: unsafe extern "system" fn(*mut DropSource) -> u32,
    query_continue_drag: unsafe extern "system" fn(*mut DropSource, BOOL, MODIFIERKEYS_FLAGS) -> HRESULT,
    give_feedback: unsafe extern "system" fn(*mut DropSource, DROPEFFECT) -> HRESULT,
}

/// The `IDropSource` of a file drag: ends it on Escape or when the left button is released.
#[repr(C)]
struct DropSource {
    vtbl: *const DropSourceVtbl,
    references: AtomicU32,
}

/// Vtable of `IDataObject`, in the order of its methods.
#[repr(C)]
struct DataObjectVtbl {
    query_interface: unsafe extern "system" fn(*mut DataObject, *const GUID, *mut *mut c_void) -> HRESULT,
    add_ref: unsafe extern "system" fn(*mut DataObject) -> u32,
    release: unsafe extern "system" fn(*mut DataObject) -> u32,
    get_data: unsafe extern "system" fn(*mut DataObject, *const FORMATETC, *mut STGMEDIUM) -> HRESULT,
    get_data_here: unsafe extern "system" fn(*mut DataObject, *const FORMATETC, *mut STGMEDIUM) -> HRESULT,
    query_get_data: unsafe extern "system" fn(*mut DataObject, *const FORMATETC) -> HRESULT,
    get_canonical_format_etc: unsafe extern "system" fn(*mut DataObject, *const FORMATETC, *mut FORMATETC) -> HRESULT,
    set_data: unsafe extern "system" fn(*mut DataObject, *const FORMATETC, *const STGMEDIUM, BOOL) -> HRESULT,
    enum_format_etc: unsafe extern "system" fn(*mut DataObject, u32, *mut *mut c_void) -> HRESULT,
    d_advise: unsafe extern "system" fn(*mut DataObject, *const FORMATETC, u32, *mut c_void, *mut u32) -> HRESULT,
    d_unadvise: unsafe extern "system" fn(*mut DataObject, u32) -> HRESULT,
    enum_d_advise: unsafe extern "system" fn(*mut DataObject, *mut *mut c_void) -> HRESULT,
}

/// The `IDataObject` of a file drag, which offers the files as `CF_HDROP` in global memory only.
#[repr(C)]
struct DataObject {
    /// Pointer to the shared `IDataObject` vtable.
    vtbl: *const DataObjectVtbl,
    /// Active COM reference count.
    references: AtomicU32,
    /// The dragged files.
    paths: Vec<PathBuf>,
}

/// Drags the files out of the window as copies, and returns true when another application took them.
///
/// Runs the modal OLE drag loop on the calling thread, which must be gpui's UI thread,
/// where `OleInitialize` has run, and returns once the drag ends.
/// Releases the mouse capture gpui took on mouse down first, because OLE needs the mouse events.
#[must_use]
pub fn start_file_drag(paths: &[PathBuf]) -> bool {
    if paths.is_empty() {
        return false;
    }
    let data = Box::into_raw(Box::new(DataObject {
        vtbl: &raw const DATA_OBJECT_VTBL,
        references: AtomicU32::new(1),
        paths: paths.to_vec(),
    }));
    let source = Box::into_raw(Box::new(DropSource {
        vtbl: &raw const DROP_SOURCE_VTBL,
        references: AtomicU32::new(1),
    }));
    let mut effect: DROPEFFECT = 0;
    // SAFETY: Both objects are live COM objects with one reference each, which this function owns.
    // DoDragDrop takes its own references for as long as it needs them, and the calls are on the UI thread.
    let result = unsafe {
        ReleaseCapture();
        let result = DoDragDrop(data.cast(), source.cast(), DROPEFFECT_COPY, &raw mut effect);
        data_object_release(data);
        drop_source_release(source);
        result
    };
    result == DRAGDROP_S_DROP && effect & DROPEFFECT_COPY != 0
}

/// Returns true when a requested format is the `CF_HDROP` in global memory that the data object offers.
const fn is_offered_format(format: &FORMATETC) -> bool {
    format.cfFormat == CF_HDROP && format.dwAspect == DVASPECT_CONTENT && format.tymed & TYMED_HGLOBAL as u32 != 0
}

/// Returns the one format the data object offers.
const fn offered_format() -> FORMATETC {
    FORMATETC {
        cfFormat: CF_HDROP,
        ptd: ptr::null_mut(),
        dwAspect: DVASPECT_CONTENT,
        lindex: -1,
        tymed: TYMED_HGLOBAL as u32,
    }
}

/// Returns the `CF_HDROP` bytes after the `DROPFILES` header:
/// each path in UTF-16 followed by a NUL, and one more NUL at the end.
fn file_list_utf16(paths: &[PathBuf]) -> Vec<u16> {
    let mut list: Vec<u16> = paths
        .iter()
        .flat_map(|path| Path::as_os_str(path).encode_wide().chain(std::iter::once(0)))
        .collect();
    list.push(0);
    list
}

/// Builds a `CF_HDROP` global memory block, a `DROPFILES` header with wide paths after it,
/// or returns null when the memory cannot be allocated.
/// The receiver of the data owns the block and frees it.
fn create_hdrop(paths: &[PathBuf]) -> HGLOBAL {
    let list = file_list_utf16(paths);
    let header_size = size_of::<DROPFILES>();
    let Some(list_size) = list.len().checked_mul(size_of::<u16>()) else {
        return ptr::null_mut();
    };
    let Some(total_size) = header_size.checked_add(list_size) else {
        return ptr::null_mut();
    };
    let Ok(files_offset) = u32::try_from(header_size) else {
        return ptr::null_mut();
    };
    let header = DROPFILES {
        pFiles: files_offset,
        pt: POINT { x: 0, y: 0 },
        fNC: 0,
        fWide: 1,
    };
    // SAFETY: The block is allocated with room for the header and the list, locked while it is written,
    // and both writes stay inside it. The header is packed, so it is written unaligned.
    unsafe {
        let memory = GlobalAlloc(GHND, total_size);
        if memory.is_null() {
            return ptr::null_mut();
        }
        let bytes = GlobalLock(memory).cast::<u8>();
        if bytes.is_null() {
            GlobalFree(memory);
            return ptr::null_mut();
        }
        ptr::write_unaligned(bytes.cast::<DROPFILES>(), header);
        ptr::copy_nonoverlapping(list.as_ptr().cast::<u8>(), bytes.add(header_size), list_size);
        GlobalUnlock(memory);
        memory
    }
}

/// Answers `QueryInterface` for an object that implements `IUnknown` and one other interface,
/// always clearing a non-null `object` out-pointer on failure as COM requires.
///
/// # Safety
///
/// `this` must point to a live COM object, and `riid` and `object` must be valid pointers or null.
unsafe fn query_interface(
    this: *mut c_void,
    riid: *const GUID,
    object: *mut *mut c_void,
    interface: &GUID,
    add_ref: impl FnOnce(),
) -> HRESULT {
    if object.is_null() {
        return E_POINTER;
    }
    // SAFETY: `object` is non-null and valid for writing.
    // COM requires clearing `*object` on failure.
    unsafe {
        *object = ptr::null_mut();
    }
    if riid.is_null() {
        return E_POINTER;
    }
    // SAFETY: The caller guarantees that both pointers are valid when they are not null.
    unsafe {
        let requested = &*riid;
        let matches = |guid: &GUID| {
            requested.data1 == guid.data1
                && requested.data2 == guid.data2
                && requested.data3 == guid.data3
                && requested.data4 == guid.data4
        };
        if matches(&IID_IUnknown) || matches(interface) {
            *object = this;
            add_ref();
            S_OK
        } else {
            E_NOINTERFACE
        }
    }
}

/// `IDropSource::QueryInterface`.
unsafe extern "system" fn drop_source_query_interface(
    this: *mut DropSource,
    riid: *const GUID,
    object: *mut *mut c_void,
) -> HRESULT {
    // SAFETY: OLE calls this with the live drop source and valid or null pointers.
    unsafe {
        query_interface(this.cast(), riid, object, &IID_IDROP_SOURCE, || {
            drop_source_add_ref(this);
        })
    }
}

/// `IDropSource::AddRef`.
unsafe extern "system" fn drop_source_add_ref(this: *mut DropSource) -> u32 {
    // SAFETY: OLE calls this with the live drop source.
    unsafe { (*this).references.fetch_add(1, Ordering::Relaxed) + 1 }
}

/// `IDropSource::Release`, which frees the drop source with its last reference.
unsafe extern "system" fn drop_source_release(this: *mut DropSource) -> u32 {
    // SAFETY: The drop source came from `Box::into_raw`, and is freed only once, with its last reference.
    unsafe {
        let remaining = (*this).references.fetch_sub(1, Ordering::AcqRel) - 1;
        if remaining == 0 {
            drop(Box::from_raw(this));
        }
        remaining
    }
}

/// `IDropSource::QueryContinueDrag`: cancels on Escape and drops when the left button is released.
const unsafe extern "system" fn drop_source_query_continue_drag(
    _this: *mut DropSource,
    escape_pressed: BOOL,
    key_state: MODIFIERKEYS_FLAGS,
) -> HRESULT {
    if escape_pressed != 0 {
        DRAGDROP_S_CANCEL
    } else if key_state & MK_LBUTTON == 0 {
        DRAGDROP_S_DROP
    } else {
        S_OK
    }
}

/// `IDropSource::GiveFeedback`, which keeps the standard drag cursors.
const unsafe extern "system" fn drop_source_give_feedback(_this: *mut DropSource, _effect: DROPEFFECT) -> HRESULT {
    DRAGDROP_S_USEDEFAULTCURSORS
}

/// `IDataObject::QueryInterface`.
unsafe extern "system" fn data_object_query_interface(
    this: *mut DataObject,
    riid: *const GUID,
    object: *mut *mut c_void,
) -> HRESULT {
    // SAFETY: OLE calls this with the live data object and valid or null pointers.
    unsafe {
        query_interface(this.cast(), riid, object, &IID_IDATA_OBJECT, || {
            data_object_add_ref(this);
        })
    }
}

/// `IDataObject::AddRef`.
unsafe extern "system" fn data_object_add_ref(this: *mut DataObject) -> u32 {
    // SAFETY: OLE calls this with the live data object.
    unsafe { (*this).references.fetch_add(1, Ordering::Relaxed) + 1 }
}

/// `IDataObject::Release`, which frees the data object with its last reference.
unsafe extern "system" fn data_object_release(this: *mut DataObject) -> u32 {
    // SAFETY: The data object came from `Box::into_raw`, and is freed only once, with its last reference.
    unsafe {
        let remaining = (*this).references.fetch_sub(1, Ordering::AcqRel) - 1;
        if remaining == 0 {
            drop(Box::from_raw(this));
        }
        remaining
    }
}

/// `IDataObject::GetData`, which hands the receiver a new `CF_HDROP` block that it then owns.
unsafe extern "system" fn data_object_get_data(
    this: *mut DataObject,
    format: *const FORMATETC,
    medium: *mut STGMEDIUM,
) -> HRESULT {
    if format.is_null() || medium.is_null() {
        return E_POINTER;
    }
    // SAFETY: OLE calls this with the live data object and valid format and medium pointers.
    unsafe {
        if !is_offered_format(&*format) {
            return DV_E_FORMATETC;
        }
        let memory = create_hdrop(&(*this).paths);
        if memory.is_null() {
            return E_OUTOFMEMORY;
        }
        let medium = &mut *medium;
        medium.tymed = TYMED_HGLOBAL as u32;
        medium.u.hGlobal = memory;
        medium.pUnkForRelease = ptr::null_mut();
    }
    S_OK
}

/// `IDataObject::GetDataHere`, which is not supported.
const unsafe extern "system" fn data_object_not_implemented_medium(
    _this: *mut DataObject,
    _format: *const FORMATETC,
    _medium: *mut STGMEDIUM,
) -> HRESULT {
    E_NOTIMPL
}

/// `IDataObject::QueryGetData`, which accepts only the offered `CF_HDROP` format.
const unsafe extern "system" fn data_object_query_get_data(
    _this: *mut DataObject,
    format: *const FORMATETC,
) -> HRESULT {
    if format.is_null() {
        return E_POINTER;
    }
    // SAFETY: OLE calls this with a valid format pointer.
    if unsafe { is_offered_format(&*format) } {
        S_OK
    } else {
        DV_E_FORMATETC
    }
}

/// `IDataObject::GetCanonicalFormatEtc`, which reports that the format is its own canonical form.
unsafe extern "system" fn data_object_get_canonical_format_etc(
    _this: *mut DataObject,
    _format: *const FORMATETC,
    output: *mut FORMATETC,
) -> HRESULT {
    if !output.is_null() {
        // SAFETY: OLE calls this with a valid output pointer when it is not null.
        unsafe { (*output).ptd = ptr::null_mut() };
    }
    E_NOTIMPL
}

/// `IDataObject::SetData`, which is not supported.
const unsafe extern "system" fn data_object_set_data(
    _this: *mut DataObject,
    _format: *const FORMATETC,
    _medium: *const STGMEDIUM,
    _release: BOOL,
) -> HRESULT {
    E_NOTIMPL
}

/// `IDataObject::EnumFormatEtc`, which lists the one offered format through the shell's standard enumerator.
unsafe extern "system" fn data_object_enum_format_etc(
    _this: *mut DataObject,
    direction: u32,
    enumerator: *mut *mut c_void,
) -> HRESULT {
    if enumerator.is_null() {
        return E_POINTER;
    }
    if i32::try_from(direction) != Ok(DATADIR_GET) {
        // SAFETY: OLE calls this with a valid output pointer.
        unsafe { *enumerator = ptr::null_mut() };
        return E_NOTIMPL;
    }
    let format = offered_format();
    // SAFETY: The format array has one element and the output pointer is valid.
    unsafe { SHCreateStdEnumFmtEtc(1, &raw const format, enumerator) }
}

/// `IDataObject::DAdvise`, which is not supported.
const unsafe extern "system" fn data_object_d_advise(
    _this: *mut DataObject,
    _format: *const FORMATETC,
    _flags: u32,
    _sink: *mut c_void,
    _connection: *mut u32,
) -> HRESULT {
    OLE_E_ADVISENOTSUPPORTED
}

/// `IDataObject::DUnadvise`, which is not supported.
const unsafe extern "system" fn data_object_d_unadvise(_this: *mut DataObject, _connection: u32) -> HRESULT {
    OLE_E_ADVISENOTSUPPORTED
}

/// `IDataObject::EnumDAdvise`, which is not supported.
const unsafe extern "system" fn data_object_enum_d_advise(
    _this: *mut DataObject,
    _enumerator: *mut *mut c_void,
) -> HRESULT {
    OLE_E_ADVISENOTSUPPORTED
}

#[cfg(test)]
mod test_file_list {
    use super::*;

    #[test]
    fn paths_are_nul_separated_with_a_final_nul() {
        let list = file_list_utf16(&[PathBuf::from(r"C:\a.wav"), PathBuf::from(r"D:\b.wav")]);
        let expected: Vec<u16> = "C:\\a.wav\0D:\\b.wav\0\0".encode_utf16().collect();
        assert_eq!(list, expected);
    }

    #[test]
    fn only_global_memory_hdrop_is_offered() {
        let mut format = offered_format();
        assert!(is_offered_format(&format));
        format.cfFormat = 1;
        assert!(!is_offered_format(&format));
    }
}
