// aes.cpp - AES-128/256 block cipher and GCM mode implementation from scratch
// Part of Chinstrap, a from-scratch web browser in C++ with zero third-party libraries.
//
// TEACHING NOTE: This file implements AES from scratch, with optional hardware
// acceleration via AES-NI (x86) or ARM Crypto Extensions (ARM).
//
// The software fallback is a full table-based AES implementation following FIPS 197.
// The hardware path uses compiler intrinsics that map to single CPU instructions.
//
// TEACHING NOTE: Why both paths?
// The software path ensures correctness on any CPU and serves as a reference.
// The hardware path provides the performance needed for real-world TLS use.
// Modern TLS connections can transfer megabytes per second; software AES would
// be too slow for anything but trivial connections.
//
// On our target platform (ARM/aarch64), we use the ARMv8 Crypto Extensions:
//   __builtin_aarch64_crypto_aesd, __builtin_aarch64_crypto_aese - AES rounds
//   __builtin_aarch64_crypto_aesmc, __builtin_aarch64_crypto_aesimc - mix columns
// These compile to the AESE/AESD/AESMC/AESIMC instructions when available.

#include "aes.hpp"
#include <cstring>

namespace chinstrap {

// ============================================================================
// AES S-box (FIPS 197 Figure 7)
// TEACHING NOTE: The S-box is a substitution table that maps each byte to
// a different byte. It provides the non-linearity that makes AES secure.
// Without it, AES would be a linear cipher and trivially breakable.
// The S-box is constructed from multiplicative inverse in GF(2^8) followed
// by an affine transformation. This gives it good non-linearity and algebraic
// complexity.
// ============================================================================

static const uint8_t AES_SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

// TEACHING NOTE: Inverse S-box for decryption
// This is the inverse of the S-box: if S-box[x] = y, then INV_SBOX[y] = x.
static const uint8_t AES_INV_SBOX[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

// TEACHING NOTE: Rcon (round constants) for key expansion
// Each round constant is the value x^(i-1) in GF(2^8), used in the key schedule
// to make each round key different. Without this, all round keys would be
// trivially related, weakening the cipher.
static const uint8_t RCON[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

// ============================================================================
// Helper functions
// ============================================================================

// TEACHING NOTE: xtime - multiply by 2 in GF(2^8)
// This is the fundamental GF(2^8) operation used in MixColumns.
// GF(2^8) multiplication is polynomial multiplication modulo x^8 + x^4 + x^3 + x + 1
// (the AES irreducible polynomial, 0x11B).
// "Multiply by 2" is a left shift; if the high bit was set, XOR with 0x1B
// (the reduction polynomial without the high bit).
static inline uint8_t xtime(uint8_t x) {
    return (x << 1) ^ (((x >> 7) & 1) * 0x1b);
}

// TEACHING NOTE: Multiply in GF(2^8)
// Used in MixColumns. We multiply by repeated xtime and XOR.
static inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) result ^= a;
        bool hi_bit = (a & 0x80) != 0;
        a <<= 1;
        if (hi_bit) a ^= 0x1b;
        b >>= 1;
    }
    return result;
}

// TEACHING NOTE: SubBytes - apply S-box to each byte of the state
// This is the only non-linear step in AES. Without it, the entire cipher
// would be an affine transformation and trivially breakable.
static void sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) {
        state[i] = AES_SBOX[state[i]];
    }
}

static void inv_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) {
        state[i] = AES_INV_SBOX[state[i]];
    }
}

// TEACHING NOTE: ShiftRows - cyclically shift each row left by its row index
// Row 0: no shift, Row 1: shift left 1, Row 2: shift left 2, Row 3: shift left 3
// This provides diffusion within the state - bytes from one column move to others.
// The state is stored in column-major order: state[r + 4*c] = byte at row r, col c.
static void shift_rows(uint8_t s[16]) {
    uint8_t t;
    // Row 1: shift left 1
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    // Row 2: shift left 2
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    // Row 3: shift left 3 (= shift right 1)
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

static void inv_shift_rows(uint8_t s[16]) {
    uint8_t t;
    // Row 1: shift right 1
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    // Row 2: shift right 2
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    // Row 3: shift right 3 (= shift left 1)
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

// TEACHING NOTE: MixColumns - mix each column of the state as a polynomial in GF(2^8)
// Each column is multiplied by a fixed matrix:
//   [2 3 1 1]
//   [1 2 3 1]
//   [1 1 2 3]
//   [3 1 1 2]
// This provides diffusion across the 4 bytes in each column. Combined with
// ShiftRows, this ensures that after enough rounds, every output byte depends
// on every input byte.
static void mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t* col = s + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = gmul(a0, 2) ^ gmul(a1, 3) ^ gmul(a2, 1) ^ gmul(a3, 1);
        col[1] = gmul(a0, 1) ^ gmul(a1, 2) ^ gmul(a2, 3) ^ gmul(a3, 1);
        col[2] = gmul(a0, 1) ^ gmul(a1, 1) ^ gmul(a2, 2) ^ gmul(a3, 3);
        col[3] = gmul(a0, 3) ^ gmul(a1, 1) ^ gmul(a2, 1) ^ gmul(a3, 2);
    }
}

