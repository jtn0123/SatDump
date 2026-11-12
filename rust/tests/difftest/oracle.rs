//! Oracle files and reproducers, the layout of `tests/diff/io.cpp`: a case is made again from
//! its description (seed, fill style, corpus file, mutations) or read from the line (a
//! structured case's "input"), run through the Rust twin and compared with what the C++ gave;
//! the first differences are saved as reproducers that both `satdump-diff replay` and
//! `SD_DIFF_REPLAY=<folder> cargo test --test difftest` replay. A Rust panic is a difference
//! like any other.

use crate::json::{Json, hex64, parse, unhex, unhex64};
use crate::outcome::{FloatRule, Outcome, Value, compare, outcome_from_json, outcome_to_json};
use crate::rng::{FNV_OFFSET, Mutation, apply, fill, fnv1a};
use std::path::{Path, PathBuf};

/// A case as the Rust twins see it
pub struct Case {
    pub id: String,
    pub chunks: Vec<usize>,
    pub params: Vec<(String, i64)>,
    pub input: Vec<u8>,
}

impl Case {
    pub fn param(&self, name: &str, fallback: i64) -> i64 {
        self.params.iter().find(|(n, _)| n == name).map_or(fallback, |(_, v)| *v)
    }

    /// The pieces the input is fed in: its own chunks, or all of it at once
    pub fn chunks(&self) -> Vec<usize> {
        if self.chunks.is_empty() {
            return vec![self.input.len()];
        }
        let sum: usize = self.chunks.iter().sum();
        assert_eq!(sum, self.input.len(), "case {}: the chunks do not add up to the input", self.id);
        self.chunks.clone()
    }
}

/// A Rust twin: the case in, the outcome out
pub type Runner = fn(&Case) -> Outcome;

fn usize_of(j: &Json) -> Result<usize, String> {
    usize::try_from(j.u64()?).map_err(|e| e.to_string())
}

/// The case described by `j`; its bytes are `input` (a reproducer's input.bin), the line's own
/// "input" (with the mutations already made), or made again
fn case_from_json(j: &Json, folder: &Path, input: Option<Vec<u8>>) -> Result<Case, String> {
    let id = j.get("id")?.str()?.to_string();
    let mut mutations = Vec::new();
    for m in j.get("mutations")?.arr()? {
        let [op, a, b, c] = m.arr()? else {
            return Err(format!("case {id}: a mutation is [op, a, b, c]: {m}"));
        };
        mutations.push(Mutation { op: op.str()?.to_string(), a: a.u64()?, b: b.u64()?, c: c.u64()? });
    }
    let input = match input {
        Some(bytes) => bytes,
        None if j.has("input") => unhex(j.get("input")?.str()?)?,
        None => {
            let length = usize_of(j.get("length")?)?;
            let corpus = j.get("corpus")?.str()?;
            let mut bytes = if corpus.is_empty() {
                let seed = unhex64(j.get("seed")?.str()?)?;
                fill(seed, j.get("index")?.u64()?, length, j.get("style")?.u64()?)?
            } else {
                let data = std::fs::read(folder.join(corpus)).map_err(|e| format!("{corpus}: {e}"))?;
                let at = usize_of(j.get("at")?)?;
                let Some(rest) = data.get(at..) else {
                    return Err(format!("{corpus} is shorter than {at} bytes"));
                };
                rest.iter().take(length).copied().collect()
            };
            for m in &mutations {
                apply(&mut bytes, m)?;
            }
            bytes
        }
    };
    if input.len() as u64 != j.get("size")?.u64()? || hex64(fnv1a(&input, FNV_OFFSET)) != j.get("fnv")?.str()? {
        return Err(format!("case {id}: the case generator no longer makes the same bytes as the C++"));
    }
    let chunks = j.get("chunks")?.arr()?.iter().map(usize_of).collect::<Result<_, _>>()?;
    let mut params = Vec::new();
    for (name, v) in j.get("params")?.fields()? {
        params.push((name.clone(), v.i64()?));
    }
    Ok(Case { id, chunks, params, input })
}

fn floats_from_json(j: &Json) -> Result<FloatRule, String> {
    Ok(FloatRule { max_ulps: j.get("max_ulps")?.u64()? })
}

/// What a twin's runs came to
#[derive(Debug, Default)]
pub struct Report {
    pub cases: u64,
    pub differences: u64,
    pub saved: Vec<PathBuf>,
    pub first: String,
}

