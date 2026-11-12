//! Just enough JSON for the oracle lines and reproducers that `tests/diff/io.cpp` writes with
//! nlohmann/json: objects, arrays, strings, integers, true, false and null. The crate has no
//! dependencies (it builds offline), so this is written out here.

use std::fmt;

#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    /// Every number in these files is an integer: i64 or u64, both fit
    Int(i128),
    Str(String),
    Arr(Vec<Json>),
    /// Fields in the order they were written
    Obj(Vec<(String, Json)>),
}

impl Json {
    pub fn object(fields: Vec<(&str, Json)>) -> Json {
        Json::Obj(fields.into_iter().map(|(k, v)| (k.to_string(), v)).collect())
    }

    pub fn has(&self, key: &str) -> bool {
        matches!(self, Json::Obj(f) if f.iter().any(|(k, _)| k == key))
    }

    pub fn get(&self, key: &str) -> Result<&Json, String> {
        match self {
            Json::Obj(f) => f.iter().find(|(k, _)| k == key).map(|(_, v)| v).ok_or_else(|| format!("JSON: no \"{key}\"")),
            _ => Err(format!("JSON: not an object (looking for \"{key}\")")),
        }
    }

    pub fn u64(&self) -> Result<u64, String> {
        match self {
            Json::Int(v) => u64::try_from(*v).map_err(|_| format!("JSON: {v} is not an unsigned 64-bit number")),
            _ => Err(format!("JSON: not a number: {self}")),
        }
    }

    pub fn i64(&self) -> Result<i64, String> {
        match self {
            Json::Int(v) => i64::try_from(*v).map_err(|_| format!("JSON: {v} is not a 64-bit number")),
            _ => Err(format!("JSON: not a number: {self}")),
        }
    }

    pub fn str(&self) -> Result<&str, String> {
        match self {
            Json::Str(s) => Ok(s),
            _ => Err(format!("JSON: not a string: {self}")),
        }
    }

    pub fn arr(&self) -> Result<&[Json], String> {
        match self {
            Json::Arr(a) => Ok(a),
            _ => Err(format!("JSON: not an array: {self}")),
        }
    }

    pub fn fields(&self) -> Result<&[(String, Json)], String> {
        match self {
            Json::Obj(f) => Ok(f),
            _ => Err(format!("JSON: not an object: {self}")),
        }
    }
}

impl From<u64> for Json {
    fn from(v: u64) -> Json {
        Json::Int(i128::from(v))
    }
}

impl From<i64> for Json {
    fn from(v: i64) -> Json {
        Json::Int(i128::from(v))
    }
}

impl From<&str> for Json {
    fn from(v: &str) -> Json {
        Json::Str(v.to_string())
    }
}

fn write_string(f: &mut fmt::Formatter<'_>, s: &str) -> fmt::Result {
    f.write_str("\"")?;
    for ch in s.chars() {
        match ch {
            '"' => f.write_str("\\\"")?,
            '\\' => f.write_str("\\\\")?,
            '\n' => f.write_str("\\n")?,
            '\r' => f.write_str("\\r")?,
            '\t' => f.write_str("\\t")?,
            c if u32::from(c) < 0x20 => write!(f, "\\u{:04x}", u32::from(c))?,
            c => write!(f, "{c}")?,
        }
    }
    f.write_str("\"")
}

/// Compact JSON, as nlohmann's `dump()`
impl fmt::Display for Json {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Json::Null => f.write_str("null"),
            Json::Bool(b) => write!(f, "{b}"),
            Json::Int(v) => write!(f, "{v}"),
            Json::Str(s) => write_string(f, s),
            Json::Arr(a) => {
                f.write_str("[")?;
                for (i, v) in a.iter().enumerate() {
                    write!(f, "{}{v}", if i == 0 { "" } else { "," })?;
                }
                f.write_str("]")
            }
            Json::Obj(fields) => {
                f.write_str("{")?;
                for (i, (k, v)) in fields.iter().enumerate() {
                    f.write_str(if i == 0 { "" } else { "," })?;
                    write_string(f, k)?;
                    write!(f, ":{v}")?;
                }
                f.write_str("}")
            }
        }
    }
}

