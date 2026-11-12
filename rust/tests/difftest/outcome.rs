//! Outcomes of the differential tests, as `tests/diff/outcome.cpp`: a status, output bytes and
//! records of named values; their digests (the same numbers as the C++), the float rule and the
//! first difference in plain words.

use crate::json::{Json, hex_bytes, hex64, unhex, unhex64};
use crate::rng::{FNV_OFFSET, fnv1a};

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Int(i64),
    Bytes(Vec<u8>),
    /// 32-bit floats as bit patterns
    F32(Vec<u32>),
    /// 64-bit floats as bit patterns
    F64(Vec<u64>),
    /// Only the kind ('b', 'f' or 'd'), length and digest were kept (long data in an oracle line)
    Summary { kind: char, len: u64, fnv: u64 },
}

impl Value {
    pub fn floats(v: &[f32]) -> Value {
        Value::F32(v.iter().map(|x| x.to_bits()).collect())
    }

    fn kind(&self) -> char {
        match self {
            Value::Int(_) => 'i',
            Value::Bytes(_) => 'b',
            Value::F32(_) => 'f',
            Value::F64(_) => 'd',
            Value::Summary { kind, .. } => *kind,
        }
    }

    pub fn length(&self) -> u64 {
        match self {
            Value::Int(_) => 0,
            Value::Bytes(v) => v.len() as u64,
            Value::F32(v) => v.len() as u64,
            Value::F64(v) => v.len() as u64,
            Value::Summary { len, .. } => *len,
        }
    }

    pub fn fnv(&self) -> u64 {
        match self {
            Value::Int(i) => fnv1a(&i.to_le_bytes(), FNV_OFFSET),
            Value::Bytes(v) => fnv1a(v, FNV_OFFSET),
            Value::F32(v) => v.iter().fold(FNV_OFFSET, |h, x| fnv1a(&x.to_le_bytes(), h)),
            Value::F64(v) => v.iter().fold(FNV_OFFSET, |h, x| fnv1a(&x.to_le_bytes(), h)),
            Value::Summary { fnv, .. } => *fnv,
        }
    }
}

pub type Record = Vec<(String, Value)>;

#[derive(Clone, Debug, PartialEq)]
pub struct Outcome {
    pub status: i64,
    pub bytes: Value,
    pub records: Vec<Record>,
    /// Read from an oracle line that kept only the number of records and their digest
    pub records_summary: Option<(u64, u64)>,
}

impl Outcome {
    pub fn new(status: i64, bytes: Vec<u8>, records: Vec<Record>) -> Outcome {
        Outcome { status, bytes: Value::Bytes(bytes), records, records_summary: None }
    }

    pub fn count(&self) -> u64 {
        self.records_summary.map_or(self.records.len() as u64, |(n, _)| n)
    }

    /// Each field: its name, a 0, its kind letter, then the integer, or the length and digest
    /// of the data; a 0x1E ends each record
    pub fn digest(&self) -> u64 {
        if let Some((_, d)) = self.records_summary {
            return d;
        }
        let mut h = FNV_OFFSET;
        for record in &self.records {
            for (name, value) in record {
                h = fnv1a(name.as_bytes(), h);
                h = fnv1a(&[0, u8::try_from(value.kind()).expect("an ASCII kind letter")], h);
                h = match value {
                    Value::Int(i) => fnv1a(&i.to_le_bytes(), h),
                    _ => fnv1a(&value.fnv().to_le_bytes(), fnv1a(&value.length().to_le_bytes(), h)),
                };
            }
            h = fnv1a(&[0x1E], h);
        }
        h
    }
}

/// How floats are compared: exact (0, the default) or within this many ULP
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FloatRule {
    pub max_ulps: u64,
}

/// The ULP distance of a NaN or infinity and anything else
pub const NOT_CLOSE: u64 = u64::MAX;

/// Floats as unsigned numbers in the same order as the values: -0 and +0 meet in the middle
fn ordered(bits: u64, width: u32) -> u64 {
    let sign = 1u64 << (width - 1);
    let magnitude = bits & (sign - 1);
    if bits & sign != 0 { (1u64 << 63) - magnitude } else { (1u64 << 63) + magnitude }
}

fn distance(a: u64, b: u64, width: u32, nan: (bool, bool), inf: (bool, bool)) -> u64 {
    if nan.0 || nan.1 {
        return if nan.0 && nan.1 { 0 } else { NOT_CLOSE };
    }
    if inf.0 || inf.1 {
        return if a == b { 0 } else { NOT_CLOSE };
    }
    ordered(a, width).abs_diff(ordered(b, width))
}