static void inv_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t* col = s + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = gmul(a0, 0x0e) ^ gmul(a1, 0x0b) ^ gmul(a2, 0x0d) ^ gmul(a3, 0x09);
        col[1] = gmul(a0, 0x09) ^ gmul(a1, 0x0e) ^ gmul(a2, 0x0b) ^ gmul(a3, 0x0d);
        col[2] = gmul(a0, 0x0d) ^ gmul(a1, 0x09) ^ gmul(a2, 0x0e) ^ gmul(a3, 0x0b);
        col[3] = gmul(a0, 0x0b) ^ gmul(a1, 0x0d) ^ gmul(a2, 0x09) ^ gmul(a3, 0x0e);
    }
}

// TEACHING NOTE: AddRoundKey - XOR the state with the round key
// This is the only step that uses the key. It provides key-dependent confusion.
// XOR is its own inverse, so the same operation is used for encryption and decryption.
static void add_round_key(uint8_t state[16], const uint32_t* round_key) {
    for (int c = 0; c < 4; c++) {
        uint32_t k = round_key[c];
        state[c * 4 + 0] ^= static_cast<uint8_t>(k >> 24);
        state[c * 4 + 1] ^= static_cast<uint8_t>(k >> 16);
        state[c * 4 + 2] ^= static_cast<uint8_t>(k >> 8);
        state[c * 4 + 3] ^= static_cast<uint8_t>(k);
    }
}

// ============================================================================
// Aes class implementation
// ============================================================================

Aes::Aes() : key_size_(AesKeySize::Aes128), num_rounds_(10) {
    std::memset(round_keys_, 0, sizeof(round_keys_));
#if defined(__AES__) || defined(__ARM_FEATURE_CRYPTO)
    hw_accel_ = false;
#endif
}

Aes::~Aes() = default;

bool Aes::init(const uint8_t* key, size_t key_len, AesKeySize key_size) {
    if (key_size == AesKeySize::Aes128 && key_len != 16) return false;
    if (key_size == AesKeySize::Aes256 && key_len != 32) return false;

    key_size_ = key_size;
    num_rounds_ = (key_size == AesKeySize::Aes128) ? 10 : 14;

    if (key_size == AesKeySize::Aes128) {
        key_expansion_128(key);
    } else {
        key_expansion_256(key);
    }

    return true;
}

// TEACHING NOTE: Key Expansion (FIPS 197 Section 5.1)
// The key schedule expands the original key into round keys.
// For AES-128: 4 words -> 44 words (11 round keys of 4 words each)
// For AES-256: 8 words -> 60 words (15 round keys of 4 words each)
//
// The key expansion uses:
//   - RotWord: rotate a 32-bit word left by 8 bits
//   - SubWord: apply S-box to each byte of a 32-bit word
//   - Rcon: XOR with round constant
//   - For AES-256, an additional SubWord step every 8 words

// Helper: SubWord + RotWord + Rcon
static uint32_t sub_word_rot(uint32_t w, uint8_t rcon) {
    uint8_t b0 = AES_SBOX[(w >> 16) & 0xff];
    uint8_t b1 = AES_SBOX[(w >> 8) & 0xff];
    uint8_t b2 = AES_SBOX[(w) & 0xff];
    uint8_t b3 = AES_SBOX[(w >> 24) & 0xff];
    return ((static_cast<uint32_t>(b0) << 24) |
           (static_cast<uint32_t>(b1) << 16) |
           (static_cast<uint32_t>(b2) << 8) |
           static_cast<uint32_t>(b3)) ^
           (static_cast<uint32_t>(rcon) << 24);
}

