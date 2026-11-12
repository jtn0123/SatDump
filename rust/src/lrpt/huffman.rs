//! Reading the Huffman-coded coefficients and the quantisation table, as the C++ `huffman.cpp`.
//!
//! The C++ turns the bytes into one `bool` per bit first; this reads the bits straight from
//! the bytes. Codes are looked for in table order, as the C++ does: a code longer than the
//! bits that are left is passed over, and a code whose value would run past the end ends the
//! search with no match.

use super::tables::{AC, DC, Q};

/// The bits of a packet's picture data, read from the first (highest) bit of the first byte
pub(super) struct Bits<'a> {
    data: &'a [u8],
    pos: usize,
    left: usize,
}

/// What [`Bits::ac`] found
#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) enum Ac {
    /// No code matched (the C++ `CFC`)
    NoMatch,
    /// The end of the block (`EOB`)
    End,
    /// `zeros` zero coefficients, then `value` (sixteen zeros are 15 zeros and a 0)
    Run { zeros: usize, value: i64 },
}

impl Ac {
    /// How many coefficients the C++ counts for it (the size of the list `FindAC` returns)
    pub(super) fn count(self) -> usize {
        match self {
            Ac::Run { zeros, .. } => zeros + 1,
            Ac::NoMatch | Ac::End => 1,
        }
    }
}

/// A coefficient's `len` bits as a number: those starting with a 0 bit are negative (JPEG),
/// as the C++ `getValue`
fn signed(raw: u32, len: usize) -> i64 {
    let v = i64::from(raw);
    if raw >> (len - 1) == 0 { v - ((1i64 << len) - 1) } else { v }
}

impl<'a> Bits<'a> {
    pub(super) fn new(data: &'a [u8]) -> Bits<'a> {
        Bits { data, pos: 0, left: data.len() * 8 }
    }

    /// `n` (at most 32) bits from bit `start` on, the first one highest; past the end they are
    /// 0, which no caller compares: each first checks that enough bits are left
    fn peek(&self, start: usize, n: usize) -> u32 {
        let first = start / 8;
        let mut w = 0u64;
        for k in 0..5 {
            w = (w << 8) | u64::from(self.data.get(first + k).copied().unwrap_or(0));
        }
        let shift = 40 - start % 8 - n;
        u32::try_from((w >> shift) & ((1u64 << n) - 1)).expect("at most 32 bits")
    }

    fn skip(&mut self, n: usize) {
        self.pos += n;
        self.left -= n;
    }

    /// The next DC coefficient (`FindDC`), or `None` when no code matches
    pub(super) fn dc(&mut self) -> Option<i64> {
        let window = self.peek(self.pos, 16);
        for m in &DC {
            if self.left < m.klen {
                continue;
            }
            if window >> (16 - m.klen) == m.code {
                if self.left < m.klen + m.len {
                    break;
                }
                let raw = self.peek(self.pos + m.klen, m.len);
                self.skip(m.klen + m.len);
                return Some(if m.len == 0 { 0 } else { signed(raw, m.len) });
            }
        }
        self.left = 0;
        None
    }

    /// The next AC coefficients (`FindAC`)
    pub(super) fn ac(&mut self) -> Ac {
        let window = self.peek(self.pos, 16);
        for m in &AC {
            if self.left < m.klen || window >> (16 - m.klen) != m.code {
                continue;
            }
            if m.clen == 0 && m.zlen == 0 {
                self.skip(m.klen);
                return Ac::End;
            }
            let mut value = 0;
            // Sixteen zeros have no value bits
            if !(m.zlen == 15 && m.clen == 0) {
                if self.left < m.klen + m.clen {
                    break;
                }
                value = signed(self.peek(self.pos + m.klen, m.clen), m.clen);
            }
            self.skip(m.klen + m.clen);
            return Ac::Run { zeros: m.zlen, value };
        }
        self.left = 0;
        Ac::NoMatch
    }
}

/// The quantisation table for quality factor `qf`, as the C++ `GetQuantizationTable((float)QF)`:
/// 32-bit floats, each entry rounded by adding 0.5 and cutting off the fraction, and at least 1.
///
/// When optimising, GCC on aarch64 fuses `qf / 100 * q + 0.5` into one multiply-add (one
/// rounding instead of two), which Rust never does. That changes the float for 154 of the
/// 256 x 64 entries but none of the integers (the test below checks all of them), so this
/// gives the C++ table whether the C++ was built with fused multiply-adds or without.
pub fn quantization_table(qf: f32) -> [i64; 64] {
    let qf = if (20.0..50.0).contains(&qf) { 5000.0 / qf } else { 200.0 - (2.0 * qf) };
    std::array::from_fn(|x| ((qf / 100.0 * Q[x] + 0.5) as i64).max(1))
}

#[cfg(test)]
mod tests {
    use super::super::tables::ZIGZAG;
    use super::*;