/// Where reproducers go and how many
pub struct Options {
    pub failures: PathBuf,
    pub max_saved: usize,
}

impl Options {
    /// SD_DIFF_FAILURES (else <target>/diff-failures) and SD_DIFF_MAX_SAVED (else 3)
    pub fn from_env() -> Options {
        let target = std::env::var_os("CARGO_TARGET_DIR")
            .map_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("target"), PathBuf::from);
        let failures = std::env::var_os("SD_DIFF_FAILURES").map_or_else(|| target.join("diff-failures"), PathBuf::from);
        let max_saved = std::env::var("SD_DIFF_MAX_SAVED").ok().and_then(|v| v.parse().ok()).unwrap_or(3);
        Options { failures, max_saved }
    }
}

fn safe_name(s: &str) -> String {
    s.chars().map(|c| if c.is_ascii_alphanumeric() || "-_.".contains(c) { c } else { '_' }).collect()
}

fn write(path: &Path, data: &[u8]) -> Result<(), String> {
    std::fs::write(path, data).map_err(|e| format!("cannot write {}: {e}", path.display()))
}

/// The command that replays a reproducer folder on the Rust side, from any folder
pub fn replay_command(dir: &Path) -> String {
    let manifest = Path::new(env!("CARGO_MANIFEST_DIR")).join("Cargo.toml");
    format!(
        "SD_DIFF_REPLAY={} cargo test --release --offline --manifest-path {} --test difftest replay_a_reproducer",
        dir.display(),
        manifest.display()
    )
}

/// The C++ replay command: `SD_DIFF_TOOL` (ctest sets it to the build's satdump-diff) or the
/// tool's name
fn cpp_replay_command(dir: &Path) -> String {
    let tool = std::env::var("SD_DIFF_TOOL").unwrap_or_else(|_| "<build>/satdump-diff".to_string());
    format!("{tool} replay {}", dir.display())
}

/// Runs the Rust twin; a panic comes back as an outcome that names it, so it is compared,
/// counted and saved like any other difference
fn run_twin(run: Runner, c: &Case) -> (Outcome, Option<String>) {
    match std::panic::catch_unwind(|| run(c)) {
        Ok(o) => (o, None),
        Err(payload) => {
            let what = payload
                .downcast_ref::<&str>()
                .map(|s| (*s).to_string())
                .or_else(|| payload.downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "a panic".to_string());
            let record = vec![("panic".to_string(), Value::Bytes(what.clone().into_bytes()))];
            (Outcome::new(i64::MIN, Vec::new(), vec![record]), Some(format!("Rust panicked: {what}")))
        }
    }
}

/// One oracle line the Rust twin did not match
struct Failure<'a> {
    twin: &'a str,
    line: &'a Json,
    input: &'a [u8],
    want: &'a Outcome,
    got: &'a Outcome,
    difference: &'a str,
    source: &'a str,
}

/// A reproducer of a Rust run that did not give the oracle's outcome, readable by
/// `satdump-diff replay` (case.json, input.bin, expected.json, actual.json)
fn save_reproducer(opt: &Options, f: &Failure<'_>) -> Result<PathBuf, String> {
    let case_json = f.line.get("case")?;
    let id = case_json.get("id")?.str()?;
    let dir = opt.failures.join(safe_name(&format!("{}-{id}-Rust", f.twin)));
    if dir.exists() {
        std::fs::remove_dir_all(&dir).map_err(|e| format!("cannot remove {}: {e}", dir.display()))?;
    }
    std::fs::create_dir_all(&dir).map_err(|e| format!("cannot make {}: {e}", dir.display()))?;
    write(&dir.join("input.bin"), f.input)?;
    let about = Json::object(vec![
        ("twin", Json::from(f.twin)),
        ("case", case_json.clone()),
        ("floats", f.line.get("floats")?.clone()),
        ("difference", Json::from(f.difference)),
        ("source", Json::from(f.source)),
        ("expected", Json::from("the oracle")),
        ("actual", Json::from("Rust")),
        ("replay", Json::from(cpp_replay_command(&dir).as_str())),
        ("replay_rust", Json::from(replay_command(&dir).as_str())),
    ]);
    write(&dir.join("case.json"), format!("{about}\n").as_bytes())?;
    write(&dir.join("expected.json"), format!("{}\n", outcome_to_json(f.want)).as_bytes())?;
    write(&dir.join("actual.json"), format!("{}\n", outcome_to_json(f.got)).as_bytes())?;
    Ok(dir)
}

