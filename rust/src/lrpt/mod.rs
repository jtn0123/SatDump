//! The Meteor LRPT MSU-MR segment decoder (fork, M6.3): the port of the C++ `Segment` in
//! `plugins/meteor_support/meteor/instruments/msumr/lrpt/` (segment.cpp, huffman.cpp, idct.cpp,
//! tables.h). One CCSDS packet payload (APIDs 64 to 69) gives one segment: its 14-byte header,
//! whether it is valid or partial, and 8 lines of 112 pixels, 14 JPEG-style blocks of 8 x 8.
//!
//! It gives what the C++ gives for every input, its quirks included (the differential tests
//! in `tests/diff`, twins `lrpt_segment` and `lrpt_qtable`, compare them):
//! - a payload of 14 bytes or fewer is not valid and keeps the default header (time -1);
//! - a Huffman code that does not match, or whose value runs past the end, ends the segment:
//!   in the first block it is then not valid, in a later one partial, and the blocks before
//!   it keep their pixels;
//! - the AC coefficients that would reach a block's 64th place are not stored;
//! - the quantisation table is worked out in 32-bit floats ([`quantization_table`]), and the
//!   time stamp in 64-bit floats as `ccsds::parseCCSDSTimeFullRaw` does; its only product is
//!   exact, so a fused multiply-add in the C++ cannot change it.
//!
//! The image assembly (`lrpt_msumr_reader.cpp`) stays C++.

mod huffman;
mod idct;
mod tables;

pub use huffman::quantization_table;
use huffman::{Ac, Bits};

/// Lines in a segment
pub const LINES: usize = 8;
/// Pixels in each line of a segment (14 blocks of 8)
pub const WIDTH: usize = 112;
/// Bytes of header before the picture data
pub const HEADER: usize = 14;
/// Blocks of 8 x 8 pixels in a segment
const BLOCKS: usize = 14;
/// The day offset of Meteor-M N2-x time stamps (`parseCCSDSTimeFullRaw(data, 11322)`)
const M2X_DAYS: i32 = 11322;

/// One decoded segment: the fields of the C++ `Segment`, with the same names in lower case
#[derive(Clone, Debug, PartialEq)]
pub struct Segment {
    /// Day count of the time stamp
    pub day_time: u16,
    /// Milliseconds of the day
    pub ms_time: u32,
    /// Microseconds of the millisecond
    pub us_time: u16,
    /// The time in seconds (-1: none)
    pub timestamp: f64,
    /// Number of the first block (MCU) in the line
    pub mcun: u8,
    /// Quantisation table number
    pub qt: u8,
    /// DC Huffman table number
    pub dc: u8,
    /// AC Huffman table number
    pub ac: u8,
    /// Quality factor marker
    pub qfm: u16,
    /// Quality factor
    pub qf: u8,
    /// The header was read and the first block decoded
    pub valid: bool,
    /// The packet was cut short, or its picture data ended before the last block
    pub partial: bool,
    /// The pixels; blocks that were not decoded stay 0
    pub lines: [[u8; WIDTH]; LINES],
}

impl Segment {
    /// What the C++ constructor leaves before it reads a packet
    fn empty(partial: bool) -> Segment {
        Segment {
            day_time: 0,
            ms_time: 0,
            us_time: 0,
            timestamp: -1.0,
            mcun: 0,
            qt: 0,
            dc: 0,
            ac: 0,
            qfm: 0,
            qf: 0,
            valid: false,
            partial,
            lines: [[0; WIDTH]; LINES],
        }
    }

    /// The picture data ended (no code matched) in block `i`
    fn cut(&mut self, i: usize) {
        if i == 0 {
            self.valid = false;
        } else {
            self.partial = true;
        }
    }
}

/// The time stamp, as `ccsds::parseCCSDSTimeFullRaw(data, offset)` with its default scales
fn timestamp(days: u16, ms: u32, us: u16, m2x: bool) -> f64 {
    let offset = if m2x { M2X_DAYS } else { 0 };
    f64::from(offset + i32::from(days)) * 86400.0 + f64::from(ms) / 1000.0 + f64::from(us) / 1_000_000.0
}

/// Decodes one packet payload, as the C++ `Segment(data, length, partial, m2x)`: `partial`
/// says the packet was cut short, `m2x` that the time stamp counts days as Meteor-M N2-x does
pub fn decode(data: &[u8], partial: bool, m2x: bool) -> Segment {
    decode_or_fail(data, partial, m2x, None)
}

/// As [`decode`]; with `fail_at`, it panics when it reaches that block (the test-only fault
/// injection of `ffi_lrpt`, so the tests can show what a failure part-way through does)
pub(crate) fn decode_or_fail(data: &[u8], partial: bool, m2x: bool, fail_at: Option<usize>) -> Segment {
    let mut s = Segment::empty(partial);
    if data.len() <= HEADER {
        return s;
    }
    let h = &data[..HEADER];
    s.day_time = u16::from_be_bytes([h[0], h[1]]);
    s.ms_time = u32::from_be_bytes([h[2], h[3], h[4], h[5]]);
    s.us_time = u16::from_be_bytes([h[6], h[7]]);
    s.timestamp = timestamp(s.day_time, s.ms_time, s.us_time, m2x);
    s.mcun = h[8];
    s.qt = h[9];
    s.dc = (h[10] & 0xF0) >> 4;
    s.ac = h[10] & 0x0F;
    s.qfm = u16::from_be_bytes([h[11], h[12]]);
    s.qf = h[13];
    s.valid = true;
    decode_blocks(&mut s, &data[HEADER..], fail_at);
    s
}