pub fn ulp_f32(a: f32, b: f32) -> u64 {
    let (x, y) = (u64::from(a.to_bits()), u64::from(b.to_bits()));
    distance(x, y, 32, (a.is_nan(), b.is_nan()), (a.is_infinite(), b.is_infinite()))
}

pub fn ulp_f64(a: f64, b: f64) -> u64 {
    distance(a.to_bits(), b.to_bits(), 64, (a.is_nan(), b.is_nan()), (a.is_infinite(), b.is_infinite()))
}

const DIGEST_ONLY: &str = " (only a digest was kept; replay the reproducer to see the first difference)";

/// A few bytes on each side of position i, the one at i in brackets
fn around(d: &[u8], i: usize) -> String {
    let from = i.saturating_sub(4);
    let to = d.len().min(i + 5);
    let parts: Vec<String> =
        (from..to).map(|k| if k == i { format!("[{:02x}]", d[k]) } else { format!("{:02x}", d[k]) }).collect();
    parts.join(" ")
}

fn f32_bits(v: u64) -> f32 {
    f32::from_bits(u32::try_from(v).expect("an f32 bit pattern"))
}

/// The first float that differs by more than the rule allows (both sides the same kind)
fn compare_floats(what: &str, w: &[u64], g: &[u64], wide: bool, rule: FloatRule, wn: &str, gn: &str) -> String {
    let text = |bits: u64| {
        if wide {
            format!("{:?} (0x{bits:016x})", f64::from_bits(bits))
        } else {
            format!("{:?} (0x{bits:08x})", f32_bits(bits))
        }
    };
    for (i, (&a, &b)) in w.iter().zip(g).enumerate() {
        if a == b {
            continue;
        }
        let d = if wide { ulp_f64(f64::from_bits(a), f64::from_bits(b)) } else { ulp_f32(f32_bits(a), f32_bits(b)) };
        if rule.max_ulps > 0 && d <= rule.max_ulps {
            continue;
        }
        let apart = match d {
            NOT_CLOSE => "not comparable (NaN or infinity)".to_string(),
            0 => "equal as numbers but not bit for bit (+0 and -0, or two NaNs)".to_string(),
            _ => format!("{d} ULP apart"),
        };
        return format!(
            "{what}: value {i} of {}: {wn} {}, {gn} {}, {apart}, {} allowed",
            w.len(),
            text(a),
            text(b),
            rule.max_ulps
        );
    }
    if w.len() != g.len() {
        return format!("{what}: {wn} has {} values, {gn} {} (the same up to there)", w.len(), g.len());
    }
    String::new()
}

fn compare_bytes(what: &str, a: &[u8], b: &[u8], wn: &str, gn: &str) -> String {
    if let Some(i) = a.iter().zip(b).position(|(x, y)| x != y) {
        return format!(
            "{what}: first difference at byte {i} of {}: {wn} 0x{:02x}, {gn} 0x{:02x} ({wn} {}, {gn} {})",
            a.len(),
            a[i],
            b[i],
            around(a, i),
            around(b, i)
        );
    }
    if a.len() != b.len() {
        return format!("{what}: {wn} gives {} bytes, {gn} {} (the same up to there)", a.len(), b.len());
    }
    String::new()
}

fn compare_value(what: &str, w: &Value, g: &Value, rule: FloatRule, wn: &str, gn: &str) -> String {
    if w.kind() != g.kind() {
        return format!("{what}: {wn} gives kind '{}', {gn} '{}'", w.kind(), g.kind());
    }
    let widen = |v: &[u32]| v.iter().map(|&x| u64::from(x)).collect::<Vec<u64>>();
    match (w, g) {
        (Value::Int(a), Value::Int(b)) if a == b => String::new(),
        (Value::Int(a), Value::Int(b)) => format!("{what}: {wn} {a}, {gn} {b}"),
        (Value::Summary { .. }, _) | (_, Value::Summary { .. }) => {
            if w.length() == g.length() && w.fnv() == g.fnv() {
                String::new()
            } else if w.kind() != 'b' && rule.max_ulps > 0 {
                format!("{what}: a float tolerance needs the values, but only a digest was kept")
            } else {
                format!(
                    "{what}: {wn} {} long with digest {:016x}, {gn} {} long with digest {:016x}{DIGEST_ONLY}",
                    w.length(),
                    w.fnv(),
                    g.length(),
                    g.fnv()
                )
            }
        }
        (Value::F32(a), Value::F32(b)) => compare_floats(what, &widen(a), &widen(b), false, rule, wn, gn),
        (Value::F64(a), Value::F64(b)) => compare_floats(what, a, b, true, rule, wn, gn),
        (Value::Bytes(a), Value::Bytes(b)) => compare_bytes(what, a, b, wn, gn),
        _ => format!("{what}: {wn} and {gn} give values of different kinds"),
    }
}