/// Every line of an oracle file, or of each .jsonl file in a folder, through the Rust twin
/// `lookup` gives for the twin it names
pub fn replay_oracles(path: &Path, lookup: &dyn Fn(&str) -> Option<Runner>, opt: &Options) -> Result<Report, String> {
    let mut files = Vec::new();
    if path.is_dir() {
        for e in std::fs::read_dir(path).map_err(|e| format!("{}: {e}", path.display()))? {
            let p = e.map_err(|e| e.to_string())?.path();
            if p.extension().is_some_and(|x| x == "jsonl") {
                files.push(p);
            }
        }
        files.sort();
    } else {
        files.push(path.to_path_buf());
    }
    if files.is_empty() {
        return Err(format!("no .jsonl oracle files in {}", path.display()));
    }
    let mut r = Report::default();
    for file in &files {
        let text = std::fs::read_to_string(file).map_err(|e| format!("{}: {e}", file.display()))?;
        let folder = file.parent().unwrap_or(Path::new("."));
        let name = file.file_name().map_or_else(String::new, |n| n.to_string_lossy().into_owned());
        let (cases, differences) = (r.cases, r.differences);
        for (n, line) in text.lines().enumerate().filter(|(_, l)| !l.is_empty()) {
            let source = format!("oracle {name}:{}", n + 1);
            let j = parse(line).map_err(|e| format!("{source}: {e}"))?;
            let twin = j.get("twin")?.str()?;
            let run = lookup(twin).ok_or_else(|| format!("{source}: no Rust twin {twin} (rust/tests/difftest/main.rs)"))?;
            let c = case_from_json(j.get("case")?, folder, None).map_err(|e| format!("{source}: {e}"))?;
            let rule = floats_from_json(j.get("floats")?)?;
            let want = outcome_from_json(j.get("out")?).map_err(|e| format!("{source}: {e}"))?;
            r.cases += 1;
            let (got, panicked) = run_twin(run, &c);
            let d = panicked.unwrap_or_else(|| compare(&want, &got, rule, "the oracle", "Rust"));
            if d.is_empty() {
                continue;
            }
            r.differences += 1;
            if r.first.is_empty() {
                r.first = d.clone();
            }
            let mut where_ = String::new();
            if r.saved.len() < opt.max_saved {
                let failure = Failure { twin, line: &j, input: &c.input, want: &want, got: &got, difference: &d, source: &source };
                let dir = save_reproducer(opt, &failure)?;
                where_ = format!("\n      reproducer: {}\n      replay it: {}", dir.display(), replay_command(&dir));
                r.saved.push(dir);
            }
            if r.differences <= 10 {
                println!("diff: {twin} case {} ({source}): {d}{where_}", c.id);
            }
        }
        println!("diff: {}: {} cases, {} differences", file.display(), r.cases - cases, r.differences - differences);
    }
    Ok(r)
}