pub fn parse(text: &str) -> Result<Json, String> {
    let mut p = Parser { s: text.as_bytes(), i: 0 };
    let v = p.value()?;
    p.space();
    if p.i != p.s.len() {
        return p.fail("text after the value");
    }
    Ok(v)
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
}

impl Parser<'_> {
    fn fail<T>(&self, what: &str) -> Result<T, String> {
        Err(format!("JSON: {what} at byte {}", self.i))
    }

    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    fn space(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t' | b'\n' | b'\r')) {
            self.i += 1;
        }
    }

    fn eat(&mut self, c: u8) -> Result<(), String> {
        self.space();
        if self.peek() != Some(c) {
            return self.fail(&format!("expected '{}'", char::from(c)));
        }
        self.i += 1;
        Ok(())
    }

    fn value(&mut self) -> Result<Json, String> {
        self.space();
        match self.peek() {
            Some(b'{') => self.object(),
            Some(b'[') => self.array(),
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(b't') => self.word("true", Json::Bool(true)),
            Some(b'f') => self.word("false", Json::Bool(false)),
            Some(b'n') => self.word("null", Json::Null),
            Some(b'-' | b'0'..=b'9') => self.number(),
            _ => self.fail("expected a value"),
        }
    }

    fn word(&mut self, w: &str, v: Json) -> Result<Json, String> {
        if !self.s[self.i..].starts_with(w.as_bytes()) {
            return self.fail("unknown word");
        }
        self.i += w.len();
        Ok(v)
    }

    fn number(&mut self) -> Result<Json, String> {
        let start = self.i;
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        while matches!(self.peek(), Some(b'0'..=b'9')) {
            self.i += 1;
        }
        if matches!(self.peek(), Some(b'.' | b'e' | b'E')) {
            return self.fail("a number that is not an integer (these files have none)");
        }
        let text = std::str::from_utf8(&self.s[start..self.i]).map_err(|e| e.to_string())?;
        text.parse::<i128>().map(Json::Int).map_err(|e| format!("JSON: bad number {text}: {e}"))
    }

    fn hex4(&mut self) -> Result<u32, String> {
        let digits = self.s.get(self.i..self.i + 4).ok_or("JSON: a short \\u escape")?;
        if !digits.iter().all(u8::is_ascii_hexdigit) {
            return self.fail("a bad \\u escape");
        }
        let text = std::str::from_utf8(digits).map_err(|e| e.to_string())?;
        self.i += 4;
        u32::from_str_radix(text, 16).map_err(|e| e.to_string())
    }

    fn unicode(&mut self) -> Result<char, String> {
        let high = self.hex4()?;
        let code = if (0xD800..0xDC00).contains(&high) {
            if self.s.get(self.i..self.i + 2) != Some(b"\\u".as_slice()) {
                return self.fail("half a surrogate pair");
            }
            self.i += 2;
            let low = self.hex4()?;
            if !(0xDC00..0xE000).contains(&low) {
                return self.fail("a bad surrogate pair");
            }
            0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00)
        } else {
            high
        };
        char::from_u32(code).ok_or_else(|| format!("JSON: bad code point {code:x}"))
    }

    fn string(&mut self) -> Result<String, String> {
        self.eat(b'"')?;
        let mut out = Vec::new();
        loop {
            let Some(c) = self.peek() else {
                return self.fail("a string with no end");
            };
            self.i += 1;
            match c {
                b'"' => break,
                b'\\' => {
                    let Some(e) = self.peek() else {
                        return self.fail("a string with no end");
                    };
                    self.i += 1;
                    let ch = match e {
                        b'"' => '"',
                        b'\\' => '\\',
                        b'/' => '/',
                        b'b' => '\u{8}',
                        b'f' => '\u{c}',
                        b'n' => '\n',
                        b'r' => '\r',
                        b't' => '\t',
                        b'u' => self.unicode()?,
                        _ => return self.fail("a bad escape"),
                    };
                    out.extend_from_slice(ch.encode_utf8(&mut [0u8; 4]).as_bytes());
                }
                _ => out.push(c),
            }
        }
        String::from_utf8(out).map_err(|e| format!("JSON: {e}"))
    }

    fn array(&mut self) -> Result<Json, String> {
        self.eat(b'[')?;
        let mut items = Vec::new();
        self.space();
        if self.peek() == Some(b']') {
            self.i += 1;
            return Ok(Json::Arr(items));
        }
        loop {
            items.push(self.value()?);
            self.space();
            match self.peek() {
                Some(b',') => self.i += 1,
                Some(b']') => {
                    self.i += 1;
                    return Ok(Json::Arr(items));
                }
                _ => return self.fail("expected ',' or ']'"),
            }
        }
    }

    fn object(&mut self) -> Result<Json, String> {
        self.eat(b'{')?;
        let mut fields = Vec::new();
        self.space();
        if self.peek() == Some(b'}') {
            self.i += 1;
            return Ok(Json::Obj(fields));
        }
        loop {
            self.space();
            let key = self.string()?;
            self.eat(b':')?;
            fields.push((key, self.value()?));
            self.space();
            match self.peek() {
                Some(b',') => self.i += 1,
                Some(b'}') => {
                    self.i += 1;
                    return Ok(Json::Obj(fields));
                }
                _ => return self.fail("expected ',' or '}'"),
            }
        }
    }
}