fn record_names(r: &Record) -> String {
    let names: Vec<&str> = r.iter().map(|(n, _)| n.as_str()).collect();
    format!("[{}]", names.join(", "))
}

/// "" when `got` gives the same as `want`, else the first difference in plain words
pub fn compare(want: &Outcome, got: &Outcome, rule: FloatRule, wn: &str, gn: &str) -> String {
    if want.status != got.status {
        return format!("status: {wn} {}, {gn} {}", want.status, got.status);
    }
    let d = compare_value("bytes", &want.bytes, &got.bytes, rule, wn, gn);
    if !d.is_empty() {
        return d;
    }
    if want.records_summary.is_some() || got.records_summary.is_some() {
        if want.count() == got.count() && want.digest() == got.digest() {
            return String::new();
        }
        return format!(
            "records: {wn} has {} with digest {:016x}, {gn} {} with digest {:016x}{DIGEST_ONLY}",
            want.count(),
            want.digest(),
            got.count(),
            got.digest()
        );
    }
    for (k, (a, b)) in want.records.iter().zip(&got.records).enumerate() {
        for ((an, av), (bn, bv)) in a.iter().zip(b) {
            if an != bn {
                return format!("record {k}: fields {wn} {}, {gn} {}", record_names(a), record_names(b));
            }
            let d = compare_value(&format!("record {k} {an}"), av, bv, rule, wn, gn);
            if !d.is_empty() {
                return d;
            }
        }
        if a.len() != b.len() {
            return format!("record {k}: fields {wn} {}, {gn} {}", record_names(a), record_names(b));
        }
    }
    let (n, m) = (want.records.len(), got.records.len());
    if n != m {
        let extra = if n > m { &want.records[m] } else { &got.records[n] };
        return format!(
            "records: {wn} gives {n}, {gn} {m} (the same up to there; the first extra one has fields {})",
            record_names(extra)
        );
    }
    String::new()
}

/// The JSON key of data of this kind
fn kind_key(kind: char) -> &'static str {
    match kind {
        'b' => "bytes",
        'f' => "f32",
        _ => "f64",
    }
}

/// A value from its JSON (tests/diff/io.cpp: an integer, or {"bytes"|"f32"|"f64": length,
/// "fnv", "hex"|"bits"}), checked against its length and digest
pub fn value_from_json(j: &Json) -> Result<Value, String> {
    if let Json::Int(_) = j {
        return Ok(Value::Int(j.i64()?));
    }
    let Some(kind) = ['b', 'f', 'd'].into_iter().find(|k| j.has(kind_key(*k))) else {
        return Err(format!("not a value: {j}"));
    };
    let len = j.get(kind_key(kind))?.u64()?;
    let fnv = unhex64(j.get("fnv")?.str()?)?;
    let bits = || -> Result<Vec<u64>, String> { j.get("bits")?.arr()?.iter().map(|b| unhex64(b.str()?)).collect() };
    let v = if j.has("hex") {
        Value::Bytes(unhex(j.get("hex")?.str()?)?)
    } else if j.has("bits") && kind == 'f' {
        let narrow = |b: u64| u32::try_from(b).map_err(|e| format!("an f32 bit pattern: {e}"));
        Value::F32(bits()?.into_iter().map(narrow).collect::<Result<_, _>>()?)
    } else if j.has("bits") {
        Value::F64(bits()?)
    } else {
        return Ok(Value::Summary { kind, len, fnv });
    };
    if v.kind() != kind || v.length() != len || v.fnv() != fnv {
        return Err(format!("a value's data does not match its kind, length and digest: {j}"));
    }
    Ok(v)
}

/// A value as JSON, always with its data (reproducers keep everything)
pub fn value_to_json(v: &Value) -> Json {
    let data = match v {
        Value::Int(i) => return Json::from(*i),
        Value::Summary { .. } => None,
        Value::Bytes(d) => Some(("hex", Json::from(hex_bytes(d).as_str()))),
        Value::F32(d) => Some(("bits", Json::Arr(d.iter().map(|b| Json::from(format!("{b:08x}").as_str())).collect()))),
        Value::F64(d) => Some(("bits", Json::Arr(d.iter().map(|b| Json::from(hex64(*b).as_str())).collect()))),
    };
    let mut fields = vec![(kind_key(v.kind()), Json::from(v.length())), ("fnv", Json::from(hex64(v.fnv()).as_str()))];
    fields.extend(data);
    Json::object(fields)
}