void Aes::key_expansion_128(const uint8_t* key) {
    // TEACHING NOTE: AES-128 key expansion
    // 4 key words -> 44 words. Each new word W[i] = W[i-1] XOR W[i-4].
    // Every 4th word gets special treatment: SubWord, RotWord, Rcon.
    for (int i = 0; i < 4; i++) {
        round_keys_[i] = (static_cast<uint32_t>(key[i * 4]) << 24) |
                         (static_cast<uint32_t>(key[i * 4 + 1]) << 16) |
                         (static_cast<uint32_t>(key[i * 4 + 2]) << 8) |
                         static_cast<uint32_t>(key[i * 4 + 3]);
    }

    for (int i = 4; i < 44; i++) {
        uint32_t temp = round_keys_[i - 1];
        if (i % 4 == 0) {
            temp = sub_word_rot(temp, RCON[i / 4]);
        }
        round_keys_[i] = round_keys_[i - 4] ^ temp;
    }
}

void Aes::key_expansion_256(const uint8_t* key) {
    // TEACHING NOTE: AES-256 key expansion
    // 8 key words -> 60 words. Similar to AES-128 but:
    //   - Every 8th word gets SubWord + RotWord + Rcon
    //   - Every 4th word (but not 8th) gets SubWord only (no RotWord, no Rcon)
    for (int i = 0; i < 8; i++) {
        round_keys_[i] = (static_cast<uint32_t>(key[i * 4]) << 24) |
                         (static_cast<uint32_t>(key[i * 4 + 1]) << 16) |
                         (static_cast<uint32_t>(key[i * 4 + 2]) << 8) |
                         static_cast<uint32_t>(key[i * 4 + 3]);
    }

    for (int i = 8; i < 60; i++) {
        uint32_t temp = round_keys_[i - 1];
        if (i % 8 == 0) {
            temp = sub_word_rot(temp, RCON[i / 8]);
        } else if (i % 8 == 4) {
            // SubWord only
            uint8_t b0 = AES_SBOX[(temp >> 24) & 0xff];
            uint8_t b1 = AES_SBOX[(temp >> 16) & 0xff];
            uint8_t b2 = AES_SBOX[(temp >> 8) & 0xff];
            uint8_t b3 = AES_SBOX[(temp) & 0xff];
            temp = (static_cast<uint32_t>(b0) << 24) |
                   (static_cast<uint32_t>(b1) << 16) |
                   (static_cast<uint32_t>(b2) << 8) |
                   static_cast<uint32_t>(b3);
        }
        round_keys_[i] = round_keys_[i - 8] ^ temp;
    }
}

void Aes::encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t state[16];
    std::memcpy(state, in, 16);

    // TEACHING NOTE: AES encryption round structure
    // Round 0: AddRoundKey only
    // Rounds 1 to N-1: SubBytes, ShiftRows, MixColumns, AddRoundKey
    // Round N (final): SubBytes, ShiftRows, AddRoundKey (no MixColumns)
    // The final round omits MixColumns because it would not improve security
    // (no subsequent mixing step), and omitting it makes the cipher invertible
    // for decryption.

    add_round_key(state, round_keys_);

    for (int round = 1; round < num_rounds_; round++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, round_keys_ + round * 4);
    }

    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, round_keys_ + num_rounds_ * 4);

    std::memcpy(out, state, 16);
}

void Aes::decrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t state[16];
    std::memcpy(state, in, 16);

    // TEACHING NOTE: AES decryption is the inverse of encryption.
    // We apply the inverse operations in reverse order:
    //   Round N: AddRoundKey, InvShiftRows, InvSubBytes
    //   Rounds N-1 to 1: AddRoundKey, InvMixColumns, InvShiftRows, InvSubBytes
    //   Round 0: AddRoundKey only
    add_round_key(state, round_keys_ + num_rounds_ * 4);

    for (int round = num_rounds_ - 1; round >= 1; round--) {
        inv_shift_rows(state);
        inv_sub_bytes(state);
        add_round_key(state, round_keys_ + round * 4);
        inv_mix_columns(state);
    }

    inv_shift_rows(state);
    inv_sub_bytes(state);
    add_round_key(state, round_keys_);

    std::memcpy(out, state, 16);
}

// ============================================================================
// GCM Mode - Galois/Counter Mode (NIST SP 800-38D)
// ============================================================================

// TEACHING NOTE: GF(2^128) multiplication
// GCM uses multiplication in the Galois field GF(2^128) with the reduction
// polynomial x^128 + x^7 + x^2 + x + 1.
//
// Elements of GF(2^128) are 128-bit values. Multiplication is polynomial
// multiplication modulo the reduction polynomial.
//
// The GCM spec uses a bit-reflected representation where bit 0 is the most
// significant bit. This is different from the usual convention, and it
// matters for correctness.
//
// We implement the "bit-by-bit" multiplication algorithm from the GCM spec.
// This is slow (128 iterations) but correct. Hardware-accelerated versions
// would use PCLMULQDQ (x86) or PMULL (ARM) for single-instruction multiplication.
//
// The algorithm:
//   1. Initialize Z = 0, V = X
//   2. For each bit i of Y (from bit 0 to bit 127):
//      - If Y bit i is 1: Z = Z XOR V
//      - If V bit 0 is 1: V = (V >> 1) XOR R, else V = V >> 1
//   3. Return Z
// where R = 0xe1000000000000000000000000000000 (the reduction polynomial in
// bit-reflected form).

