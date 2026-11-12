//! The integer 2-D inverse DCT of the C++ `idct.cpp` (fixed point, Z. Wang's algorithm), step
//! for step. Rust's `>>` on a negative number is the arithmetic shift GCC and clang use in the
//! C++. The fork's C++ multiplies where upstream shifted negative numbers left (undefined
//! behaviour in C++17); a multiplication gives the same numbers.

const W1: i64 = 2841; // 2048 * sqrt(2) * cos(1 * pi / 16)
const W2: i64 = 2676; // 2048 * sqrt(2) * cos(2 * pi / 16)
const W3: i64 = 2408; // 2048 * sqrt(2) * cos(3 * pi / 16)
const W5: i64 = 1609; // 2048 * sqrt(2) * cos(5 * pi / 16)
const W6: i64 = 1108; // 2048 * sqrt(2) * cos(6 * pi / 16)
const W7: i64 = 565; // 2048 * sqrt(2) * cos(7 * pi / 16)

const W1PW7: i64 = W1 + W7;
const W1MW7: i64 = W1 - W7;
const W2PW6: i64 = W2 + W6;
const W2MW6: i64 = W2 - W6;
const W3PW5: i64 = W3 + W5;
const W3MW5: i64 = W3 - W5;

const R2: i64 = 181; // 256 / sqrt(2)

/// The inverse DCT of one block of dequantised coefficients (rows of 8), in place
pub(super) fn idct(src: &mut [i64; 64]) {
    // Horizontal 1-D IDCT
    for y8 in (0..64).step_by(8) {
        let row = &mut src[y8..y8 + 8];
        // If all the AC components are zero, then the IDCT is trivial
        if row[1..].iter().all(|&v| v == 0) {
            let dc = row[0] * 8;
            row.fill(dc);
            continue;
        }

        // Prescale
        let mut x0 = row[0] * 2048 + 128;
        let mut x1 = row[4] * 2048;
        let mut x2 = row[6];
        let mut x3 = row[2];
        let mut x4 = row[1];
        let mut x5 = row[7];
        let mut x6 = row[5];
        let mut x7 = row[3];

        // Stage 1
        let mut x8 = W7 * (x4 + x5);
        x4 = x8 + W1MW7 * x4;
        x5 = x8 - W1PW7 * x5;
        x8 = W3 * (x6 + x7);
        x6 = x8 - W3MW5 * x6;
        x7 = x8 - W3PW5 * x7;

        // Stage 2
        x8 = x0 + x1;
        x0 -= x1;
        x1 = W6 * (x3 + x2);
        x2 = x1 - W2PW6 * x2;
        x3 = x1 + W2MW6 * x3;
        x1 = x4 + x6;
        x4 -= x6;
        x6 = x5 + x7;
        x5 -= x7;

        // Stage 3
        x7 = x8 + x3;
        x8 -= x3;
        x3 = x0 + x2;
        x0 -= x2;
        x2 = (R2 * (x4 + x5) + 128) >> 8;
        x4 = (R2 * (x4 - x5) + 128) >> 8;

        // Stage 4
        row[0] = (x7 + x1) >> 8;
        row[1] = (x3 + x2) >> 8;
        row[2] = (x0 + x4) >> 8;
        row[3] = (x8 + x6) >> 8;
        row[4] = (x8 - x6) >> 8;
        row[5] = (x0 - x4) >> 8;
        row[6] = (x3 - x2) >> 8;
        row[7] = (x7 - x1) >> 8;
    }

    // Vertical 1-D IDCT. After the horizontal pass there are typically non-zero AC
    // components, so the all-zero case is not looked for here.
    for x in 0..8 {
        // Prescale
        let mut y0 = src[x] * 256 + 8192;
        let mut y1 = src[8 * 4 + x] * 256;
        let mut y2 = src[8 * 6 + x];
        let mut y3 = src[8 * 2 + x];
        let mut y4 = src[8 + x];
        let mut y5 = src[8 * 7 + x];
        let mut y6 = src[8 * 5 + x];
        let mut y7 = src[8 * 3 + x];

        // Stage 1
        let mut y8 = W7 * (y4 + y5) + 4;
        y4 = (y8 + W1MW7 * y4) >> 3;
        y5 = (y8 - W1PW7 * y5) >> 3;
        y8 = W3 * (y6 + y7) + 4;
        y6 = (y8 - W3MW5 * y6) >> 3;
        y7 = (y8 - W3PW5 * y7) >> 3;

        // Stage 2
        y8 = y0 + y1;
        y0 -= y1;
        y1 = W6 * (y3 + y2) + 4;
        y2 = (y1 - W2PW6 * y2) >> 3;
        y3 = (y1 + W2MW6 * y3) >> 3;
        y1 = y4 + y6;
        y4 -= y6;
        y6 = y5 + y7;
        y5 -= y7;

        // Stage 3
        y7 = y8 + y3;
        y8 -= y3;
        y3 = y0 + y2;
        y0 -= y2;
        y2 = (R2 * (y4 + y5) + 128) >> 8;
        y4 = (R2 * (y4 - y5) + 128) >> 8;

        // Stage 4
        src[x] = (y7 + y1) >> 14;
        src[8 + x] = (y3 + y2) >> 14;
        src[8 * 2 + x] = (y0 + y4) >> 14;
        src[8 * 3 + x] = (y8 + y6) >> 14;
        src[8 * 4 + x] = (y8 - y6) >> 14;
        src[8 * 5 + x] = (y0 - y4) >> 14;
        src[8 * 6 + x] = (y3 - y2) >> 14;
        src[8 * 7 + x] = (y7 - y1) >> 14;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_flat_block_stays_flat() {
        // DC only: 8 * dc after the rows, (8 * dc * 256 + 8192 + ...) >> 14 after the columns
        let mut b = [0i64; 64];
        b[0] = 80;
        idct(&mut b);
        assert!(b.iter().all(|&v| v == 10), "{b:?}");
        let mut b = [0i64; 64];
        b[0] = -1024;
        idct(&mut b);
        assert!(b.iter().all(|&v| v == -128), "{b:?}");
    }

    #[test]
    fn one_ac_coefficient_makes_a_cosine_across_the_rows() {
        // The first horizontal frequency: a half cosine, symmetric about the middle
        let mut b = [0i64; 64];
        b[1] = 160;
        idct(&mut b);
        let row: Vec<i64> = b[..8].to_vec();
        assert!(row[0] > row[1] && row[1] > row[2] && row[2] > row[3] && row[3] > 0, "{row:?}");
        for (k, v) in row.iter().enumerate() {
            assert!((v + row[7 - k]).abs() <= 1, "{row:?}");
        }
        assert!(b.chunks(8).all(|r| r == row.as_slice()));
    }
}