pub fn outcome_from_json(j: &Json) -> Result<Outcome, String> {
    let r = j.get("records")?;
    let (count, digest) = (r.get("count")?.u64()?, unhex64(r.get("fnv")?.str()?)?);
    let mut o = Outcome {
        status: j.get("status")?.i64()?,
        bytes: value_from_json(j.get("bytes")?)?,
        records: Vec::new(),
        records_summary: None,
    };
    if !r.has("list") {
        o.records_summary = Some((count, digest));
        return Ok(o);
    }
    for record in r.get("list")?.arr()? {
        let mut fields = Record::new();
        for f in record.arr()? {
            let [name, value] = f.arr()? else {
                return Err(format!("a field is a [name, value] pair: {f}"));
            };
            fields.push((name.str()?.to_string(), value_from_json(value)?));
        }
        o.records.push(fields);
    }
    if o.count() != count || o.digest() != digest {
        return Err("the records do not match their count and digest".to_string());
    }
    Ok(o)
}

pub fn outcome_to_json(o: &Outcome) -> Json {
    let mut records = vec![("count", Json::from(o.count())), ("fnv", Json::from(hex64(o.digest()).as_str()))];
    if o.records_summary.is_none() {
        let field = |(n, v): &(String, Value)| Json::Arr(vec![Json::from(n.as_str()), value_to_json(v)]);
        let list = o.records.iter().map(|r| Json::Arr(r.iter().map(field).collect()));
        records.push(("list", Json::Arr(list.collect())));
    }
    Json::object(vec![
        ("status", Json::from(o.status)),
        ("bytes", value_to_json(&o.bytes)),
        ("records", Json::object(records)),
    ])
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json::parse;

    fn bytes(status: i64, d: &[u8]) -> Outcome {
        Outcome::new(status, d.to_vec(), Vec::new())
    }

    fn field(name: &str, v: Value) -> (String, Value) {
        (name.to_string(), v)
    }

    #[test]
    fn the_records_digest_is_the_cpp_one() {
        // tests/diff/test_diff.cpp pins the same number
        let records = vec![
            vec![field("at", Value::Int(5)), field("payload", Value::Bytes(b"ab".to_vec()))],
            vec![field("y", Value::floats(&[1.0, -0.0]))],
        ];
        let o = Outcome::new(0, Vec::new(), records);
        assert_eq!(o.digest(), 0xa189_7933_d37f_8cf4);
        // Through JSON and back, and as a summary
        let back = outcome_from_json(&parse(&outcome_to_json(&o).to_string()).unwrap()).unwrap();
        assert_eq!(back, o);
        let summary = Outcome { records: Vec::new(), records_summary: Some((2, o.digest())), ..o.clone() };
        assert_eq!(compare(&summary, &o, FloatRule::default(), "a", "b"), "");
        let one_less = Outcome { records: o.records[..1].to_vec(), ..o.clone() };
        assert!(compare(&summary, &one_less, FloatRule::default(), "a", "b").contains("only a digest was kept"));
    }

    #[test]
    fn ulp_distances_with_nan_and_infinity_never_close() {
        let next = f32::from_bits(1.0f32.to_bits() + 1);
        assert_eq!(ulp_f32(1.0, 1.0), 0);
        assert_eq!(ulp_f32(1.0, next), 1);
        assert_eq!(ulp_f32(-1.0, -next), 1);
        assert_eq!(ulp_f32(0.0, -0.0), 0);
        let tiny = f32::from_bits(1);
        assert_eq!(ulp_f32(-tiny, tiny), 2);
        assert_eq!(ulp_f32(0.0, tiny), 1);
        assert_eq!(ulp_f32(f32::NAN, f32::from_bits(0x7fc0_0001)), 0);
        assert_eq!(ulp_f32(f32::NAN, 1.0), NOT_CLOSE);
        assert_eq!(ulp_f32(f32::INFINITY, f32::INFINITY), 0);
        assert_eq!(ulp_f32(f32::INFINITY, f32::MAX), NOT_CLOSE);
        assert_eq!(ulp_f32(f32::INFINITY, f32::NEG_INFINITY), NOT_CLOSE);
        assert_eq!(ulp_f32(-f32::MAX, f32::MAX), 2 * 0x7f7f_ffff);
        assert_eq!(ulp_f64(1.0, f64::from_bits(1.0f64.to_bits() - 1)), 1);
        assert_eq!(ulp_f64(-f64::MAX, f64::MAX), 2 * 0x7fef_ffff_ffff_ffff);
        assert_eq!(ulp_f64(f64::NAN, f64::NAN), 0);
        assert_eq!(ulp_f64(f64::NEG_INFINITY, f64::NEG_INFINITY), 0);
    }

    #[test]
    fn the_first_difference_in_plain_words() {
        let exact = FloatRule::default();
        let a = bytes(0, &[1, 2, 3, 4, 5, 6]);
        assert_eq!(compare(&a, &a, exact, "C++", "Rust"), "");
        assert_eq!(
            compare(&a, &bytes(0, &[1, 2, 3, 9, 5, 6]), exact, "C++", "Rust"),
            "bytes: first difference at byte 3 of 6: C++ 0x04, Rust 0x09 (C++ 01 02 03 [04] 05 06, Rust 01 02 03 [09] 05 06)"
        );
        let short = compare(&a, &bytes(0, &[1, 2, 3]), exact, "C++", "Rust");
        assert_eq!(short, "bytes: C++ gives 6 bytes, Rust 3 (the same up to there)");
        assert_eq!(compare(&a, &bytes(1, &[1, 2, 3, 4, 5, 6]), exact, "C++", "Rust"), "status: C++ 0, Rust 1");

        let rec = |apid: i64| vec![field("apid", Value::Int(apid)), field("payload", Value::Bytes(vec![7, 8]))];
        let (x, y) = (Outcome::new(0, Vec::new(), vec![rec(64)]), Outcome::new(0, Vec::new(), vec![rec(65)]));
        assert_eq!(compare(&x, &y, exact, "C++", "Rust"), "record 0 apid: C++ 64, Rust 65");
        assert_eq!(
            compare(&x, &Outcome::new(0, Vec::new(), Vec::new()), exact, "C++", "Rust"),
            "records: C++ gives 1, Rust 0 (the same up to there; the first extra one has fields [apid, payload])"
        );

        let floats = |v: &[f32]| Outcome::new(0, Vec::new(), vec![vec![field("y", Value::floats(v))]]);
        let next = f32::from_bits(1.0f32.to_bits() + 1);
        let one_ulp = FloatRule { max_ulps: 1 };
        let d = compare(&floats(&[1.0, 2.0]), &floats(&[next, 2.0]), exact, "C++", "Rust");
        let want = "record 0 y: value 0 of 2: C++ 1.0 (0x3f800000), Rust 1.0000001 (0x3f800001), 1 ULP apart, 0 allowed";
        assert_eq!(d, want);
        assert_eq!(compare(&floats(&[1.0, 2.0]), &floats(&[next, 2.0]), one_ulp, "C++", "Rust"), "");
        let nan = compare(&floats(&[1.0, 2.0]), &floats(&[f32::NAN, 2.0]), FloatRule { max_ulps: 1000 }, "C++", "Rust");
        assert!(nan.contains("not comparable"), "{nan}");
        let zeros = compare(&floats(&[0.0]), &floats(&[-0.0]), exact, "C++", "Rust");
        assert!(zeros.contains("equal as numbers but not bit for bit"), "{zeros}");
        assert_eq!(compare(&floats(&[0.0]), &floats(&[-0.0]), one_ulp, "C++", "Rust"), "");
    }

    #[test]
    fn values_are_checked_against_their_digest() {
        let good = parse(&value_to_json(&Value::Bytes(vec![1, 2, 3])).to_string()).unwrap();
        assert_eq!(value_from_json(&good).unwrap(), Value::Bytes(vec![1, 2, 3]));
        let bad = parse(r#"{"bytes":3,"fnv":"0000000000000000","hex":"010203"}"#).unwrap();
        assert!(value_from_json(&bad).is_err());
        let summary = parse(r#"{"f64":3,"fnv":"00000000000000ff"}"#).unwrap();
        assert_eq!(value_from_json(&summary).unwrap(), Value::Summary { kind: 'd', len: 3, fnv: 255 });
        let wide = Value::F64(vec![1.5f64.to_bits(), f64::NAN.to_bits()]);
        assert_eq!(value_from_json(&parse(&value_to_json(&wide).to_string()).unwrap()).unwrap(), wide);
    }
}
