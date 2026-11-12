//! Fork (M6.3): the Rust runners of the Meteor LRPT twins (`tests/diff/twins_lrpt.cpp`), through
//! the C functions SatDump calls (`satdump_rs::ffi_lrpt`). The outcome has the C++ twin's layout:
//! the 8 x 112 pixels as bytes, and one record of the header fields, the time stamp (a 64-bit
//! float), valid and partial.

use crate::oracle::Case;
use crate::outcome::{Outcome, Record, Value};
use satdump_rs::ffi::STATUS_OK;
use satdump_rs::ffi_lrpt::{LrptSegment, satdump_rs_lrpt_quant_table, satdump_rs_lrpt_segment};
use satdump_rs::lrpt;

fn int(name: &str, v: impl Into<i64>) -> (String, Value) {
    (name.to_string(), Value::Int(v.into()))
}

fn outcome(status: i32, s: &LrptSegment) -> Outcome {
    let record: Record = vec![
        int("day_time", s.day_time),
        int("ms_time", s.ms_time),
        int("us_time", s.us_time),
        ("timestamp".to_string(), Value::F64(vec![s.timestamp.to_bits()])),
        int("mcun", s.mcun),
        int("qt", s.qt),
        int("dc", s.dc),
        int("ac", s.ac),
        int("qfm", s.qfm),
        int("qf", s.qf),
        int("valid", s.valid),
        int("partial", s.partial),
    ];
    Outcome::new(i64::from(status), s.lines.concat(), vec![record])
}

/// lrpt_segment: params "partial", "m2x", and "length" only for a negative length
pub fn segment(c: &Case) -> Outcome {
    let real = i64::try_from(c.input.len()).expect("an input length");
    let length = c.param("length", real);
    assert!(length < 0 || length == real, "case {}: a length that is not the input's", c.id);
    let length = i32::try_from(length).expect("a length that fits in an int");
    let (partial, m2x) = (c.param("partial", 0) != 0, c.param("m2x", 0) != 0);
    let mut out = LrptSegment::from(&lrpt::decode(&[], partial, m2x));
    // SAFETY: the input has `length` readable bytes when the length is not negative, and `out`
    // is an LrptSegment of this function's own
    let status = unsafe { satdump_rs_lrpt_segment(c.input.as_ptr(), length, u8::from(partial), u8::from(m2x), &mut out) };
    // When Rust refuses the packet (a negative length), the C++ constructor decodes it: as an
    // empty packet, which `out` already holds
    outcome(status, &out)
}

/// lrpt_qtable: the table for quality factor "qf", 64 integers as little-endian bytes
pub fn qtable(c: &Case) -> Outcome {
    let qf = i32::try_from(c.param("qf", 0)).expect("a quality factor that fits in an int");
    let mut t = [0i64; 64];
    // SAFETY: t has 64 writable values
    let status = unsafe { satdump_rs_lrpt_quant_table(qf, t.as_mut_ptr()) };
    let bytes = t.iter().flat_map(|v| v.to_le_bytes()).collect();
    Outcome::new(i64::from(status), bytes, vec![vec![int("qf", c.param("qf", 0))]])
}

#[cfg(test)]
mod tests {
    use super::*;

    fn case(input: &[u8], params: &[(&str, i64)]) -> Case {
        Case {
            id: "t".to_string(),
            chunks: Vec::new(),
            params: params.iter().map(|(n, v)| ((*n).to_string(), *v)).collect(),
            input: input.to_vec(),
        }
    }

    #[test]
    fn a_header_only_packet_is_not_valid_and_a_negative_length_is_refused() {
        let o = segment(&case(&[0u8; 14], &[("partial", 1)]));
        assert_eq!(o.status, i64::from(STATUS_OK));
        assert_eq!(o.records[0][3].1, Value::F64(vec![(-1.0f64).to_bits()]));
        assert_eq!(o.records[0][10].1, Value::Int(0));
        assert_eq!(o.records[0][11].1, Value::Int(1));
        // The same outcome, as the C++ constructor gives it, with the status of the refusal
        let refused = segment(&case(&[0u8; 32], &[("partial", 1), ("length", -1)]));
        assert_eq!(refused.status, i64::from(satdump_rs::ffi::STATUS_BAD_ARGUMENT));
        assert_eq!(refused.records, o.records);
        assert_eq!(refused.bytes, o.bytes);
    }

    #[test]
    fn the_table_for_quality_80_is_the_standard_one_scaled() {
        let o = qtable(&case(&[], &[("qf", 80)]));
        let Value::Bytes(b) = &o.bytes else { panic!("bytes") };
        assert_eq!(b.len(), 64 * 8);
        // 16 * 0.4 + 0.5, truncated
        assert_eq!(i64::from_le_bytes(b[..8].try_into().expect("8 bytes")), 6);
    }
}
