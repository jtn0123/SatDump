//! Fork: the Rust half of the differential tests (`tests/diff/diff.h`). Without the C++, it
//! replays the JSONL oracles the C++ twins wrote and checks that the Rust code gives the same:
//!
//! - the small oracles committed in `tests/diff/oracles` (always);
//! - with `SD_DIFF_ORACLES=<folder>`, those of a build's full plan (`satdump-diff oracle all
//!   --out <folder>`; ctest `diff_rust_replay` does this);
//! - with `SD_DIFF_REPLAY=<folder>`, one reproducer, from either side: `SD_DIFF_REPLAY=<folder>
//!   cargo test --release --manifest-path rust/Cargo.toml --test difftest replay_a_reproducer`.
//!
//! Every twin in `tests/diff` has its Rust runner in [`twin`].

mod json;
mod oracle;
mod outcome;
mod rng;
mod toys;
// Fork part "aos"
mod aos;

use oracle::{Case, Options, Report, Runner, replay_oracles, replay_reproducer};
use outcome::{Outcome, Value};
use satdump_rs::ffi;
use std::path::{Path, PathBuf};

/// The derandomisers' buffers, as `tests/diff/twins_derand.cpp`: `offset` bytes of 0xA5 (so
/// the data is not aligned), the input, then 8 bytes of 0x5A. The outcome is the whole buffer
/// and the status the C function returns.
fn derand<T>(c: &Case, f: unsafe extern "C" fn(*mut T, i32) -> i32) -> Outcome {
    let offset = usize::try_from(c.param("offset", 0)).expect("a buffer offset");
    let real = i64::try_from(c.input.len()).expect("an input length");
    let length = i32::try_from(c.param("length", real)).expect("a length that fits in an int");
    // The function may touch `length` bytes, so a case may only give a negative length instead
    assert!(length < 0 || i64::from(length) == real, "case {}: a length that is not the input's", c.id);
    let mut buf = vec![0xA5u8; offset];
    buf.extend_from_slice(&c.input);
    buf.extend_from_slice(&[0x5Au8; 8]);
    // SAFETY: buf holds offset + input + 8 bytes and T is one byte, so from `offset` on there
    // are at least `length` writable values when the length is not negative
    let status = unsafe { f(buf.as_mut_ptr().add(offset).cast::<T>(), length) };
    Outcome::new(i64::from(status), buf, Vec::new())
}

fn derand_bytes(c: &Case) -> Outcome {
    derand::<u8>(c, ffi::satdump_rs_derand_ccsds)
}

fn derand_soft(c: &Case) -> Outcome {
    derand::<i8>(c, ffi::satdump_rs_derand_ccsds_soft)
}

fn derand_bits(c: &Case) -> Outcome {
    derand::<u8>(c, ffi::satdump_rs_derand_ccsds_bits)
}

/// The Rust twin of each twin in `tests/diff` (`satdump-diff list`)
fn twin(name: &str) -> Option<Runner> {
    let run: Runner = match name {
        "derand_ccsds" => derand_bytes,
        "derand_ccsds_soft" => derand_soft,
        "derand_ccsds_bits" => derand_bits,
        "selftest_splitter" => toys::splitter,
        // The contracted twin's oracle is the reference's outcome with a float tolerance
        "selftest_lowpass" | "selftest_lowpass_contracted" => toys::lowpass,
        "selftest_gapfill" => toys::gapfill,
        // Fork part "aos"
        "aos_demux" => aos::demux,
        _ => return None,
    };
    Some(run)
}

/// The oracles committed with the code (`satdump-diff oracle <twin> --small --out ...`)
fn committed() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../tests/diff/oracles")
}

const COMMITTED: [&str; 6] =
    ["derand_ccsds", "derand_ccsds_soft", "derand_ccsds_bits", "selftest_splitter", "selftest_lowpass", "selftest_gapfill"];

/// Every twin with a committed oracle: [`COMMITTED`] and those of the ported parts
fn committed_twins() -> Vec<&'static str> {
    let mut twins = COMMITTED.to_vec();
    // Fork part "aos"
    twins.push("aos_demux");
    twins
}

/// The fewest lines each twin's oracle file may have (a small plan has at least 6 per twin), so
/// an emptied or cut file cannot pass with nothing checked
const MIN_CASES: u64 = 6;

/// Non-empty lines in the .jsonl files of a folder
fn oracle_lines(dir: &Path) -> u64 {
    let mut n = 0;
    for e in std::fs::read_dir(dir).unwrap_or_else(|e| panic!("{}: {e}", dir.display())) {
        let p = e.expect("a folder entry").path();
        if p.extension().is_some_and(|x| x == "jsonl") {
            let text = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display()));
            n += text.lines().filter(|l| !l.is_empty()).count() as u64;
        }
    }
    n
}

fn replay_all(dir: &Path, twins: &[&str]) -> Report {
    let opt = Options::from_env();
    let mut all = Report::default();
    for t in twins {
        let file = dir.join(format!("{t}.jsonl"));
        assert!(file.is_file(), "{} is missing", file.display());
        let r = replay_oracles(&file, &twin, &opt).unwrap_or_else(|e| panic!("{e}"));
        assert!(r.cases >= MIN_CASES, "{}: {} cases, fewer than {MIN_CASES}", file.display(), r.cases);
        assert_eq!(r.differences, 0, "Rust differs from the C++ oracle of {t}; the first: {}", r.first);
        all.cases += r.cases;
    }
    assert_eq!(all.cases, oracle_lines(dir), "every oracle file in {} is one of {twins:?}", dir.display());
    all
}