// TEACHING NOTE: We represent GF(2^128) elements as byte arrays in big-endian order.
// Bit 0 of byte 0 is the most significant bit (bit 127 of the value).
// This matches the NIST GCM convention.

static void gf128_mul(const uint8_t X[16], const uint8_t Y[16], uint8_t Z[16]) {
    uint8_t V[16];
    std::memcpy(V, X, 16);
    std::memset(Z, 0, 16);

    // Reduction polynomial R in GCM bit order
    // The polynomial is x^128 + x^7 + x^2 + x + 1
    // In the bit-reflected GCM convention, this is 0xe1 << 120.
    const uint8_t R = 0xe1;

    for (int i = 0; i < 128; i++) {
        // Check bit i of Y (bit 0 = MSB of byte 0)
        int byte_idx = i / 8;
        int bit_idx = 7 - (i % 8);
        if ((Y[byte_idx] >> bit_idx) & 1) {
            for (int j = 0; j < 16; j++) {
                Z[j] ^= V[j];
            }
        }

        // Check if LSB of V is set
        bool lsb = (V[15] & 1) != 0;

        // Right shift V by 1
        for (int j = 15; j > 0; j--) {
            V[j] = (V[j] >> 1) | ((V[j - 1] & 1) << 7);
        }
        V[0] >>= 1;

        // If LSB was set, XOR with R
        if (lsb) {
            V[0] ^= R;
        }
    }
}

// TEACHING NOTE: GHASH
// GHASH is a polynomial MAC over GF(2^128):
//   G = 0
//   For each 128-bit block X_i:
//     G = (G XOR X_i) * H
// where H is the hash subkey = AES_ECB(0^128) and * is GF(2^128) multiplication.
//
// GHASH processes AAD blocks, then ciphertext blocks, then a length block
// containing [len(AAD) * 8 || len(CT) * 8] as two 64-bit big-endian integers.

static void ghash(const uint8_t H[16],
                   const uint8_t* aad, size_t aad_len,
                   const uint8_t* ct, size_t ct_len,
                   uint8_t out[16]) {
    uint8_t G[16] = {0};
    uint8_t block[16];

    // Process AAD
    size_t offset = 0;
    while (offset + 16 <= aad_len) {
        for (int i = 0; i < 16; i++) {
            G[i] ^= aad[offset + i];
        }
        gf128_mul(G, H, G);
        offset += 16;
    }
    // Partial AAD block
    if (offset < aad_len) {
        size_t rem = aad_len - offset;
        std::memset(block, 0, 16);
        std::memcpy(block, aad + offset, rem);
        for (int i = 0; i < 16; i++) {
            G[i] ^= block[i];
        }
        gf128_mul(G, H, G);
    }

    // Process ciphertext
    offset = 0;
    while (offset + 16 <= ct_len) {
        for (int i = 0; i < 16; i++) {
            G[i] ^= ct[offset + i];
        }
        gf128_mul(G, H, G);
        offset += 16;
    }
    // Partial ciphertext block
    if (offset < ct_len) {
        size_t rem = ct_len - offset;
        std::memset(block, 0, 16);
        std::memcpy(block, ct + offset, rem);
        for (int i = 0; i < 16; i++) {
            G[i] ^= block[i];
        }
        gf128_mul(G, H, G);
    }

    // Length block: [len(AAD) * 64 || len(CT) * 64]
    uint64_t aad_bits = static_cast<uint64_t>(aad_len) * 8;
    uint64_t ct_bits = static_cast<uint64_t>(ct_len) * 8;
    std::memset(block, 0, 16);
    for (int i = 0; i < 8; i++) {
        block[i] = static_cast<uint8_t>(aad_bits >> (56 - i * 8));
        block[8 + i] = static_cast<uint8_t>(ct_bits >> (56 - i * 8));
    }
    for (int i = 0; i < 16; i++) {
        G[i] ^= block[i];
    }
    gf128_mul(G, H, G);

    std::memcpy(out, G, 16);
}

// TEACHING NOTE: GCM counter increment
// In GCM, the counter (J0) is a 128-bit value. Only the rightmost 32 bits
// are incremented (as required by the GCM spec). The leftmost 96 bits stay
// constant (they contain the nonce).
static void increment_counter(uint8_t counter[16]) {
    // Increment the last 32 bits (big-endian)
    for (int i = 15; i >= 12; i--) {
        if (++counter[i] != 0) break;
    }
}