/// Runs a reproducer folder (from the C++ or from Rust) through the Rust twin and compares it
/// with the outcome saved there
pub fn replay_reproducer(dir: &Path, lookup: &dyn Fn(&str) -> Option<Runner>) -> Result<Report, String> {
    let read = |name: &str| std::fs::read(dir.join(name)).map_err(|e| format!("{}: {e}", dir.join(name).display()));
    let text = |name: &str| String::from_utf8(read(name)?).map_err(|e| e.to_string());
    let about = parse(&text("case.json")?)?;
    let twin = about.get("twin")?.str()?;
    let run = lookup(twin).ok_or_else(|| format!("no Rust twin {twin} (rust/tests/difftest/main.rs)"))?;
    let c = case_from_json(about.get("case")?, dir, Some(read("input.bin")?))?;
    let want = outcome_from_json(&parse(&text("expected.json")?)?)?;
    let wn = about.get("expected")?.str()?;
    let rule = floats_from_json(about.get("floats")?)?;
    let (got, panicked) = run_twin(run, &c);
    let d = panicked.unwrap_or_else(|| compare(&want, &got, rule, wn, "Rust"));
    let agree = format!("{wn} and Rust agree");
    println!("replay {}: {twin} case {}: {}", dir.display(), c.id, if d.is_empty() { &agree } else { &d });
    Ok(Report { cases: 1, differences: u64::from(!d.is_empty()), saved: Vec::new(), first: d })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(case: &str, out: &str) -> String {
        format!(r#"{{"twin":"echo","case":{case},"floats":{{"max_ulps":0,"reason":""}},"out":{out}}}"#)
    }

    fn echo(c: &Case) -> Outcome {
        Outcome::new(c.param("status", 0), c.input.clone(), Vec::new())
    }

    fn broken(c: &Case) -> Outcome {
        Outcome::new(0, c.input.iter().map(|b| b ^ 1).collect(), Vec::new())
    }

    #[test]
    fn a_case_is_made_again_from_its_description_and_checked() {
        let dir = std::env::temp_dir().join(format!("satdump-difftest-oracle-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        // fill(1, 2, 5, Zero) = 00 00 00 00 00, then byte 1 becomes 0xAB and the last is cut
        let input = [0u8, 0xAB, 0, 0];
        let fnv = hex64(fnv1a(&input, FNV_OFFSET));
        let case = format!(
            r#"{{"id":"c","seed":"0000000000000001","index":2,"length":5,"style":1,"corpus":"","at":0,"mutations":[["set",1,171,0],["truncate",4,0,0]],"chunks":[1,3],"params":{{"status":0}},"size":4,"fnv":"{fnv}"}}"#
        );
        let out = format!(r#"{{"status":0,"bytes":{{"bytes":4,"fnv":"{fnv}","hex":"00ab0000"}},"records":{{"count":0,"fnv":"cbf29ce484222325","list":[]}}}}"#);
        let file = dir.join("echo.jsonl");
        std::fs::write(&file, format!("{}\n\n{}\n", line(&case, &out), line(&case, &out))).unwrap();
        let opt = Options { failures: dir.join("failures"), max_saved: 1 };
        let (echo, broken): (Runner, Runner) = (echo, broken);
        let lookup = |t: &str| (t == "echo").then_some(echo);
        let r = replay_oracles(&dir, &lookup, &opt).unwrap();
        assert_eq!((r.cases, r.differences), (2, 0));

        // A twin that gives other bytes: found, saved once, and the replay shows it again
        let wrong = |t: &str| (t == "echo").then_some(broken);
        let r = replay_oracles(&file, &wrong, &opt).unwrap();
        assert_eq!((r.cases, r.differences, r.saved.len()), (2, 2, 1));
        assert!(r.first.starts_with("bytes: first difference at byte 0 of 4: the oracle 0x00, Rust 0x01"), "{}", r.first);
        assert_eq!(std::fs::read(r.saved[0].join("input.bin")).unwrap(), input);
        assert_eq!(replay_reproducer(&r.saved[0], &wrong).unwrap().differences, 1);
        assert_eq!(replay_reproducer(&r.saved[0], &lookup).unwrap().differences, 0);

        // A case that carries its bytes ("input") is read, not made again
        let carried = case.replace(r#""size":4"#, r#""input":"00ab0000","size":4"#).replace(r#""style":1"#, r#""style":0"#);
        std::fs::write(&file, line(&carried, &out)).unwrap();
        assert_eq!(replay_oracles(&file, &lookup, &opt).map(|r| r.differences), Ok(0));
        std::fs::write(&file, line(&carried.replace("00ab0000", "00ac0000"), &out)).unwrap();
        assert!(replay_oracles(&file, &lookup, &opt).unwrap_err().contains("no longer makes the same bytes"));

        // A panicking twin is a difference that names the panic, saved and replayed
        std::fs::write(&file, line(&case, &out)).unwrap();
        let panics: Runner = |_| panic!("on purpose");
        let r = replay_oracles(&file, &|t: &str| (t == "echo").then_some(panics), &opt).unwrap();
        assert_eq!((r.differences, r.first.as_str()), (1, "Rust panicked: on purpose"));
        assert_eq!(replay_reproducer(&r.saved[0], &|t: &str| (t == "echo").then_some(panics)).unwrap().differences, 1);
        let about = std::fs::read_to_string(r.saved[0].join("case.json")).unwrap();
        assert!(about.contains("--manifest-path /"), "{about}");

        // A case whose bytes no longer match its digest, and a twin with no Rust side, are errors
        std::fs::write(&file, line(&case.replace("\"size\":4", "\"size\":5"), &out)).unwrap();
        assert!(replay_oracles(&file, &lookup, &opt).unwrap_err().contains("no longer makes the same bytes"));
        std::fs::write(&file, line(&case, &out)).unwrap();
        let none = |_: &str| -> Option<Runner> { None };
        assert!(replay_oracles(&file, &none, &opt).unwrap_err().contains("no Rust twin echo"));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