/// The 14 blocks (`Segment::decode`)
fn decode_blocks(s: &mut Segment, data: &[u8], fail_at: Option<usize>) {
    let mut bits = Bits::new(data);
    let table = quantization_table(f32::from(s.qf));
    let mut last_dc = 0i64;
    for i in 0..BLOCKS {
        if fail_at == Some(i) {
            panic!("the LRPT segment decoder was told to fail at block {i} (a test)");
        }
        // The coefficients in the order they are sent; the C++ copies each run of zeros and
        // its value in, and the places after `index` are still 0, so only the value is written
        let mut block = [0i64; 64];
        let mut index = 1;

        let Some(dc) = bits.dc() else {
            s.cut(i);
            return;
        };
        last_dc += dc;
        block[0] = last_dc;

        let mut j = 0;
        while j < 63 {
            let ac = bits.ac();
            j += ac.count();
            match ac {
                Ac::NoMatch => {
                    s.cut(i);
                    return;
                }
                Ac::Run { zeros, value } if index + zeros + 1 < 64 => {
                    block[index + zeros] = value;
                    index += zeros + 1;
                }
                // The end of the block, or a run that would reach its 64th place
                Ac::Run { .. } | Ac::End => break,
            }
        }

        let mut pixels: [i64; 64] = std::array::from_fn(|x| block[tables::ZIGZAG[x]] * table[x]);
        idct::idct(&mut pixels);
        for (x, v) in pixels.iter().enumerate() {
            s.lines[x / 8][i * 8 + x % 8] = u8::try_from((v + 128).clamp(0, 255)).expect("clamped to 0..=255");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The good packet of `tests/unit/test_init_ub.cpp`: MCU 28, quality factor 80, and 14
    /// blocks that are each "DC difference 0" (00) and "end of block" (1010): flat grey
    fn grey() -> Vec<u8> {
        let mut pkt = vec![0u8; HEADER + 16];
        pkt[8] = 28;
        pkt[11] = 0xFF;
        pkt[12] = 0xF0;
        pkt[13] = 80;
        let mut bit = 0;
        for _ in 0..BLOCKS {
            for v in [false, false, true, false, true, false] {
                if v {
                    pkt[HEADER + bit / 8] |= 0x80 >> (bit % 8);
                }
                bit += 1;
            }
        }
        pkt
    }

    #[test]
    fn a_flat_packet_is_grey_and_valid() {
        let s = decode(&grey(), false, true);
        assert!(s.valid && !s.partial);
        assert_eq!((s.mcun, s.qf, s.qfm), (28, 80, 0xFFF0));
        assert!(s.lines.iter().flatten().all(|&p| p == 128));
        assert_eq!(s.timestamp, f64::from(M2X_DAYS) * 86400.0);
    }

    #[test]
    fn short_packets_are_not_valid_and_keep_the_defaults() {
        for n in 0..=HEADER {
            let s = decode(&grey()[..n], n % 2 == 0, true);
            assert_eq!(s, Segment::empty(n % 2 == 0), "{n} bytes");
        }
    }

    #[test]
    fn picture_data_that_ends_early_makes_a_partial_segment() {
        // Three whole blocks (18 bits) and a cut fourth: the first three are grey, the rest black
        let mut pkt = grey();
        pkt.truncate(HEADER + 2);
        pkt.push(0b1000_0000);
        let s = decode(&pkt, false, false);
        assert!(s.valid && s.partial);
        for row in &s.lines {
            assert!(row[..24].iter().all(|&p| p == 128) && row[24..].iter().all(|&p| p == 0));
        }
        // Not even the first block: not valid
        let mut pkt = grey();
        pkt.truncate(HEADER);
        pkt.push(0xFF);
        let s = decode(&pkt, false, false);
        assert!(!s.valid && !s.partial);
        assert_eq!(s.day_time, 0);
        assert_eq!(s.timestamp, 0.0);
    }

    #[test]
    fn the_time_stamp_is_worked_out_as_the_cpp_does() {
        let mut pkt = grey();
        pkt[..8].copy_from_slice(&[0x23, 0x78, 0x02, 0x93, 0x2E, 0x00, 0x01, 0xF4]);
        let s = decode(&pkt, false, false);
        assert_eq!((s.day_time, s.ms_time, s.us_time), (9080, 43_200_000, 500));
        assert_eq!(s.timestamp, 9080.0 * 86400.0 + 43_200.0 + 0.0005);
        let s = decode(&pkt, false, true);
        assert_eq!(s.timestamp, f64::from(9080 + M2X_DAYS) * 86400.0 + 43_200.0 + 0.0005);
    }

    #[test]
    #[should_panic(expected = "told to fail at block 3")]
    fn a_requested_failure_panics_part_way() {
        decode_or_fail(&grey(), false, false, Some(3));
    }
}
