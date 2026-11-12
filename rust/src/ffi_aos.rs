//! The C functions of the CCSDS AOS demuxer ([`crate::aos`]), called by
//! `rust/bridge/rust_aos.cpp`. They follow the rules in [`crate::ffi`]: no panic reaches C++,
//! arguments are checked before anything is changed, and on an error nothing is.
//!
//! The demuxer has state, so C++ holds it as an [`AosHandle`]. After a
//! [`crate::ffi::STATUS_PANIC`] from [`satdump_rs_aos_work`] the handle's state is not known;
//! it may then only be freed, and the C++ demuxer takes over from that CADU (a lost frame, as
//! far as the packets go).

use crate::aos::{Demuxer, Settings};
use crate::ffi::{STATUS_BAD_ARGUMENT, STATUS_OK, guard};

/// A Rust demuxer as C++ holds it
pub struct AosHandle {
    demuxer: Demuxer,
    /// For tests: the work call this many calls from now panics (0: none)
    panic_in: i32,
}

/// Makes a demuxer with the C++ constructor's settings. `*out` gets the handle (free it with
/// [`satdump_rs_aos_free`]) and `*cadu_bytes` the bytes of a CADU it reads. Settings it does
/// not take ([`Settings::new`]) give [`STATUS_BAD_ARGUMENT`], and `*out` is not changed.
///
/// # Safety
/// `out` and `cadu_bytes` must each be null or point to a writable value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn satdump_rs_aos_new(
    mpdu_data_size: i32,
    has_insert_zone: bool,
    insert_zone_size: i32,
    secondary_header_extends_pkt: bool,
    check_frame_counter: bool,
    strict: bool,
    out: *mut *mut AosHandle,
    cadu_bytes: *mut i32,
) -> i32 {
    guard(|| {
        if out.is_null() || cadu_bytes.is_null() {
            return STATUS_BAD_ARGUMENT;
        }
        let Some(s) = Settings::new(
            mpdu_data_size,
            has_insert_zone,
            insert_zone_size,
            secondary_header_extends_pkt,
            check_frame_counter,
            strict,
        ) else {
            return STATUS_BAD_ARGUMENT;
        };
        let Ok(bytes) = i32::try_from(s.cadu_bytes()) else {
            return STATUS_BAD_ARGUMENT;
        };
        let handle = Box::new(AosHandle { demuxer: Demuxer::new(s), panic_in: 0 });
        // SAFETY: both are not null, and the caller guarantees they point to writable values
        unsafe {
            *cadu_bytes = bytes;
            *out = Box::into_raw(handle);
        }
        STATUS_OK
    })
}

/// Frees a demuxer made by [`satdump_rs_aos_new`] (null is accepted and does nothing)
///
/// # Safety
/// `handle` must be null or a handle from [`satdump_rs_aos_new`] not freed before.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn satdump_rs_aos_free(handle: *mut AosHandle) -> i32 {
    guard(|| {
        if !handle.is_null() {
            // SAFETY: the caller guarantees it came from Box::into_raw and is freed only once
            drop(unsafe { Box::from_raw(handle) });
        }
        STATUS_OK
    })
}

/// Runs the demuxer on one CADU (from its sync marker; `length` bytes, at least the
/// `cadu_bytes` [`satdump_rs_aos_new`] gave) and puts in `*count` how many packets it finished;
/// read them with [`satdump_rs_aos_packet`]. A bad argument changes nothing.
///
/// # Safety
/// `handle` must be a live handle that nothing else uses during the call, `cadu` must point
/// to `length` readable bytes, and `count` must be null or point to a writable value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn satdump_rs_aos_work(handle: *mut AosHandle, cadu: *const u8, length: i32, count: *mut i32) -> i32 {
    guard(|| {
        // SAFETY: the caller guarantees a live handle used by nothing else during the call
        let Some(h) = (unsafe { handle.as_mut() }) else {
            return STATUS_BAD_ARGUMENT;
        };
        let Ok(len) = usize::try_from(length) else {
            return STATUS_BAD_ARGUMENT;
        };
        if cadu.is_null() || count.is_null() || len < h.demuxer.settings().cadu_bytes() {
            return STATUS_BAD_ARGUMENT;
        }
        // SAFETY: cadu is not null and the caller guarantees `length` readable bytes
        let cadu = unsafe { std::slice::from_raw_parts(cadu, len) };
        let finished = h.demuxer.work(cadu).len();
        if h.panic_in > 0 {
            h.panic_in -= 1;
            assert!(h.panic_in != 0, "satdump_rs_aos_work: a deliberate panic (this is a test)");
        }
        let Ok(finished) = i32::try_from(finished) else {
            return STATUS_BAD_ARGUMENT;
        };
        // SAFETY: count is not null and the caller guarantees it points to a writable value
        unsafe { *count = finished };
        STATUS_OK
    })
}

