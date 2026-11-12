//! The Rust demuxer on its own. The C++ unit tests (`tests/unit/test_aos_demuxer.cpp`) check the
//! same cases on the C++ demuxer; the differential tests compare the two on many more.

use super::*;

/// MetOp / Meteor layout: 2-byte insert zone, 882-byte packet zone, 1024-byte CADUs
const ZONE: usize = 882;
const IDLE_APID: u16 = 2047;

fn metop(counter: bool, strict: bool) -> Settings {
    Settings::new(882, true, 2, false, counter, strict).expect("a layout SatDump uses")
}

/// A space packet without secondary header: the header and `len` payload bytes
fn packet(apid: u16, seq: u16, len: usize) -> Vec<u8> {
    let length = u16::try_from(len - 1).expect("short enough");
    let mut p = vec![(apid >> 8) as u8 & 7, apid as u8, 0xC0 | (seq >> 8) as u8 & 0x3F, seq as u8];
    p.extend_from_slice(&length.to_be_bytes());
    p.extend((0..len).map(|i| (usize::from(seq) * 31 + i * 7 + 1) as u8));
    p
}

/// Packets laid end to end and cut into frames, as `Channel` in the C++ tests
#[derive(Default)]
struct Channel {
    stream: Vec<u8>,
    starts: Vec<usize>,
    packets: Vec<Vec<u8>>,
}

/// One frame before it is made into a CADU
#[derive(Clone)]
struct Frame {
    counter: u32,
    fhp: u16,
    zone: Vec<u8>,
}

impl Frame {
    fn cadu(&self) -> Vec<u8> {
        let id: u16 = (1 << 14) | (11 << 6) | 9;
        let c = self.counter.to_be_bytes();
        let mut out = vec![0x1A, 0xCF, 0xFC, 0x1D, (id >> 8) as u8, id as u8, c[1], c[2], c[3], 0, 0, 0];
        out.extend_from_slice(&self.fhp.to_be_bytes());
        out.extend_from_slice(&self.zone);
        out.resize(1024, 0);
        out
    }
}

impl Channel {
    fn add(&mut self, len: usize) -> usize {
        let seq = u16::try_from(self.packets.len()).expect("few packets");
        self.add_packet(packet(100, seq, len))
    }

    fn add_packet(&mut self, p: Vec<u8>) -> usize {
        self.starts.push(self.stream.len());
        self.stream.extend_from_slice(&p);
        self.packets.push(p);
        self.packets.len() - 1
    }

    /// A packet that ends where the next one should start: byte `pos` of the stream
    fn add_until(&mut self, pos: usize) -> usize {
        assert!(pos >= self.stream.len() + 7);
        self.add(pos - self.stream.len() - 6)
    }

    fn next_frame_end(&self) -> usize {
        (self.stream.len() / ZONE + 2) * ZONE
    }

    /// Ends the stream with an idle packet that fills the last frame and cuts it into frames
    fn frames(&mut self, first_counter: u32) -> Vec<Frame> {
        let pad = ZONE - self.stream.len() % ZONE;
        self.starts.push(self.stream.len());
        let idle = packet(IDLE_APID, 0, if pad >= 7 { pad } else { pad + ZONE } - 6);
        self.stream.extend_from_slice(&idle);
        let mut out = Vec::new();
        for (n, pos) in (0..self.stream.len()).step_by(ZONE).enumerate() {
            let first = self.starts.iter().find(|&&s| (pos..pos + ZONE).contains(&s));
            out.push(Frame {
                counter: (first_counter + u32::try_from(n).expect("few frames")) & 0xFF_FFFF,
                fhp: first.map_or(2047, |s| u16::try_from(s - pos).expect("in the zone")),
                zone: self.stream[pos..pos + ZONE].to_vec(),
            });
        }
        out
    }

    fn sent(&self, lost: &[usize]) -> Vec<Vec<u8>> {
        (0..self.packets.len()).filter(|i| !lost.contains(i)).map(|i| self.packets[i].clone()).collect()
    }
}

/// What comes out (header and payload), without idle packets; every packet must be whole
fn run(s: Settings, frames: &[Frame]) -> Vec<Vec<u8>> {
    let mut d = Demuxer::new(s);
    let mut out = Vec::new();
    for f in frames {
        for p in d.work(&f.cadu()) {
            assert_eq!(p.payload.len(), usize::from(u16::from_be_bytes([p.header[4], p.header[5]])) + 1);
            if ((u16::from(p.header[0] & 7) << 8) | u16::from(p.header[1])) == IDLE_APID {
                continue;
            }
            let mut b = p.header.to_vec();
            b.extend_from_slice(&p.payload);
            out.push(b);
        }
    }
    out
}

/// Every combination of the frame counter check and strict mode
fn each_mode(mut check: impl FnMut(Settings)) {
    for counter in [false, true] {
        for strict in [false, true] {
            check(metop(counter, strict));
        }
    }
}

