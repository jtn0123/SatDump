//! The C functions of the Meteor LRPT segment decoder ([`crate::lrpt`]), called by
//! `rust/bridge/rust_lrpt.cpp`. They follow the rules of [`crate::ffi`]: a panic comes back
//! as [`STATUS_PANIC`](crate::ffi::STATUS_PANIC), arguments are checked first, and on any error the caller's output is
//! not touched. The decoder keeps no state from one packet to the next, so after a failure the
//! C++ decoder simply decodes the same packet.

use crate::ffi::{STATUS_BAD_ARGUMENT, STATUS_OK, guard};
use crate::lrpt::{self, LINES, WIDTH};
use std::cell::Cell;

/// One decoded segment as C++ sees it (`satdump::rust::LrptSegment` in
/// `rust/bridge/rust_lrpt.h`, which checks the same size and places); flags are 0 or 1
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct LrptSegment {
    /// The time in seconds (-1: none)
    pub timestamp: f64,
    /// Milliseconds of the day
    pub ms_time: u32,
    /// Day count
    pub day_time: u16,
    /// Microseconds of the millisecond
    pub us_time: u16,
    /// Quality factor marker
    pub qfm: u16,
    /// Number of the first block in the line
    pub mcun: u8,
    /// Quantisation table number
    pub qt: u8,
    /// DC Huffman table number
    pub dc: u8,
    /// AC Huffman table number
    pub ac: u8,
    /// Quality factor
    pub qf: u8,
    /// The segment is valid
    pub valid: u8,
    /// The segment is partial
    pub partial: u8,
    /// The pixels, 8 lines of 112
    pub lines: [[u8; WIDTH]; LINES],
}

/// The size C++ checks too (`static_assert` in rust_lrpt.cpp)
pub const LRPT_SEGMENT_SIZE: usize = 928;
const _: () = assert!(std::mem::size_of::<LrptSegment>() == LRPT_SEGMENT_SIZE);
const _: () = assert!(std::mem::offset_of!(LrptSegment, qf) == 22 && std::mem::offset_of!(LrptSegment, lines) == 25);

impl From<&lrpt::Segment> for LrptSegment {
    fn from(s: &lrpt::Segment) -> LrptSegment {
        LrptSegment {
            timestamp: s.timestamp,
            ms_time: s.ms_time,
            day_time: s.day_time,
            us_time: s.us_time,
            qfm: s.qfm,
            mcun: s.mcun,
            qt: s.qt,
            dc: s.dc,
            ac: s.ac,
            qf: s.qf,
            valid: u8::from(s.valid),
            partial: u8::from(s.partial),
            lines: s.lines,
        }
    }
}

thread_local! {
    /// The block at which this thread's next segment decode panics (-1: none); see
    /// [`satdump_rs_lrpt_fail_next_segment`]
    static FAIL_AT_BLOCK: Cell<i32> = const { Cell::new(-1) };
}

/// A C++ (pointer, length) pair as a slice to read, or `None` when they can't be one (a
/// negative length, or a null pointer with a length)
///
/// # Safety
/// When `length` > 0 and `data` is not null, `data` must point to `length` readable bytes that
/// nothing changes until the slice is dropped.
unsafe fn input<'a>(data: *const u8, length: i32) -> Option<&'a [u8]> {
    let len = usize::try_from(length).ok()?;
    if len == 0 {
        return Some(&[]);
    }
    if data.is_null() {
        return None;
    }
    // SAFETY: data is not null and the caller guarantees len readable bytes
    Some(unsafe { std::slice::from_raw_parts(data, len) })
}

/// Decodes one MSU-MR packet payload into `*out`, as the C++ `Segment(data, length, partial,
/// m2x)` does (`partial` and `m2x` are 0 or not). On any status but [`STATUS_OK`], `*out` is
/// as it was.
///
/// # Safety
/// `data` must point to `length` readable bytes (or be null with length 0), and `out`, when
/// not null, to an `LrptSegment` that nothing else uses during the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn satdump_rs_lrpt_segment(
    data: *const u8,
    length: i32,
    partial: u8,
    m2x: u8,
    out: *mut LrptSegment,
) -> i32 {
    // A requested failure is taken by this call, whatever happens to it
    let fail_at = usize::try_from(FAIL_AT_BLOCK.replace(-1)).ok();
    guard(|| {
        if out.is_null() {
            return STATUS_BAD_ARGUMENT;
        }
        // SAFETY: passed on from this function's own contract
        let Some(bytes) = (unsafe { input(data, length) }) else {
            return STATUS_BAD_ARGUMENT;
        };
        let s = lrpt::decode_or_fail(bytes, partial != 0, m2x != 0, fail_at);
        // SAFETY: out is not null and the caller guarantees it is an LrptSegment of its own
        unsafe { out.write(LrptSegment::from(&s)) };
        STATUS_OK
    })
}