#[test]
fn the_committed_oracles_replay_in_rust() {
    replay_all(&committed(), &committed_twins());
}

#[test]
fn a_builds_full_oracles_replay_in_rust() {
    let Some(dir) = std::env::var_os("SD_DIFF_ORACLES") else {
        println!("SD_DIFF_ORACLES is not set; ctest diff_rust_replay sets it to the oracles of the build's full plan");
        return;
    };
    let mut twins = committed_twins();
    twins.push("selftest_lowpass_contracted");
    let r = replay_all(Path::new(&dir), &twins);
    println!("replayed {} oracle lines from {}", r.cases, Path::new(&dir).display());
}

#[test]
fn replay_a_reproducer() {
    let Some(dir) = std::env::var_os("SD_DIFF_REPLAY") else {
        println!("SD_DIFF_REPLAY is not set; set it to a reproducer folder to replay that case");
        return;
    };
    let r = replay_reproducer(Path::new(&dir), &twin).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(r.differences, 0, "{}", r.first);
}

/// The real byte derandomiser with one output bit flipped for inputs of 1020 bytes (a CADU
/// after its sync marker): the deliberate fault of the self-test below
fn flipped(c: &Case) -> Outcome {
    let mut o = derand_bytes(c);
    if let (1020, 0, Value::Bytes(b)) = (c.input.len(), o.status, &mut o.bytes) {
        b[usize::try_from(c.param("offset", 0)).expect("a buffer offset") + 100] ^= 0x01;
    }
    o
}

fn temp_failures(what: &str) -> Options {
    Options { failures: std::env::temp_dir().join(format!("satdump-difftest-{what}-{}", std::process::id())), max_saved: 3 }
}

#[test]
fn a_flipped_bit_is_found_saved_and_replayed() {
    let opt = temp_failures("flipped");
    let flipped: Runner = flipped;
    let mutant = |name: &str| if name == "derand_ccsds" { Some(flipped) } else { twin(name) };
    let r = replay_oracles(&committed().join("derand_ccsds.jsonl"), &mutant, &opt).unwrap_or_else(|e| panic!("{e}"));
    // The small plan has one case of 1020 bytes: edge length 1020 at offset 3
    assert_eq!((r.differences, r.saved.len()), (1, 1), "{}", r.first);
    assert!(r.first.starts_with("bytes: first difference at byte 103 of 1031: the oracle 0x"), "{}", r.first);
    for file in ["input.bin", "case.json", "expected.json", "actual.json"] {
        assert!(r.saved[0].join(file).is_file(), "{file}");
    }
    assert_eq!(std::fs::metadata(r.saved[0].join("input.bin")).map(|m| m.len()).ok(), Some(1020));

    // The replay shows it again with the changed twin, and not with the real one
    let again = replay_reproducer(&r.saved[0], &mutant).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!((again.differences, again.first.as_str()), (1, r.first.as_str()));
    assert_eq!(replay_reproducer(&r.saved[0], &twin).map(|r| r.differences), Ok(0));
    std::fs::remove_dir_all(&opt.failures).expect("the test's own folder");
}

#[test]
fn a_fused_multiply_add_changes_pixels_and_the_oracle_finds_it() {
    // The float rule (tests/diff/diff.h): the C++ the oracle came from is built with
    // -ffp-contract=off, so a Rust port that fuses where it does not gets other integers
    let opt = temp_failures("fused");
    let fused: Runner = toys::gapfill_fused;
    let mutant = |name: &str| if name == "selftest_gapfill" { Some(fused) } else { twin(name) };
    let r = replay_oracles(&committed().join("selftest_gapfill.jsonl"), &mutant, &opt).unwrap_or_else(|e| panic!("{e}"));
    assert!(r.differences > 0);
    assert!(r.first.contains(" pixels: first difference at byte "), "{}", r.first);
    assert_eq!(replay_reproducer(&r.saved[0], &mutant).map(|r| r.differences), Ok(1));
    assert_eq!(replay_reproducer(&r.saved[0], &twin).map(|r| r.differences), Ok(0));
    std::fs::remove_dir_all(&opt.failures).expect("the test's own folder");
}

#[test]
fn a_chunk_boundary_fault_is_found_from_the_oracle_alone() {
    // The oracle lines carry each case's chunks, so a fault that shows only when a chunk ends
    // in the wrong place is found without the C++
    let opt = temp_failures("boundary");
    let buggy: Runner = toys::splitter_with_boundary_bug;
    let mutant = |name: &str| if name == "selftest_splitter" { Some(buggy) } else { twin(name) };
    let r = replay_oracles(&committed().join("selftest_splitter.jsonl"), &mutant, &opt).unwrap_or_else(|e| panic!("{e}"));
    assert!(r.differences > 0);
    assert_eq!(replay_reproducer(&r.saved[0], &mutant).map(|r| r.differences), Ok(1));
    assert_eq!(replay_reproducer(&r.saved[0], &twin).map(|r| r.differences), Ok(0));
    std::fs::remove_dir_all(&opt.failures).expect("the test's own folder");
}