/// How the files carry 64-bit numbers (digests, seeds, f64 bit patterns) and data: as hex
pub fn unhex64(s: &str) -> Result<u64, String> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(format!("not a hex number: {s}"));
    }
    u64::from_str_radix(s, 16).map_err(|e| format!("{s}: {e}"))
}

pub fn hex64(v: u64) -> String {
    format!("{v:016x}")
}

pub fn hex_bytes(d: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(2 * d.len());
    for &b in d {
        s.push(char::from(DIGITS[usize::from(b >> 4)]));
        s.push(char::from(DIGITS[usize::from(b & 15)]));
    }
    s
}

pub fn unhex(s: &str) -> Result<Vec<u8>, String> {
    if s.len() % 2 != 0 || !s.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(format!("not hex bytes: {s}"));
    }
    (0..s.len() / 2).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).map_err(|e| e.to_string())).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_what_nlohmann_writes_and_writes_it_back() {
        let text = r#"{"twin":"derand_ccsds","n":[0,-1,18446744073709551615,-9223372036854775808],"e":{},"a":[],"t":true,"f":false,"z":null}"#;
        let j = parse(text).unwrap();
        assert_eq!(j.to_string(), text);
        assert_eq!(j.get("twin").unwrap().str().unwrap(), "derand_ccsds");
        let n = j.get("n").unwrap().arr().unwrap();
        assert_eq!(n[2].u64().unwrap(), u64::MAX);
        assert_eq!(n[3].i64().unwrap(), i64::MIN);
        assert!(n[1].u64().is_err());
        assert!(n[2].i64().is_err());
        assert!(j.has("e") && !j.has("missing"));
        assert!(j.get("missing").is_err());
        // Pretty-printed (case.json, expected.json) reads the same
        let pretty = parse("{\n  \"a\": [\n   1,\n   2\n  ]\n}\n").unwrap();
        assert_eq!(pretty.to_string(), r#"{"a":[1,2]}"#);
    }

    #[test]
    fn strings_with_escapes() {
        let j = parse(r#""a\"b\\c\/d\n\t\u00e9\ud83d\ude00\u0001""#).unwrap();
        assert_eq!(j.str().unwrap(), "a\"b\\c/d\n\té\u{1F600}\u{1}");
        assert_eq!(parse(&j.to_string()).unwrap(), j);
        assert_eq!(Json::from("x\u{1}").to_string(), r#""x\u0001""#);
    }

    #[test]
    fn refuses_what_is_not_json() {
        for bad in ["", "{", "[1,]", "{\"a\" 1}", "1.5", "1e3", "\"\\ud800\"", "\"\\q\"", "tru", "[1] 2", "\"abc"] {
            assert!(parse(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn hex_numbers_and_bytes() {
        assert_eq!(unhex64("ff"), Ok(255));
        assert!(unhex64("").is_err() && unhex64("+1").is_err() && unhex64("1g").is_err());
        assert!(unhex64("10000000000000000").is_err());
        assert_eq!(hex64(255), "00000000000000ff");
        assert_eq!(hex_bytes(&[0, 0xab, 0x10]), "00ab10");
        assert_eq!(unhex("00ab10"), Ok(vec![0, 0xab, 0x10]));
        assert!(unhex("abc").is_err() && unhex("zz").is_err());
    }
}