/// The quantisation table for quality factor `qf` (0 to 255) into `table[0..64]`, as the C++
/// `GetQuantizationTable((float)qf)`; for the differential tests (SatDump uses it only through
/// [`satdump_rs_lrpt_segment`])
///
/// # Safety
/// `table`, when not null, must point to 64 writable `i64` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn satdump_rs_lrpt_quant_table(qf: i32, table: *mut i64) -> i32 {
    guard(|| {
        let Ok(qf) = u8::try_from(qf) else {
            return STATUS_BAD_ARGUMENT;
        };
        if table.is_null() {
            return STATUS_BAD_ARGUMENT;
        }
        let t = lrpt::quantization_table(f32::from(qf));
        // SAFETY: table is not null and the caller guarantees 64 writable values
        unsafe { table.cast::<[i64; 64]>().write_unaligned(t) };
        STATUS_OK
    })
}

/// For tests only: the next call of [`satdump_rs_lrpt_segment`] on this thread panics when it
/// reaches block `block` (0 to 13) of its packet, after it has decoded the blocks before it; a
/// negative `block` takes the request back. The panic comes back as [`STATUS_PANIC`](crate::ffi::STATUS_PANIC) as any
/// other.
#[unsafe(no_mangle)]
pub extern "C" fn satdump_rs_lrpt_fail_next_segment(block: i32) {
    FAIL_AT_BLOCK.set(block.max(-1));
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::STATUS_PANIC;

    fn blank() -> LrptSegment {
        LrptSegment {
            timestamp: 7.0,
            ms_time: 0xA5A5_A5A5,
            day_time: 0xA5A5,
            us_time: 0xA5A5,
            qfm: 0xA5A5,
            mcun: 0xA5,
            qt: 0xA5,
            dc: 0xA5,
            ac: 0xA5,
            qf: 0xA5,
            valid: 0xA5,
            partial: 0xA5,
            lines: [[0xA5; WIDTH]; LINES],
        }
    }

    fn same(a: &LrptSegment, b: &LrptSegment) -> bool {
        format!("{a:?}") == format!("{b:?}")
    }

    #[test]
    fn bad_arguments_change_nothing() {
        let data = [0u8; 20];
        let mut out = blank();
        // SAFETY: the pointers are null or to `data` and `out`, with lengths they cover
        unsafe {
            assert_eq!(satdump_rs_lrpt_segment(data.as_ptr(), -1, 0, 0, &mut out), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_lrpt_segment(std::ptr::null(), 5, 0, 0, &mut out), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_lrpt_segment(data.as_ptr(), 20, 0, 0, std::ptr::null_mut()), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_lrpt_quant_table(256, [0i64; 64].as_mut_ptr()), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_lrpt_quant_table(-1, [0i64; 64].as_mut_ptr()), STATUS_BAD_ARGUMENT);
            assert_eq!(satdump_rs_lrpt_quant_table(80, std::ptr::null_mut()), STATUS_BAD_ARGUMENT);
        }
        assert!(same(&out, &blank()));
        // Null with length 0 is an empty packet: an invalid segment
        // SAFETY: out is an LrptSegment
        assert_eq!(unsafe { satdump_rs_lrpt_segment(std::ptr::null(), 0, 1, 0, &mut out) }, STATUS_OK);
        assert_eq!((out.valid, out.partial, out.timestamp), (0, 1, -1.0));
    }

    #[test]
    fn a_failure_part_way_leaves_the_output_as_it_was() {
        // 14 blocks of DC 0 and end of block: the decoder gets to block 5 before it fails
        let mut pkt = vec![0u8; 14];
        pkt.extend_from_slice(&[0x28, 0xA2, 0x8A, 0x28, 0xA2, 0x8A, 0x28, 0xA2, 0x8A, 0x28, 0xA0]);
        let mut out = blank();
        satdump_rs_lrpt_fail_next_segment(5);
        let len = i32::try_from(pkt.len()).expect("a short packet");
        // SAFETY: pkt has len readable bytes and out is an LrptSegment
        assert_eq!(unsafe { satdump_rs_lrpt_segment(pkt.as_ptr(), len, 0, 0, &mut out) }, STATUS_PANIC);
        assert!(same(&out, &blank()));
        // The request was used up: the next call decodes the packet
        // SAFETY: as above
        assert_eq!(unsafe { satdump_rs_lrpt_segment(pkt.as_ptr(), len, 0, 0, &mut out) }, STATUS_OK);
        assert_eq!((out.valid, out.partial), (1, 0));
        assert!(out.lines.iter().flatten().all(|&p| p == 128));
    }

    #[test]
    fn the_table_comes_out_whole() {
        let mut t = [0i64; 64];
        // SAFETY: t has 64 writable values
        assert_eq!(unsafe { satdump_rs_lrpt_quant_table(0, t.as_mut_ptr()) }, STATUS_OK);
        assert_eq!(t, lrpt::quantization_table(0.0));
    }
}
