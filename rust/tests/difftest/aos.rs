//! Fork part "aos": the Rust runner of the aos_demux twin (`tests/diff/twins_aos.cpp`), with
//! the Rust demuxer alone. It gives the outcome the C++ twin writes into the oracles: one
//! record per packet, CADU by CADU.

use crate::oracle::Case;
use crate::outcome::{Outcome, Record, Value};
use satdump_rs::aos::{Demuxer, Packet, Settings};
use std::collections::BTreeMap;

/// STATUS_BAD_ARGUMENT in `rust/src/ffi.rs`: settings the Rust demuxer does not take
const REFUSED: i64 = 1;

/// The settings of a case, or the four demuxers of a corpus file (no "mpdu" parameter): the
/// MetOp / Meteor layout with the counter check off and on, times strict mode off and on
fn variants(c: &Case) -> Option<Vec<Settings>> {
    let flag = |name: &str| c.param(name, 0) != 0;
    let int = |name: &str| i32::try_from(c.param(name, 0)).ok();
    if is_corpus(c) {
        return [(false, false), (false, true), (true, false), (true, true)]
            .iter()
            .map(|&(counter, strict)| Settings::new(882, true, 2, false, counter, strict))
            .collect();
    }
    Some(vec![Settings::new(int("mpdu")?, flag("has_iz"), int("iz")?, flag("ext"), flag("counter"), flag("strict"))?])
}

fn is_corpus(c: &Case) -> bool {
    !c.params.iter().any(|(n, _)| n == "mpdu")
}

fn record(cadu: i64, variant: usize, vcid: i64, p: &Packet) -> Record {
    let h = &p.header;
    let int = |name: &str, v: i64| (name.to_string(), Value::Int(v));
    vec![
        int("cadu", cadu),
        int("variant", i64::try_from(variant).expect("four at most")),
        int("vcid", vcid),
        int("apid", (i64::from(h[0] & 7) << 8) | i64::from(h[1])),
        int("version", i64::from(h[0] >> 5)),
        int("type", i64::from((h[0] >> 4) & 1)),
        int("secondary", i64::from((h[0] >> 3) & 1)),
        int("sequence_flag", i64::from(h[2] >> 6)),
        int("sequence", (i64::from(h[2] & 0x3F) << 8) | i64::from(h[3])),
        int("length", i64::from(u16::from_be_bytes([h[4], h[5]]))),
        ("header".to_string(), Value::Bytes(h.to_vec())),
        ("payload".to_string(), Value::Bytes(p.payload.clone())),
    ]
}

/// aos_demux
pub fn demux(c: &Case) -> Outcome {
    let Some(variants) = variants(c) else {
        return Outcome::new(REFUSED, Vec::new(), Vec::new());
    };
    let route = c.param("route", i64::from(is_corpus(c))) != 0;
    let bytes = variants[0].cadu_bytes();
    let mut demuxers: BTreeMap<(usize, i64), Demuxer> = BTreeMap::new();
    let mut records = Vec::new();
    let mut cadu = vec![0u8; bytes];
    let mut pos = 0;
    for (index, n) in c.chunks().into_iter().enumerate() {
        if n > 0 {
            cadu.fill(0);
            let take = n.min(bytes);
            cadu[..take].copy_from_slice(&c.input[pos..pos + take]);
            let vcid = if route { i64::from(cadu[5] & 0x3F) } else { -1 };
            for (k, s) in variants.iter().enumerate() {
                let d = demuxers.entry((k, vcid)).or_insert_with(|| Demuxer::new(*s));
                let at = i64::try_from(index).expect("a CADU number fits in i64");
                records.extend(d.work(&cadu).iter().map(|p| record(at, k, vcid, p)));
            }
        }
        pos += n;
    }
    Outcome::new(0, Vec::new(), records)
}