/// Packet `index` of those the last [`satdump_rs_aos_work`] call finished: its 6 header bytes
/// into `header`, and where its payload is and how long it is. The payload stays readable
/// until the next work call or [`satdump_rs_aos_free`].
///
/// # Safety
/// `handle` must be a live handle; `header` must be null or point to 6 writable bytes, and
/// `payload` and `length` each null or point to a writable value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn satdump_rs_aos_packet(
    handle: *const AosHandle,
    index: i32,
    header: *mut u8,
    payload: *mut *const u8,
    length: *mut i32,
) -> i32 {
    guard(|| {
        // SAFETY: the caller guarantees a live handle
        let Some(h) = (unsafe { handle.as_ref() }) else {
            return STATUS_BAD_ARGUMENT;
        };
        let Some(p) = usize::try_from(index).ok().and_then(|i| h.demuxer.packets().get(i)) else {
            return STATUS_BAD_ARGUMENT;
        };
        let Ok(n) = i32::try_from(p.payload.len()) else {
            return STATUS_BAD_ARGUMENT;
        };
        if header.is_null() || payload.is_null() || length.is_null() {
            return STATUS_BAD_ARGUMENT;
        }
        // SAFETY: none is null, and the caller guarantees 6 writable header bytes and
        // writable values behind the other two
        unsafe {
            std::ptr::copy_nonoverlapping(p.header.as_ptr(), header, 6);
            *payload = p.payload.as_ptr();
            *length = n;
        }
        STATUS_OK
    })
}

