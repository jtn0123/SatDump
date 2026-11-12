//! CCSDS pseudo-randomiser (CCSDS 131.0-B): h(x) = x^8 + x^7 + x^5 + x^3 + 1, all-ones seed.
//! The same sequence applied again removes it, so each function both adds and removes it:
//! on bytes, on soft symbols (one `i8` per bit) and on hard bits (one `u8` per bit).

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

/// The sequence one bit at a time (it repeats every 255 bits): the bits of [`PN`], top bit first
pub const PN_BITS: [bool; PERIOD] = pn_bits();

const fn pn_bits() -> [bool; PERIOD] {
    let mut out = [false; PERIOD];
    let mut k = 0;
    while k < PERIOD {
        out[k] = (PN[k / 8] >> (7 - k % 8)) & 1 == 1;
        k += 1;
    }
    out
}

/// XOR the sequence into `data`, starting at the start of the sequence
pub fn derandomize(data: &mut [u8]) {
    for (byte, pn) in data.iter_mut().zip(PN.iter().cycle()) {
        *byte ^= pn;
    }
}

/// Soft symbols, one per bit: each one where the sequence has a 1 is inverted (bitwise NOT, as
/// the C++ `~`)
pub fn derandomize_soft(symbols: &mut [i8]) {
    for (symbol, bit) in symbols.iter_mut().zip(PN_BITS.iter().cycle()) {
        if *bit {
            *symbol = !*symbol;
        }
    }
}

/// Hard bits, one per byte: each one where the sequence has a 1 becomes 1 if it was 0 and 0
/// otherwise (as the C++ `!`, so any value other than 0 counts as a 1)
pub fn derandomize_bits(bits: &mut [u8]) {
    for (b, bit) in bits.iter_mut().zip(PN_BITS.iter().cycle()) {
        if *bit {
            *b = u8::from(*b == 0);
        }
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
        derandomize_soft(&mut []);
        derandomize_bits(&mut []);
    }

    #[test]
    fn the_bit_sequence_is_the_byte_sequence() {
        // 255 bits, then the same again: bit k of the endless sequence is PN_BITS[k % 255]
        for k in 0..8 * PERIOD {
            assert_eq!(PN_BITS[k % PERIOD], (PN[k / 8] >> (7 - k % 8)) & 1 == 1, "bit {k}");
        }
    }

    #[test]
    fn soft_and_bits_invert_where_the_sequence_has_a_1() {
        let n = 3 * PERIOD + 7;
        let mut soft = vec![100i8; n];
        derandomize_soft(&mut soft);
        let mut bits = vec![0u8; n];
        derandomize_bits(&mut bits);
        let mut odd = vec![7u8; n];
        derandomize_bits(&mut odd);
        for i in 0..n {
            let one = PN_BITS[i % PERIOD];
            assert_eq!(soft[i], if one { !100i8 } else { 100 }, "soft {i}");
            assert_eq!(bits[i], u8::from(one), "bit {i}");
            assert_eq!(odd[i], if one { 0 } else { 7 }, "odd {i}");
        }
    }
}
