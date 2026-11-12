//! The case bytes of the differential tests, made the same way as `tests/diff/cases.cpp`
//! (change both together): SplitMix64, FNV-1a, the fill styles and the recorded mutations.

/// SplitMix64, as `satdump::diff::Rng`
pub struct Rng(u64);

impl Rng {
    pub fn new(seed: u64) -> Self {
        Rng(seed)
    }

    /// The generator for case `index` of a run with `seed`
    pub fn for_case(seed: u64, index: u64) -> Self {
        Rng(seed ^ index.wrapping_mul(0xD1B5_4A32_D192_ED03))
    }

    pub fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
}

pub const FNV_OFFSET: u64 = 0xCBF2_9CE4_8422_2325;

pub fn fnv1a(data: &[u8], mut h: u64) -> u64 {
    for &b in data {
        h ^= u64::from(b);
        h = h.wrapping_mul(0x0100_0000_01B3);
    }
    h
}

/// The bytes of a generated case: `satdump::diff::fill`. Styles: 0 random, 1 all 0x00,
/// 2 all 0xFF, 3 bits (0 or 1), 4 extremes, 5 sparse.
pub fn fill(seed: u64, index: u64, length: usize, style: u64) -> Result<Vec<u8>, String> {
    const EXTREMES: [u8; 6] = [0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF];
    match style {
        0 | 3..=5 => {}
        1 => return Ok(vec![0x00; length]),
        2 => return Ok(vec![0xFF; length]),
        _ => return Err(format!("unknown fill style {style}")),
    }
    let mut rng = Rng::for_case(seed, index);
    let mut out = Vec::with_capacity(length);
    while out.len() < length {
        // Eight bytes from each number, lowest first
        for b in rng.next_u64().to_le_bytes() {
            if out.len() == length {
                break;
            }
            out.push(match style {
                3 => b & 1,
                4 => EXTREMES[usize::from(b % 6)],
                5 if b & 0xF0 == 0 => b,
                5 => 0,
                _ => b,
            });
        }
    }
    Ok(out)
}

/// One recorded change to a case's bytes (`satdump::diff::Mutation`)
#[derive(Clone, Debug, PartialEq)]
pub struct Mutation {
    pub op: String,
    pub a: u64,
    pub b: u64,
    pub c: u64,
}

/// A position known to be inside a buffer of `usize` bytes
fn at(v: u64) -> usize {
    usize::try_from(v).expect("a position inside the data fits in usize")
}

/// Changes `d` as `satdump::diff::apply` does, or says why the mutation does not fit
pub fn apply(d: &mut Vec<u8>, m: &Mutation) -> Result<(), String> {
    let n = d.len() as u64;
    // A range of b bytes at a must lie inside the data
    let inside = |a: u64, b: u64| b <= n && a <= n - b;
    match m.op.as_str() {
        "flip" if m.a / 8 < n => d[at(m.a / 8)] ^= 0x80u8 >> (m.a % 8),
        "set" if m.a < n && m.b < 256 => d[at(m.a)] = u8::try_from(m.b).map_err(|e| e.to_string())?,
        "truncate" if m.a <= n => d.truncate(at(m.a)),
        "copy" if inside(m.a, m.b) && inside(m.c, m.b) => d.copy_within(at(m.a)..at(m.a + m.b), at(m.c)),
        "cut" if inside(m.a, m.b) => {
            d.drain(at(m.a)..at(m.a + m.b));
        }
        "repeat" if inside(m.a, m.b) => {
            let part = d[at(m.a)..at(m.a + m.b)].to_vec();
            d.splice(at(m.a + m.b)..at(m.a + m.b), part);
        }
        _ => return Err(format!("bad mutation {} {} {} {} on {n} bytes", m.op, m.a, m.b, m.c)),
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json::hex_bytes as hex;

    fn m(op: &str, a: u64, b: u64, c: u64) -> Mutation {
        Mutation { op: op.to_string(), a, b, c }
    }

    #[test]
    fn the_same_numbers_as_the_cpp_generator() {
        // tests/diff/test_diff.cpp pins the same values
        let mut r = Rng::new(0);
        assert_eq!(r.next_u64(), 0xe220_a839_7b1d_cdaf); // SplitMix64's published first output
        assert_eq!(r.next_u64(), 0x6e78_9e6a_a1b9_65f4);
        assert_eq!(Rng::for_case(20261112, 7).next_u64(), 0xbacf_e2bf_7d6b_a8b6);
        assert_eq!(fnv1a(&[], FNV_OFFSET), 0xcbf2_9ce4_8422_2325);
        assert_eq!(fnv1a(b"foobar", FNV_OFFSET), 0x8594_4171_f739_67e8);
        let expect = [
            "59d0f5e41c790a880fdaa0df",
            "000000000000000000000000",
            "ffffffffffffffffffffffff",
            "010001000001000001000001",
            "fffeff00fe01fefe807ffe01",
            "0000000000000a000f000000",
        ];
        for (style, want) in (0u64..).zip(expect) {
            assert_eq!(hex(&fill(20261112, 3, 12, style).unwrap()), want, "style {style}");
        }
        assert!(fill(1, 1, 4, 6).is_err());
        // Lengths that are not a multiple of 8 take the first bytes of the last number
        assert_eq!(fill(20261112, 3, 5, 0).unwrap(), fill(20261112, 3, 12, 0).unwrap()[..5]);
    }

    #[test]
    fn mutations_as_the_cpp_ones() {
        // The sequence of the C++ test "mutations change the bytes and keep the chunks matching"
        let mut d = vec![0u8; 40];
        for op in [m("flip", 9, 0, 0), m("set", 0, 0xAB, 0), m("copy", 0, 2, 20)] {
            apply(&mut d, &op).unwrap();
        }
        assert_eq!((d[1], d[20], d[21]), (0x40, 0xAB, 0x40));
        apply(&mut d, &m("repeat", 20, 10, 0)).unwrap();
        assert_eq!((d.len(), d[30]), (50, 0xAB));
        apply(&mut d, &m("cut", 0, 10, 0)).unwrap();
        assert_eq!((d.len(), d[10]), (40, 0xAB));
        apply(&mut d, &m("truncate", 25, 0, 0)).unwrap();
        assert_eq!(d.len(), 25);

        let mut small = vec![0u8; 4];
        assert!(apply(&mut small, &m("cut", 2, 3, 0)).is_err());
        assert!(apply(&mut small, &m("flip", 32, 0, 0)).is_err());
        assert!(apply(&mut small, &m("copy", u64::MAX, 2, 0)).is_err());
        assert!(apply(&mut small, &m("set", 0, 256, 0)).is_err());
        assert!(apply(&mut small, &m("shuffle", 0, 0, 0)).is_err());
        assert_eq!(small, [0, 0, 0, 0]);
    }
}