/// For tests: the `calls`-th work call from now (1: the next one) does its work and then
/// panics, so the C++ side's handling of a Rust failure can be checked. 0 cancels it.
///
/// # Safety
/// `handle` must be null or a live handle that nothing else uses during the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn satdump_rs_aos_panic_in(handle: *mut AosHandle, calls: i32) -> i32 {
    guard(|| {
        // SAFETY: the caller guarantees a live handle used by nothing else during the call
        match unsafe { handle.as_mut() } {
            Some(h) if calls >= 0 => {
                h.panic_in = calls;
                STATUS_OK
            }
            _ => STATUS_BAD_ARGUMENT,
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::STATUS_PANIC;
    use std::ptr::{null, null_mut};

    /// A demuxer, with an insert zone when `iz` is not 0
    fn new(mpdu: i32, iz: i32) -> (i32, *mut AosHandle, i32) {
        let (mut h, mut bytes) = (null_mut(), 0);
        // SAFETY: both point to local values
        let status = unsafe { satdump_rs_aos_new(mpdu, iz != 0, iz, false, true, false, &mut h, &mut bytes) };
        (status, h, bytes)
    }

    /// A MetOp-layout CADU whose packet zone holds a 10-byte packet, then an idle packet to
    /// the end of the zone
    fn cadu(counter: u8) -> Vec<u8> {
        let mut c = vec![0u8; 1024];
        c[8] = counter;
        c[14..20].copy_from_slice(&[0x08, 0x64, 0xC0, 0x00, 0x00, 0x03]);
        c[20..24].copy_from_slice(&[1, 2, 3, 4]);
        c[24..30].copy_from_slice(&[0x07, 0xFF, 0xC0, 0x00, 0x03, 0x61]); // 866 bytes: to the end of the zone
        c
    }

    #[test]
    fn a_demuxer_through_the_c_functions() {
        let (status, h, bytes) = new(882, 2);
        assert_eq!((status, bytes), (STATUS_OK, 896));
        let (mut count, mut header, mut payload, mut length) = (-1, [0u8; 6], null(), 0);
        // SAFETY: h is live, the CADU has 1024 bytes and the outputs are local values
        unsafe {
            for counter in 0..2 {
                assert_eq!(satdump_rs_aos_work(h, cadu(counter).as_ptr(), 1024, &mut count), STATUS_OK);
            }
            assert_eq!(count, 2);
            assert_eq!(satdump_rs_aos_packet(h, 0, header.as_mut_ptr(), &mut payload, &mut length), STATUS_OK);
            assert_eq!(header, [0x08, 0x64, 0xC0, 0x00, 0x00, 0x03]);
            assert_eq!(std::slice::from_raw_parts(payload, usize::try_from(length).unwrap()), [1, 2, 3, 4]);
            assert_eq!(satdump_rs_aos_packet(h, 1, header.as_mut_ptr(), &mut payload, &mut length), STATUS_OK);
            assert_eq!((header[0], header[1], length), (0x07, 0xFF, 866));
            assert_eq!(satdump_rs_aos_free(h), STATUS_OK);
        }
    }

    #[test]
    fn bad_arguments_change_nothing() {
        for (mpdu, iz) in [(5, 0), (65537, 0), (884, -1), (884, 65537)] {
            let (status, h, bytes) = new(mpdu, iz);
            assert_eq!((status, h, bytes), (STATUS_BAD_ARGUMENT, null_mut(), 0));
        }
        let (_, h, _) = new(882, 2);
        let c = cadu(0);
        let (mut count, mut header, mut payload, mut length) = (-1, [9u8; 6], null(), -1);
        // SAFETY: h is live, the CADU has 1024 bytes and the outputs are local values
        unsafe {
            assert_eq!(satdump_rs_aos_new(882, true, 2, false, false, false, null_mut(), &mut length), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_aos_work(null_mut(), c.as_ptr(), 1024, &mut count), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_aos_work(h, null(), 1024, &mut count), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_aos_work(h, c.as_ptr(), 895, &mut count), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_aos_work(h, c.as_ptr(), -1, &mut count), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_aos_work(h, c.as_ptr(), 1024, null_mut()), STATUS_BAD_ARGUMENT);
            assert_eq!(count, -1);
            assert_eq!(satdump_rs_aos_work(h, c.as_ptr(), 896, &mut count), STATUS_OK);
            assert_eq!(count, 2);
            for index in [-1, 2, i32::MAX] {
                assert_eq!(satdump_rs_aos_packet(h, index, header.as_mut_ptr(), &mut payload, &mut length), STATUS_BAD_ARGUMENT);
            }
            assert_eq!(satdump_rs_aos_packet(h, 0, null_mut(), &mut payload, &mut length), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_aos_packet(null(), 0, header.as_mut_ptr(), &mut payload, &mut length), STATUS_BAD_ARGUMENT);
            assert_eq!((header, payload, length), ([9; 6], null(), -1));
            assert_eq!(satdump_rs_aos_panic_in(h, -1), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_aos_panic_in(null_mut(), 1), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_aos_free(h), STATUS_OK);
            assert_eq!(satdump_rs_aos_free(null_mut()), STATUS_OK);
        }
    }

    #[test]
    fn the_test_panic_comes_back_as_a_status() {
        let (_, h, _) = new(882, 2);
        let mut count = -1;
        // SAFETY: h is live, the CADUs have 1024 bytes and count is a local value
        unsafe {
            assert_eq!(satdump_rs_aos_panic_in(h, 2), STATUS_OK);
            assert_eq!(satdump_rs_aos_work(h, cadu(0).as_ptr(), 1024, &mut count), STATUS_OK);
            count = -1;
            assert_eq!(satdump_rs_aos_work(h, cadu(1).as_ptr(), 1024, &mut count), STATUS_PANIC);
            assert_eq!(count, -1);
            assert_eq!(satdump_rs_aos_free(h), STATUS_OK);
        }
    }
}
