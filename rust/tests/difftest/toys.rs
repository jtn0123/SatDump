//! Rust versions of the framework's own twins (`tests/diff/twins_selftest.cpp`). They stand
//! in for the decoders still to be ported: fed in chunks, giving records, with floats, and
//! integers made from floats.

use crate::oracle::Case;
use crate::outcome::{Outcome, Record, Value};

/// The toy stream format: records of [tag][length][length bytes], read one byte at a time with
/// the state kept from one chunk to the next. With `boundary_bug`, a header cut after its
/// first byte is forgotten (the self-test's deliberate fault).
#[derive(Default)]
struct Splitter {
    pos: u64,
    at: u64,
    tag: u8,
    length: usize,
    have: u8,
    payload: Vec<u8>,
    boundary_bug: bool,
}

impl Splitter {
    fn feed(&mut self, chunk: &[u8], out: &mut Vec<Record>) {
        if self.boundary_bug && self.have == 1 {
            self.have = 0;
        }
        for &b in chunk {
            match self.have {
                0 => (self.at, self.tag, self.have) = (self.pos, b, 1),
                1 => {
                    (self.length, self.have) = (usize::from(b), 2);
                    self.payload.clear();
                }
                _ => self.payload.push(b),
            }
            if self.have == 2 && self.payload.len() == self.length {
                out.push(vec![
                    ("at".to_string(), Value::Int(i64::try_from(self.at).expect("a position fits in i64"))),
                    ("tag".to_string(), Value::Int(i64::from(self.tag))),
                    ("payload".to_string(), Value::Bytes(std::mem::take(&mut self.payload))),
                ]);
                self.have = 0;
            }
            self.pos += 1;
        }
    }

    /// Bytes read but not yet part of a record
    fn left(&self) -> i64 {
        let payload = i64::try_from(self.payload.len()).expect("a payload of at most 255 bytes");
        if self.have == 0 { 0 } else { i64::from(self.have) + payload }
    }
}

fn split(c: &Case, boundary_bug: bool) -> Outcome {
    let mut s = Splitter { boundary_bug, ..Splitter::default() };
    let mut records = Vec::new();
    let mut pos = 0;
    for n in c.chunks() {
        s.feed(&c.input[pos..pos + n], &mut records);
        pos += n;
    }
    Outcome { status: s.left(), ..Outcome::new(0, Vec::new(), records) }
}

/// selftest_splitter
pub fn splitter(c: &Case) -> Outcome {
    split(c, false)
}

/// The splitter with the chunk-boundary fault, to show the oracles carry the chunking
pub fn splitter_with_boundary_bug(c: &Case) -> Outcome {
    split(c, true)
}

/// selftest_lowpass: y = 0.3 x + 0.7 y per byte (x = byte / 255), one record per chunk with
/// the outputs as 32-bit floats. Rust never fuses a multiply-add on its own, as the C++ built
/// with -ffp-contract=off (the float rule in `tests/diff/diff.h`).
pub fn lowpass(c: &Case) -> Outcome {
    let mut y = 0.0f32;
    let mut pos = 0;
    let mut records = Vec::new();
    for (k, n) in c.chunks().into_iter().enumerate() {
        let mut out = Vec::with_capacity(n);
        for &b in &c.input[pos..pos + n] {
            let x = f32::from(b) / 255.0;
            y = 0.3f32 * x + 0.7f32 * y;
            out.push(y);
        }
        pos += n;
        let chunk = i64::try_from(k).expect("a chunk number fits in i64");
        records.push(vec![("chunk".to_string(), Value::Int(chunk)), ("y".to_string(), Value::floats(&out))]);
    }
    Outcome::new(0, Vec::new(), records)
}

/// The pixels between a good one (`top`) and the next good one `range` lines below, as the
/// Meteor LRPT reader repairs missing lines; `pixel` is how one is worked out. Each gap is 3
/// bytes (top, bottom, range - 2 modulo 49) and gives one record.
fn fill_gaps(c: &Case, pixel: fn(f32, f32, f32) -> u8) -> Outcome {
    let mut records = Vec::new();
    for gap in c.input.chunks_exact(3) {
        let (top, bottom) = (f32::from(gap[0]), f32::from(gap[1]));
        let range = 2 + usize::from(gap[2] % 49);
        let pixels = (1..range).map(|fix_y| pixel(fix_y as f32 / range as f32, top, bottom)).collect();
        let range = Value::Int(i64::try_from(range).expect("at most 50"));
        records.push(vec![("range".to_string(), range), ("pixels".to_string(), Value::Bytes(pixels))]);
    }
    Outcome::new(0, Vec::new(), records)
}

/// selftest_gapfill: (1 - p) top + p bottom, made an `int` and then 8 bits, as the C++
/// (`Image::set(size_t, int)`); Rust's `as` truncates toward zero as the C++ conversion does
pub fn gapfill(c: &Case) -> Outcome {
    fill_gaps(c, |percent, top, bottom| ((1.0 - percent) * top + percent * bottom) as i32 as u8)
}

/// The same with one fused multiply-add, as GCC builds the C++ with the project's flags: the
/// self-test's deliberate fault
pub fn gapfill_fused(c: &Case) -> Outcome {
    fill_gaps(c, |percent, top, bottom| (1.0 - percent).mul_add(top, percent * bottom) as i32 as u8)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn case(input: &[u8], chunks: &[usize]) -> Case {
        Case { id: "t".to_string(), chunks: chunks.to_vec(), params: Vec::new(), input: input.to_vec() }
    }

    #[test]
    fn the_splitter_reads_records_across_chunks() {
        // Two records, then a header with no payload yet
        let input = [7, 2, 0xAA, 0xBB, 9, 0, 5, 3, 1];
        let whole = splitter(&case(&input, &[]));
        assert_eq!(whole.records.len(), 2);
        assert_eq!(whole.records[1][0].1, Value::Int(4));
        assert_eq!(whole.status, 3);
        assert_eq!(splitter(&case(&input, &[1; 9])), whole);
        // The fault shows only when a chunk ends right after a tag byte
        assert_eq!(splitter_with_boundary_bug(&case(&input, &[3, 6])), whole);
        assert_ne!(splitter_with_boundary_bug(&case(&input, &[1, 8])), whole);
    }

    #[test]
    fn gaps_are_filled_between_the_two_pixels() {
        // 10 above, 40 below, 4 lines apart (range byte 2): 17.5, 25 and 32.5, truncated
        let o = gapfill(&case(&[10, 40, 2, 99], &[]));
        assert_eq!(o.records.len(), 1);
        assert_eq!(o.records[0][0].1, Value::Int(4));
        assert_eq!(o.records[0][1].1, Value::Bytes(vec![17, 25, 32]));
        assert_eq!(gapfill_fused(&case(&[10, 40, 2], &[])), o);
    }

    #[test]
    fn the_lowpass_carries_its_state_across_chunks() {
        let input = [255u8, 0, 128, 64];
        let whole = lowpass(&case(&input, &[]));
        let split = lowpass(&case(&input, &[1, 0, 3]));
        assert_eq!(split.records.len(), 3);
        let floats = |o: &Outcome| -> Vec<u32> {
            o.records.iter().flat_map(|r| if let Value::F32(v) = &r[1].1 { v.clone() } else { Vec::new() }).collect()
        };
        assert_eq!(floats(&whole), floats(&split));
        assert_eq!(floats(&whole)[0], 0.3f32.to_bits());
    }
}
