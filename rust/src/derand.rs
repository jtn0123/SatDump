//! CCSDS pseudo-randomiser (CCSDS 131.0-B): h(x) = x^8 + x^7 + x^5 + x^3 + 1, all-ones seed.
//! The same sequence XORed in again removes it, so one function both adds and removes it.

/// The sequence repeats every 255 bytes
pub const PERIOD: usize = 255;

/// One period of the sequence, worked out from the polynomial when this is compiled
pub const PN: [u8; PERIOD] = pn_bytes();

const fn pn_bytes() -> [u8; PERIOD] {
    let mut out = [0u8; PERIOD];
    let mut reg = [1u8; 8];
    let mut i = 0;
    while i < PERIOD {
        let mut byte = 0u8;
        let mut b = 0;
        while b < 8 {
            byte = (byte << 1) | reg[0];
            let feedback = reg[0] ^ reg[3] ^ reg[5] ^ reg[7];
            let mut k = 0;
            while k < 7 {
                reg[k] = reg[k + 1];
                k += 1;
            }
            reg[7] = feedback;
            b += 1;
        }
        out[i] = byte;
        i += 1;
    }
    out
}

/// XOR the sequence into `data`, starting at the start of the sequence
pub fn derandomize(data: &mut [u8]) {
    for (byte, pn) in data.iter_mut().zip(PN.iter().cycle()) {
        *byte ^= pn;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn starts_like_the_standard() {
        assert_eq!(PN[..8], [0xFF, 0x48, 0x0E, 0xC0, 0x9A, 0x0D, 0x70, 0xBC]);
        assert_eq!(PN[PERIOD - 3..], [0x66, 0xF5, 0x58]);
    }

    #[test]
    fn repeats_every_255_bytes() {
        let mut data = vec![0u8; 3 * PERIOD + 7];
        derandomize(&mut data);
        for (i, v) in data.iter().enumerate() {
            assert_eq!(*v, PN[i % PERIOD], "byte {i}");
        }
    }

    #[test]
    fn twice_gives_back_the_original() {
        let original: Vec<u8> = (0..1020u32).map(|i| (i.wrapping_mul(2_654_435_761) >> 13) as u8).collect();
        let mut data = original.clone();
        derandomize(&mut data);
        assert_ne!(data, original);
        derandomize(&mut data);
        assert_eq!(data, original);
    }

    #[test]
    fn empty_input_is_fine() {
        derandomize(&mut []);
    }
}
