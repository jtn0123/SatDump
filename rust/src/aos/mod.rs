//! CCSDS AOS demuxer: the space packets carried in the M_PDUs of one virtual channel, CADU by
//! CADU. It is the twin of the C++ `ccsds::ccsds_aos::Demuxer`
//! (`src-core/common/ccsds/ccsds_aos/demuxer.cpp`) and follows that code step by step, so both
//! give the same packets, in the same CADUs, for every input, damaged ones included. The
//! differential tests (`tests/diff/twins_aos.cpp`) compare the two.
//!
//! Strict mode (off by default, as in the C++) is described in `demuxer.h`: a packet that runs
//! on from an earlier frame must end exactly where the frame it ends in says the next packet
//! starts (its first header pointer), or at the end of that frame's packet zone when no packet
//! starts in it; otherwise it is dropped.

#[cfg(test)]
mod tests;

/// A space packet header is 6 bytes
const HEADER: i64 = 6;
/// First header pointer value: no packet starts in this M_PDU
const NO_HEADER: i64 = 2047;

/// The smallest packet zone the Rust demuxer takes: a whole header can then be read from the
/// start of a zone (SatDump uses 880 to 2034 bytes)
pub const MIN_MPDU_DATA_SIZE: i32 = 6;
/// The largest packet zone the Rust demuxer takes
pub const MAX_MPDU_DATA_SIZE: i32 = 65536;
/// The largest insert zone the Rust demuxer takes (SatDump uses 0, 2 and 9 bytes)
pub const MAX_INSERT_ZONE_SIZE: i32 = 65536;

/// How a virtual channel's frames are laid out, as the C++ constructor takes it
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Settings {
    /// Bytes in the M_PDU packet zone
    pub mpdu_data_size: usize,
    /// Bytes in the VCDU insert zone (0 when there is none)
    pub insert_zone_size: usize,
    /// A packet with its secondary header flag set has 8 more payload bytes than its length says
    pub secondary_header_extends_pkt: bool,
    /// Follow the 24-bit frame counter and drop the packet in progress when frames are missing
    pub check_frame_counter: bool,
    /// Strict mode (see the module documentation)
    pub strict: bool,
}

impl Settings {
    /// The settings in the C++ constructor's terms, or `None` when the Rust demuxer does not take
    /// them (a packet zone under 6 bytes or over 65536, an insert zone below 0 or over 65536); for
    /// those SatDump uses its C++ demuxer
    pub fn new(
        mpdu_data_size: i32,
        has_insert_zone: bool,
        insert_zone_size: i32,
        secondary_header_extends_pkt: bool,
        check_frame_counter: bool,
        strict: bool,
    ) -> Option<Settings> {
        if !(MIN_MPDU_DATA_SIZE..=MAX_MPDU_DATA_SIZE).contains(&mpdu_data_size) {
            return None;
        }
        // Without an insert zone its size is not used, as in the C++
        let insert_zone = if has_insert_zone { insert_zone_size } else { 0 };
        if !(0..=MAX_INSERT_ZONE_SIZE).contains(&insert_zone) {
            return None;
        }
        Some(Settings {
            mpdu_data_size: usize::try_from(mpdu_data_size).ok()?,
            insert_zone_size: usize::try_from(insert_zone).ok()?,
            secondary_header_extends_pkt,
            check_frame_counter,
            strict,
        })
    }

    /// Where the M_PDU header is: after the sync marker, the VCDU header and the insert zone
    fn mpdu_header_at(&self) -> usize {
        10 + self.insert_zone_size
    }

    /// The bytes of a CADU the demuxer reads: up to the end of the packet zone
    pub fn cadu_bytes(&self) -> usize {
        self.mpdu_header_at() + 2 + self.mpdu_data_size
    }
}

/// A space packet: its 6-byte header as received, and its payload
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Packet {
    /// The header bytes
    pub header: [u8; 6],
    /// Everything after the header
    pub payload: Vec<u8>,
}

/// A position or length in a packet zone; the demuxer never makes a negative one
fn at(v: i64) -> usize {
    usize::try_from(v).expect("a position in the packet zone is never negative")
}

/// The demuxer of one virtual channel (frames of other channels must be filtered out first)
pub struct Demuxer {
    s: Settings,
    /// The packet being put together (C++ `currentCCSDSPacket`)
    current: Packet,
    /// Its payload length from its header, its length with the header, and what is still missing
    payload_length: i64,
    total_length: i64,
    remaining: i64,
    /// A packet has been started (C++ `workingOnPacket`)
    working: bool,
    /// The start of a header was at the end of the last frame (C++ `inHeader`)
    in_header: bool,
    header_buffer: [u8; 6],
    in_header_buffer: usize,
    /// The frame counter of the last CADU, when following it
    last_counter: Option<u32>,
    /// The packets the last CADU finished
    out: Vec<Packet>,
}

impl Demuxer {
    /// A demuxer with nothing in progress
    pub fn new(s: Settings) -> Demuxer {
        Demuxer {
            s,
            current: Packet::default(),
            payload_length: 0,
            total_length: 0,
            remaining: 0,
            working: false,
            in_header: false,
            header_buffer: [0; 6],
            in_header_buffer: 0,
            last_counter: None,
            out: Vec::new(),
        }
    }

    /// The settings it was made with
    pub fn settings(&self) -> &Settings {
        &self.s
    }

    /// Starts a packet from its header (C++ `readPacket`)
    fn read_packet(&mut self, h: [u8; 6]) {
        self.working = true;
        self.current.payload.clear();
        self.current.header = h;
        let packet_length = i64::from(u16::from_be_bytes([h[4], h[5]]));
        let secondary_header_flag = (h[0] >> 3) & 1 == 1;
        let extra = if self.s.secondary_header_extends_pkt && secondary_header_flag { 8 } else { 0 };
        self.payload_length = packet_length + 1 + extra;
        self.total_length = self.payload_length + HEADER;
        self.remaining = self.payload_length;
    }