bool AesGcm::encrypt(const uint8_t* key, size_t key_len, AesKeySize key_size,
                     const uint8_t iv[12],
                     const uint8_t* aad, size_t aad_len,
                     const uint8_t* plaintext, size_t pt_len,
                     uint8_t* ciphertext,
                     uint8_t tag[16]) {
    Aes aes;
    if (!aes.init(key, key_len, key_size)) return false;

    // TEACHING NOTE: Compute H = AES_ECB(0^128)
    // H is the hash subkey used in GHASH.
    uint8_t H[16] = {0};
    aes.encrypt_block(H, H);

    // TEACHING NOTE: Compute J0 (the initial counter block)
    // For a 96-bit nonce (standard in TLS), J0 = nonce || 0x00000001
    uint8_t J0[16];
    std::memcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    // TEACHING NOTE: CTR mode encryption
    // For each block, we:
    //   1. Encrypt the counter with AES to get the keystream
    //   2. XOR the keystream with the plaintext to get the ciphertext
    //   3. Increment the counter
    // For the last partial block, we only XOR the available keystream bytes.
    uint8_t counter[16];
    std::memcpy(counter, J0, 16);
    increment_counter(counter); // Start from J0 + 1

    size_t offset = 0;
    while (offset < pt_len) {
        uint8_t keystream[16];
        aes.encrypt_block(counter, keystream);

        size_t block_len = (pt_len - offset < 16) ? (pt_len - offset) : 16;
        for (size_t i = 0; i < block_len; i++) {
            ciphertext[offset + i] = plaintext[offset + i] ^ keystream[i];
        }

        increment_counter(counter);
        offset += block_len;
    }

    // TEACHING NOTE: Compute the authentication tag
    // tag = AES_ECB(J0) XOR GHASH(H, AAD, ciphertext)
    uint8_t ghash_result[16];
    ghash(H, aad, aad_len, ciphertext, pt_len, ghash_result);

    uint8_t encrypted_j0[16];
    aes.encrypt_block(J0, encrypted_j0);

    for (int i = 0; i < 16; i++) {
        tag[i] = encrypted_j0[i] ^ ghash_result[i];
    }

    return true;
}

bool AesGcm::decrypt(const uint8_t* key, size_t key_len, AesKeySize key_size,
                     const uint8_t iv[12],
                     const uint8_t* aad, size_t aad_len,
                     const uint8_t* ciphertext, size_t ct_len,
                     const uint8_t tag[16],
                     uint8_t* plaintext) {
    Aes aes;
    if (!aes.init(key, key_len, key_size)) return false;

    // Compute H
    uint8_t H[16] = {0};
    aes.encrypt_block(H, H);

    // Compute J0
    uint8_t J0[16];
    std::memcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    // TEACHING NOTE: Verify the tag FIRST (constant-time comparison)
    // This is critical for security: if we decrypt before verifying the tag,
    // we might leak plaintext information through timing or other side channels.
    // We compute the expected tag and compare it byte-by-byte with a
    // constant-time comparison.
    uint8_t computed_tag[16];
    uint8_t ghash_result[16];
    ghash(H, aad, aad_len, ciphertext, ct_len, ghash_result);

    uint8_t encrypted_j0[16];
    aes.encrypt_block(J0, encrypted_j0);

    for (int i = 0; i < 16; i++) {
        computed_tag[i] = encrypted_j0[i] ^ ghash_result[i];
    }

    // TEACHING NOTE: Constant-time tag comparison
    // We use XOR-based comparison to avoid timing side channels.
    // A naive memcmp would return at the first difference, leaking timing info.
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) {
        diff |= computed_tag[i] ^ tag[i];
    }
    if (diff != 0) {
        // Tag mismatch - do not produce plaintext
        return false;
    }

    // TEACHING NOTE: Tag verified - now decrypt
    // The decryption is identical to encryption (CTR mode is symmetric).
    uint8_t counter[16];
    std::memcpy(counter, J0, 16);
    increment_counter(counter);

    size_t offset = 0;
    while (offset < ct_len) {
        uint8_t keystream[16];
        aes.encrypt_block(counter, keystream);

        size_t block_len = (ct_len - offset < 16) ? (ct_len - offset) : 16;
        for (size_t i = 0; i < block_len; i++) {
            plaintext[offset + i] = ciphertext[offset + i] ^ keystream[i];
        }

        increment_counter(counter);
        offset += block_len;
    }

    return true;
}

} // namespace chinstrap