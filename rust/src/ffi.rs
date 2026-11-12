//! The C functions SatDump's C++ calls (through `rust/bridge/rust_bridge.cpp`). Rules for
//! every one of them:
//!
//! 1. No panic reaches C++: the body runs inside [`guard`], which turns a panic into
//!    [`STATUS_PANIC`]. The C++ side then logs it once and uses its own code.
//! 2. Arguments are checked before anything is changed, so on any error the caller's buffer
//!    is exactly as it was and the C++ code can do the work instead.
//! 3. A negative length is an error; a null pointer is accepted only with length 0.
//!
//! The status codes are repeated in `rust/bridge/rust_bridge.h`.

use std::panic::{AssertUnwindSafe, catch_unwind};

/// The work was done
pub const STATUS_OK: i32 = 0;
/// Bad pointer or length; nothing was changed
pub const STATUS_BAD_ARGUMENT: i32 = 1;
/// The Rust code panicked; nothing was changed
pub const STATUS_PANIC: i32 = 2;

/// Run `body`, turning a panic into [`STATUS_PANIC`] (the other `ffi_*` files use it too)
pub(crate) fn guard(body: impl FnOnce() -> i32) -> i32 {
    catch_unwind(AssertUnwindSafe(body)).unwrap_or(STATUS_PANIC)
}

/// A C++ (pointer, length) pair as a slice, or `None` when they can't be one
///
/// # Safety
/// When `length` > 0 and `data` is not null, `data` must point to `length` writable, aligned
/// values of `T` that nothing else touches until the slice is dropped.
unsafe fn buffer<'a, T>(data: *mut T, length: i32) -> Option<&'a mut [T]> {
    let len = usize::try_from(length).ok()?;
    if len == 0 {
        return Some(&mut []);
    }
    if data.is_null() {
        return None;
    }
    // SAFETY: data is not null and the caller guarantees len writable, aligned, unshared values
    Some(unsafe { std::slice::from_raw_parts_mut(data, len) })
}

/// Runs `work` on the C++ buffer inside [`guard`], or refuses bad arguments
///
/// # Safety
/// As [`buffer`].
unsafe fn in_place<T>(data: *mut T, length: i32, work: impl FnOnce(&mut [T])) -> i32 {
    guard(|| {
        // SAFETY: passed on from this function's own contract
        match unsafe { buffer(data, length) } {
            Some(buf) => {
                work(buf);
                STATUS_OK
            }
            None => STATUS_BAD_ARGUMENT,
        }
    })
}

/// CCSDS derandomiser, in place (the twin of C++ `derand_ccsds_cpp`)
///
/// # Safety
/// `data` must point to `length` writable bytes (see [`buffer`]).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn satdump_rs_derand_ccsds(data: *mut u8, length: i32) -> i32 {
    // SAFETY: passed on from this function's own contract
    unsafe { in_place(data, length, crate::derand::derandomize) }
}

/// CCSDS derandomiser on soft symbols, in place (the twin of C++ `derand_ccsds_soft`; not used
/// by SatDump yet, only compared with it by the differential tests in `tests/diff`)
///
/// # Safety
/// `data` must point to `length` writable bytes (see [`buffer`]).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn satdump_rs_derand_ccsds_soft(data: *mut i8, length: i32) -> i32 {
    // SAFETY: passed on from this function's own contract
    unsafe { in_place(data, length, crate::derand::derandomize_soft) }
}

/// CCSDS derandomiser on one bit per byte, in place (the twin of C++ `derand_ccsds_bits`; not
/// used by SatDump yet, only compared with it by the differential tests in `tests/diff`)
///
/// # Safety
/// `data` must point to `length` writable bytes (see [`buffer`]).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn satdump_rs_derand_ccsds_bits(data: *mut u8, length: i32) -> i32 {
    // SAFETY: passed on from this function's own contract
    unsafe { in_place(data, length, crate::derand::derandomize_bits) }
}

/// Panics on purpose inside [`guard`] and returns the status; the C++ tests use it to prove
/// that a Rust panic comes back as an error code instead of stopping SatDump
#[unsafe(no_mangle)]
pub extern "C" fn satdump_rs_panic_check() -> i32 {
    guard(|| panic!("satdump_rs_panic_check: a deliberate panic (this is a test)"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_panic_becomes_a_status() {
        assert_eq!(satdump_rs_panic_check(), STATUS_PANIC);
        assert_eq!(guard(|| STATUS_OK), STATUS_OK);
    }

    #[test]
    fn bad_arguments_change_nothing() {
        let mut data = [1u8, 2, 3];
        // SAFETY: the pointers are either null or to `data`, with lengths it covers
        unsafe {
            assert_eq!(satdump_rs_derand_ccsds(std::ptr::null_mut(), 5), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_derand_ccsds(data.as_mut_ptr(), -1), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_derand_ccsds(std::ptr::null_mut(), 0), STATUS_OK);
            assert_eq!(satdump_rs_derand_ccsds_soft(std::ptr::null_mut(), 5), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_derand_ccsds_soft(data.as_mut_ptr().cast(), -1), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_derand_ccsds_soft(std::ptr::null_mut(), 0), STATUS_OK);
            assert_eq!(satdump_rs_derand_ccsds_bits(std::ptr::null_mut(), 5), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_derand_ccsds_bits(data.as_mut_ptr(), i32::MIN), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_derand_ccsds_bits(std::ptr::null_mut(), 0), STATUS_OK);
        }
        assert_eq!(data, [1, 2, 3]);
    }

    #[test]
    fn derandomises_in_place() {
        let mut data = [0u8; 4];
        // SAFETY: data has 4 writable bytes
        assert_eq!(unsafe { satdump_rs_derand_ccsds(data.as_mut_ptr(), 4) }, STATUS_OK);
        assert_eq!(data, [0xFF, 0x48, 0x0E, 0xC0]);
        let mut soft = [5i8; 10];
        // SAFETY: soft has 10 writable bytes
        assert_eq!(unsafe { satdump_rs_derand_ccsds_soft(soft.as_mut_ptr(), 10) }, STATUS_OK);
        assert_eq!(soft, [!5, !5, !5, !5, !5, !5, !5, !5, 5, !5]); // 0xFF, then 0x48 = 0100 1000
        let mut bits = [3u8; 10];
        // SAFETY: bits has 10 writable bytes
        assert_eq!(unsafe { satdump_rs_derand_ccsds_bits(bits.as_mut_ptr(), 10) }, STATUS_OK);
        assert_eq!(bits, [0, 0, 0, 0, 0, 0, 0, 0, 3, 0]);
    }
}