    /// The packet is finished (C++ `pushPacket`)
    fn push_packet(&mut self) {
        let payload = std::mem::take(&mut self.current.payload);
        self.out.push(Packet { header: self.current.header, payload });
        self.payload_length = 0;
        self.remaining = 0;
        self.working = false;
    }

    /// The packet is dropped (C++ `abortPacket`)
    fn abort_packet(&mut self) {
        self.working = false;
        self.current.payload.clear();
        self.payload_length = 0;
        self.remaining = 0;
    }

    /// Adds bytes to the packet (C++ `pushPayload`)
    fn push_payload(&mut self, bytes: &[u8]) {
        self.current.payload.extend_from_slice(bytes);
        self.remaining -= i64::try_from(bytes.len()).expect("a packet zone is under 2^63 bytes");
    }

    /// The first bytes of a header cut by the end of the zone are kept for the next frame
    fn keep_header_start(&mut self, data: &[u8], from: usize) {
        self.in_header = true;
        let part = &data[from..];
        self.header_buffer[..part.len()].copy_from_slice(part);
        self.in_header_buffer = part.len();
    }

    /// The packets that end in this CADU (C++ `work`). `cadu` starts at the sync marker and
    /// must have at least [`Settings::cadu_bytes`] bytes.
    pub fn work(&mut self, cadu: &[u8]) -> &[Packet] {
        self.out.clear();
        let mpdu = i64::try_from(self.s.mpdu_data_size).expect("checked by Settings::new");

        // A packet that ended exactly at the end of the last frame is finished first
        if self.remaining == 0 && self.working {
            self.push_packet();
        }

        // Frames missing from the virtual channel: drop the packet in progress
        if self.s.check_frame_counter {
            let counter = (u32::from(cadu[6]) << 16) | (u32::from(cadu[7]) << 8) | u32::from(cadu[8]);
            if self.last_counter.is_some_and(|last| counter.wrapping_sub(last) & 0xFF_FFFF != 1) {
                self.abort_packet();
                self.in_header = false;
            }
            self.last_counter = Some(counter);
        }

        let h = self.s.mpdu_header_at();
        let fhp = (i64::from(cadu[h] & 0b111) << 8) | i64::from(cadu[h + 1]);
        let data = &cadu[h + 2..h + 2 + self.s.mpdu_data_size];

        // A pointer past the packet zone: this frame cannot be used
        if fhp < NO_HEADER && fhp >= mpdu {
            self.abort_packet();
            self.in_header = false;
            return &self.out;
        }

        // A header cut across frames is completed from the start of this one
        let mut offset = 0i64;
        if self.in_header {
            self.in_header = false;
            let have = self.in_header_buffer;
            self.header_buffer[have..].copy_from_slice(&data[..6 - have]);
            offset = i64::try_from(6 - have).expect("at most 6");
            self.in_header_buffer = 6;
            self.read_packet(self.header_buffer);
        }

        // The rest of the packet in progress
        if self.remaining > 0 && self.working {
            if fhp < NO_HEADER {
                // It must end before the new header; strict mode: exactly there
                if self.s.strict && self.remaining != fhp - offset {
                    self.abort_packet();
                } else if self.remaining <= fhp - offset {
                    let end = offset + self.remaining;
                    self.push_payload(&data[at(offset)..at(end)]);
                } else {
                    self.abort_packet();
                }
            } else if self.s.strict && self.remaining < mpdu - offset {
                // No packet starts in this frame, so strict mode wants it to fill the zone
                self.abort_packet();
            } else {
                let to_write = self.remaining.min(mpdu - offset);
                self.push_payload(&data[at(offset)..at(offset + to_write)]);
            }
        }

        if self.remaining == 0 && self.working {
            self.push_packet();
        }

        if fhp < NO_HEADER {
            if fhp + HEADER < mpdu {
                self.read_packet(header_at(data, at(fhp)));
                // Whether another header can follow this packet in the frame
                if mpdu > fhp + self.total_length {
                    if fhp + self.total_length < mpdu {
                        let from = fhp + HEADER;
                        self.push_payload(&data[at(from)..at(from + self.payload_length)]);
                        self.push_packet();
                    } else {
                        self.abort_packet();
                    }

                    let mut next = fhp + self.total_length;
                    while next < mpdu {
                        if next + HEADER < mpdu {
                            self.read_packet(header_at(data, at(next)));
                            let to_write = self.remaining.min(mpdu - (next + HEADER));
                            let from = next + HEADER;
                            self.push_payload(&data[at(from)..at(from + to_write)]);
                        } else {
                            // Only part of the header fits: the rest is in the next frame
                            self.keep_header_start(data, at(next));
                            break;
                        }

                        if self.remaining == 0 && self.working {
                            self.push_packet();
                        }
                        next += self.total_length;
                    }
                } else if self.working {
                    let to_write = self.remaining.min(mpdu - (fhp + HEADER));
                    let from = fhp + HEADER;
                    self.push_payload(&data[at(from)..at(from + to_write)]);
                }
            } else if fhp < mpdu {
                self.keep_header_start(data, at(fhp));
            }
        }

        &self.out
    }

    /// The packets the last [`Demuxer::work`] call finished
    pub fn packets(&self) -> &[Packet] {
        &self.out
    }
}

/// The 6 header bytes at `from` (the caller checked that they are in the zone)
fn header_at(data: &[u8], from: usize) -> [u8; 6] {
    let mut h = [0u8; 6];
    h.copy_from_slice(&data[from..from + 6]);
    h
}