    #[test]
    fn a_fused_multiply_add_gives_the_same_table_for_every_quality_factor() {
        for qf in 0..=255u8 {
            let f = f32::from(qf);
            let scaled = if (20.0..50.0).contains(&f) { 5000.0 / f } else { 200.0 - (2.0 * f) };
            let fused: [i64; 64] = std::array::from_fn(|x| ((scaled / 100.0).mul_add(Q[x], 0.5) as i64).max(1));
            assert_eq!(quantization_table(f), fused, "quality factor {qf}");
        }
    }

    #[test]
    fn the_table_scales_as_jpeg_does() {
        // 80: 40 % of the table, rounded (as tests/golden/make_recordings.py works it out)
        let t = quantization_table(80.0);
        for (x, q) in Q.iter().enumerate() {
            let q = *q as i64;
            assert_eq!(t[x], ((4 * q + 5) / 10).max(1), "entry {x}");
        }
        // 100 and above: all 1; 0: twice the table; 20: 250 %
        assert_eq!(quantization_table(100.0), [1; 64]);
        assert_eq!(quantization_table(255.0), [1; 64]);
        assert_eq!(quantization_table(0.0)[0], 32);
        assert_eq!(quantization_table(20.0)[0], 40);
    }

    #[test]
    fn the_codes_are_prefix_free_and_each_ac_pair_is_there_once() {
        // So the order the tables are searched in cannot change a result; it is kept anyway
        let check = |codes: Vec<(u32, usize)>| {
            for (i, &(a, la)) in codes.iter().enumerate() {
                for (j, &(b, lb)) in codes.iter().enumerate() {
                    assert!(i == j || la > lb || b >> (lb - la) != a, "code {i} starts code {j}");
                }
            }
        };
        check(DC.iter().map(|m| (m.code, m.klen)).collect());
        check(AC.iter().map(|m| (m.code, m.klen)).collect());
        let mut pairs: Vec<_> = AC.iter().map(|m| (m.zlen, m.clen)).collect();
        pairs.sort_unstable();
        pairs.dedup();
        assert_eq!(pairs.len(), AC.len());
        let mut order = ZIGZAG;
        order.sort_unstable();
        assert!(order.iter().enumerate().all(|(i, &z)| i == z));
    }

    #[test]
    fn bits_are_read_from_the_top() {
        // 00 = DC 0; 010 1 = DC +1; 011 00 = DC -3; 1010 = end of block; one bit left over
        let data = [0b0001_0101, 0b1001_0100];
        let mut b = Bits::new(&data);
        assert_eq!(b.dc(), Some(0));
        assert_eq!(b.dc(), Some(1));
        assert_eq!(b.dc(), Some(-3));
        assert_eq!(b.ac(), Ac::End);
        assert_eq!(b.left, 1);
        // Too little left for any code: no match
        assert_eq!(b.dc(), None);
    }

    #[test]
    fn a_code_whose_value_runs_past_the_end_is_no_match() {
        // 111111110 (DC, 11 bits of value) with only 7 bits after it
        let data = [0xFF, 0x00];
        assert_eq!(Bits::new(&data).dc(), None);
        // 11111111 1 (AC zero run 15, size 10 needs 16 + 10 bits) with too few bits left: no match
        let data = [0xFF, 0xFE];
        assert_eq!(Bits::new(&data).ac(), Ac::NoMatch);
        // Sixteen zeros (11111111001) need no value bits
        let data = [0xFF, 0x20];
        let mut b = Bits::new(&data);
        assert_eq!(b.ac(), Ac::Run { zeros: 15, value: 0 });
        assert_eq!(b.left, 5);
    }
}