#[test]
fn intact_frames_give_exactly_the_packets_sent() {
    let mut ch = Channel::default();
    ch.add(100);
    ch.add(200);
    ch.add(1);
    for k in 1..=6 {
        let end = ch.next_frame_end();
        ch.add_until(end - k);
        ch.add(40 + 50 * k);
    }
    ch.add(2000);
    let end = ch.next_frame_end();
    ch.add_until(end + 300);
    ch.add(ZONE - 300 - 6);
    ch.add(100);
    ch.add(ZONE - 106 - 6);
    let end = ch.next_frame_end();
    ch.add_until(end - 2);
    ch.add(ZONE - 4);
    let mut x: u32 = 1;
    for i in 0..300 {
        x = x.wrapping_mul(1_103_515_245).wrapping_add(12345);
        ch.add(1 + (x >> 16) as usize % if i % 3 == 0 { 1500 } else { 120 });
    }
    let frames = ch.frames(0);
    each_mode(|s| assert_eq!(run(s, &frames), ch.sent(&[])));
}

#[test]
fn a_header_cut_across_two_frames_at_each_place() {
    for k in 1..=6 {
        let mut ch = Channel::default();
        ch.add_until(ZONE - k);
        ch.add(300);
        ch.add(50);
        let frames = ch.frames(0);
        assert_eq!(frames[0].fhp, 0);
        assert_eq!(usize::from(frames[1].fhp), 300 + 6 - k);
        each_mode(|s| assert_eq!(run(s, &frames), ch.sent(&[]), "k = {k}"));
    }
}

#[test]
fn a_packet_over_many_frames_and_the_longest_packet() {
    let mut ch = Channel::default();
    ch.add(500);
    ch.add(3000);
    ch.add(65536); // packet length 65535: the longest a header can say
    ch.add(10);
    let frames = ch.frames(0);
    each_mode(|s| assert_eq!(run(s, &frames), ch.sent(&[])));
}

#[test]
fn the_frame_counter_wraps_without_a_false_gap() {
    let mut ch = Channel::default();
    ch.add(1500);
    ch.add(1500);
    ch.add(20);
    let frames = ch.frames(0xFF_FFFE);
    assert_eq!(frames[2].counter, 0);
    each_mode(|s| assert_eq!(run(s, &frames), ch.sent(&[])));
}

#[test]
fn a_lost_frame_drops_the_packet_in_progress_when_the_counter_is_followed() {
    let mut ch = Channel::default();
    ch.add(294);
    let p = ch.add(994); // to byte 418 of frame 1
    let q = ch.add(1994); // from there over all of frame 2
    ch.add(94);
    ch.add(494);
    ch.add(50);
    let all = ch.frames(0);
    assert_eq!((all[1].fhp, all[2].fhp), (418, 2047));
    let mut frames = all.clone();
    frames.remove(1);
    for strict in [false, true] {
        assert_eq!(run(metop(true, strict), &frames), ch.sent(&[p, q]));
    }
    // Without the check, P is finished with bytes from the middle of Q, and still comes out
    let out = run(metop(false, false), &frames);
    assert_eq!(out.len(), ch.packets.len() - 1);
    assert_ne!(out[1], ch.packets[p]);

    // A repeated frame in the middle of Q is a gap for the counter check. Without it, Q gets
    // the same bytes twice and comes out wrong; strict mode drops it, as Q then ends inside a
    // frame where no packet starts (rule 2)
    let mut frames = all.clone();
    frames.insert(3, all[2].clone());
    for strict in [false, true] {
        assert_eq!(run(metop(true, strict), &frames), ch.sent(&[q]));
    }
    let out = run(metop(false, false), &frames);
    assert_eq!(out.len(), ch.packets.len());
    assert_ne!(out[2], ch.packets[q]);
    assert_eq!(run(metop(false, true), &frames), ch.sent(&[q]));
}

#[test]
fn strict_mode_drops_a_packet_that_ends_before_the_next_header() {
    // P's header says 10 bytes fewer than it has; the next frame's pointer shows where it ends
    let mut ch = Channel::default();
    ch.add(294);
    let p = ch.add(994);
    ch.add(300);
    let mut frames = ch.frames(0);
    assert_eq!(frames[1].fhp, 418);
    // P's header is at byte 300 of frame 0, its length at 304 and 305
    let length = u16::from_be_bytes([frames[0].zone[304], frames[0].zone[305]]) - 10;
    frames[0].zone[304..306].copy_from_slice(&length.to_be_bytes());
    let normal = run(metop(true, false), &frames);
    let strict = run(metop(true, true), &frames);
    assert_eq!(normal.len(), 3);
    assert_eq!(normal[1].len(), 6 + 984);
    assert_eq!(strict, ch.sent(&[p]));
}

