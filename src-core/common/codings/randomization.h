#pragma once

#include <cstdint>

/*
CCSDS Derandomizer, adapted from SatHelper
*/

// Byte (bit)-based derandomizer
void derand_ccsds(uint8_t *data, int length);

// Fork: the C++ version of derand_ccsds, also the reference the Rust version is tested against
void derand_ccsds_cpp(uint8_t *data, int length);

// Soft symbol derandomizer
void derand_ccsds_soft(int8_t *data, int length);

// Bits
void derand_ccsds_bits(uint8_t *data, int length);