#[test]
fn strict_mode_drops_a_packet_that_ends_inside_a_frame_where_no_packet_starts() {
    // P ends at byte 418 of frame 1, where Q starts; frame 1 wrongly says no packet starts
    let mut ch = Channel::default();
    ch.add(294);
    let p = ch.add(994);
    let q = ch.add(1994);
    ch.add(94);
    let mut frames = ch.frames(0);
    frames[1].fhp = 2047;
    let normal = run(metop(false, false), &frames);
    assert_eq!(normal, ch.sent(&[q]));
    assert_eq!(run(metop(false, true), &frames), ch.sent(&[p, q]));
}

#[test]
fn strict_mode_keeps_a_packet_that_fills_a_frame_where_no_packet_starts() {
    // Q fills frame 2 to the end and goes on: rule 2 is met
    let mut ch = Channel::default();
    ch.add(294);
    ch.add(994);
    ch.add(1994);
    ch.add(94);
    let frames = ch.frames(0);
    assert_eq!(frames[2].fhp, 2047);
    each_mode(|s| assert_eq!(run(s, &frames), ch.sent(&[])));

    // P ends exactly at the end of frame 1, where no packet starts: rule 2 is met too
    let mut ch = Channel::default();
    ch.add(294);
    ch.add_until(2 * ZONE);
    ch.add(94);
    let frames = ch.frames(0);
    assert_eq!((frames[1].fhp, frames[2].fhp), (2047, 0));
    each_mode(|s| assert_eq!(run(s, &frames), ch.sent(&[])));
}

#[test]
fn a_pointer_outside_the_packet_zone_counts_as_a_lost_frame() {
    let mut ch = Channel::default();
    ch.add(294);
    let p = ch.add(994);
    let q = ch.add(1994);
    ch.add(94);
    let mut frames = ch.frames(0);
    for fhp in [882, 1000, 2046] {
        frames[1].fhp = fhp;
        each_mode(|s| assert_eq!(run(s, &frames), ch.sent(&[p, q]), "pointer {fhp}"));
    }
}

#[test]
fn the_secondary_header_adds_8_bytes_when_asked() {
    let s = Settings::new(882, true, 2, true, false, false).expect("a layout SatDump uses");
    let mut ch = Channel::default();
    let mut p = packet(100, 0, 108);
    p[0] |= 0x08; // secondary header flag: the header says 100 bytes, 108 follow
    p[4..6].copy_from_slice(&99u16.to_be_bytes());
    ch.add_packet(p);
    ch.add(50);
    let frames = ch.frames(0);
    let mut d = Demuxer::new(s);
    let got: Vec<usize> = frames.iter().flat_map(|f| d.work(&f.cadu()).iter().map(|p| p.payload.len()).collect::<Vec<_>>()).collect();
    assert_eq!(&got[..2], &[108, 50]);
    let without = run(metop(false, false), &frames);
    assert_eq!(without[0].len(), 6 + 100);
}

#[test]
fn settings_outside_the_supported_range_are_refused() {
    assert!(Settings::new(5, false, 2, false, false, false).is_none());
    assert!(Settings::new(0, false, 2, false, false, false).is_none());
    assert!(Settings::new(-1, false, 2, false, false, false).is_none());
    assert!(Settings::new(65537, false, 2, false, false, false).is_none());
    assert!(Settings::new(884, true, -1, false, false, false).is_none());
    assert!(Settings::new(884, true, 65537, false, false, false).is_none());
    // Without an insert zone its size does not matter, as in the C++
    let s = Settings::new(884, false, -1, false, false, false).expect("in range");
    assert_eq!((s.insert_zone_size, s.cadu_bytes()), (0, 896));
    let s = Settings::new(6, true, 65536, false, false, false).expect("in range");
    assert_eq!(s.cadu_bytes(), 10 + 65536 + 2 + 6);
    assert_eq!(metop(false, false).cadu_bytes(), 896);
}

#[test]
fn any_bytes_give_only_whole_packets() {
    // Damaged frames of every kind: each packet that comes out is as long as its header says
    let mut x: u64 = 0x9E37_79B9_7F4A_7C15;
    let mut next = move || {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x
    };
    for (mpdu, iz, ext) in [(882, 2, false), (884, 0, true), (6, 0, false), (7, 9, true)] {
        for strict in [false, true] {
            let s = Settings::new(mpdu, iz > 0, iz, ext, true, strict).expect("in range");
            let mut d = Demuxer::new(s);
            let mut cadu = vec![0u8; s.cadu_bytes()];
            for _ in 0..3000 {
                cadu.iter_mut().for_each(|b| *b = next() as u8);
                // Small pointers and lengths, so packets start and end inside the frames
                let h = s.mpdu_header_at();
                cadu[h] &= if next() % 4 == 0 { 0x07 } else { 0x00 };
                cadu[h + 1] %= u8::try_from(mpdu.min(255)).expect("small");
                for p in d.work(&cadu) {
                    let length = usize::from(u16::from_be_bytes([p.header[4], p.header[5]])) + 1;
                    let extra = if ext && p.header[0] & 0x08 != 0 { 8 } else { 0 };
                    assert_eq!(p.payload.len(), length + extra);
                }
            }
        }
    }
}
