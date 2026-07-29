// sha256.cpp - SHA-256 implementation from scratch per FIPS 180-4
// Part of Chinstrap, a from-scratch web browser in C++ with zero third-party libraries.
//
// TEACHING NOTE: FIPS 180-4
// The SHA-256 specification is in FIPS 180-4, Section 6.2. This implementation
// follows that specification exactly. The algorithm has three main parts:
//
// 1. Preprocessing: pad the message to a multiple of 512 bits
// 2. Hash computation: process each 512-bit block through the compression function
// 3. Output: the final state IS the hash
//
// TEACHING NOTE: Padding
// The message is padded as follows:
//   - Append a single 1 bit (as 0x80 byte)
//   - Append zeros until length = 56 mod 64 (leaving 8 bytes for length)
//   - Append the original message length as a 64-bit big-endian integer (in BITS)
// This padding ensures different messages produce different padded blocks,
// and the length field prevents length-extension attacks.
//
// TEACHING NOTE: The compression function
// Each 512-bit block is split into 16x 32-bit big-endian words (W[0..15]).
// We then extend these to 64 words (W[0..63]) using a recurrence relation.
// The 64 rounds each apply a combination of:
//   - The Ch (choose) function: Ch(x,y,z) = (x AND y) XOR (NOT x AND z)
//   - The Maj (majority) function: Maj(x,y,z) = (x AND y) XOR (x AND z) XOR (y AND z)
//   - Right rotations and right shifts
//   - Round constants K[0..63] (fractional parts of cube roots of primes)
// These non-linear operations create the avalanche effect that makes SHA-256
// cryptographically strong.

#include "sha256.hpp"
#include <cstring>
#include <string>

namespace chinstrap {

// TEACHING NOTE: Initial hash values (FIPS 180-4 Section 5.3.3)
// These are the fractional parts of the square roots of the first 8 prime numbers:
//   sqrt(2), sqrt(3), sqrt(5), sqrt(7), sqrt(11), sqrt(13), sqrt(17), sqrt(19)
// These are "nothing up my sleeve" numbers - they are derived from a simple
// mathematical property and are not chosen adversarially.
static const uint32_t K_INIT[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

// TEACHING NOTE: Round constants (FIPS 180-4 Section 4.2.2)
// These are the fractional parts of the cube roots of the first 64 prime numbers.
// Each round uses a different constant to ensure rounds are not identical.
static const uint32_t K_ROUND[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// TEACHING NOTE: Bitwise rotation
// SHA-256 uses right-rotation (circular shift). Most C++ compilers recognize
// this pattern and optimize it to a single rotate instruction (ROR on x86).
// We write it explicitly for portability and clarity.
static inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

// TEACHING NOTE: SHA-256 functions (FIPS 180-4 Section 4.1.2)
// Ch (Choose): for each bit position, if x is 1, take y, else take z.
//   This is a bit-level multiplexer controlled by x.
// Maj (Majority): for each bit position, take the majority vote of x, y, z.
//   This provides diffusion - changing one input bit affects many output bits.
// Sigma0, Sigma1: these are rotation-based diffusion functions that ensure
//   each input bit affects many output bits across rounds.
static inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}
static inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
static inline uint32_t Sigma0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}
static inline uint32_t Sigma1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}
static inline uint32_t sigma0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}
static inline uint32_t sigma1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

Sha256::Sha256() {
    init();
}

Sha256::~Sha256() = default;

void Sha256::init() {
    for (int i = 0; i < 8; i++) {
        state_[i] = K_INIT[i];
    }
    buffer_len_ = 0;
    total_len_ = 0;
}

void Sha256::update(const uint8_t* data, size_t len) {
    total_len_ += static_cast<uint64_t>(len);

    // TEACHING NOTE: We fill the buffer first, then process full blocks.
    // This handles the case where update() is called with data that does not
    // align to 64-byte boundaries.
    if (buffer_len_ > 0) {
        size_t to_fill = 64 - buffer_len_;
        if (to_fill > len) to_fill = len;
        std::memcpy(buffer_ + buffer_len_, data, to_fill);
        buffer_len_ += to_fill;
        data += to_fill;
        len -= to_fill;

        if (buffer_len_ == 64) {
            process_block(buffer_);
            buffer_len_ = 0;
        }
    }

    // Process full blocks directly from the input
    while (len >= 64) {
        process_block(data);
        data += 64;
        len -= 64;
    }

    // Buffer remaining bytes
    if (len > 0) {
        std::memcpy(buffer_, data, len);
        buffer_len_ = static_cast<size_t>(len);
    }
}

Sha256Digest Sha256::final() {
    // TEACHING NOTE: Padding
    // We need to append: 0x80, then zeros, then the 64-bit big-endian bit length.
    // The padded message length must be a multiple of 64 bytes.
    // So we need 64 - buffer_len_ bytes of padding, but the last 8 bytes are
    // the length field. If there is not enough room (buffer_len_ > 55),
    // we need to pad into the next block.

    uint64_t bit_len = total_len_ * 8;

    // Append 0x80
    buffer_[buffer_len_++] = 0x80;

    // Pad with zeros until we have 8 bytes left for the length
    if (buffer_len_ > 56) {
        // Not enough room in this block, pad to end and process
        while (buffer_len_ < 64) {
            buffer_[buffer_len_++] = 0;
        }
        process_block(buffer_);
        buffer_len_ = 0;
    }

    // Pad with zeros up to byte 56
    while (buffer_len_ < 56) {
        buffer_[buffer_len_++] = 0;
    }

    // Append length in bits as big-endian 64-bit integer
    for (int i = 7; i >= 0; i--) {
        buffer_[buffer_len_++] = static_cast<uint8_t>(bit_len >> (i * 8));
    }

    process_block(buffer_);

    // TEACHING NOTE: The final state IS the hash output.
    // We just need to serialize the 8x 32-bit state words in big-endian order.
    Sha256Digest digest;
    for (int i = 0; i < 8; i++) {
        digest.bytes[i * 4 + 0] = static_cast<uint8_t>(state_[i] >> 24);
        digest.bytes[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        digest.bytes[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        digest.bytes[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }

    return digest;
}

Sha256Digest Sha256::hash(const uint8_t* data, size_t len) {
    Sha256 s;
    s.update(data, len);
    return s.final();
}

Sha256Digest Sha256::hash(const char* data, size_t len) {
    return hash(reinterpret_cast<const uint8_t*>(data), len);
}

void Sha256::process_block(const uint8_t* block) {
    // TEACHING NOTE: Message schedule
    // The first 16 words W[0..15] are the 16 big-endian 32-bit words from the block.
    // The remaining 48 words W[16..63] are computed using a recurrence:
    //   W[t] = sigma1(W[t-2]) + W[t-7] + sigma0(W[t-15]) + W[t-16]
    // This expansion ensures that each round depends on all bits of the block,
    // not just the local 32-bit word, providing diffusion across the block.

    uint32_t W[64];

    for (int t = 0; t < 16; t++) {
        W[t] = (static_cast<uint32_t>(block[t * 4]) << 24) |
               (static_cast<uint32_t>(block[t * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[t * 4 + 2]) << 8) |
               static_cast<uint32_t>(block[t * 4 + 3]);
    }

    for (int t = 16; t < 64; t++) {
        W[t] = sigma1(W[t - 2]) + W[t - 7] + sigma0(W[t - 15]) + W[t - 16];
    }

    // TEACHING NOTE: Compression function
    // We save the current state, then run 64 rounds. Each round:
    //   1. Compute T1 = h + Sigma1(e) + Ch(e,f,g) + K[t] + W[t]
    //   2. Compute T2 = Sigma0(a) + Maj(a,b,c)
    //   3. Shift all state variables: h=g, g=f, f=e, e=d+T1, d=c, c=b, b=a, a=T1+T2
    // The addition of T1 to d and T1+T2 to a creates the mixing between rounds.
    // The round constants K[t] ensure each round is different, preventing
    // slide attacks and other structural attacks.

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int t = 0; t < 64; t++) {
        uint32_t T1 = h + Sigma1(e) + Ch(e, f, g) + K_ROUND[t] + W[t];
        uint32_t T2 = Sigma0(a) + Maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    // TEACHING NOTE: Add the compressed block to the state.
    // This is the Davies-Meyer construction: the input state is added to
    // the output of the compression function. This makes the function
    // one-way (you cannot easily reverse it) and is key to collision resistance.
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

// TEACHING NOTE: HMAC-SHA256 implementation (RFC 2104)
// HMAC(K, m) = H((K ^ opad) || H((K ^ ipad) || m))
//
// The key K is padded (or hashed) to the block size (64 bytes for SHA-256).
// ipad = 0x36 repeated, opad = 0x5c repeated.
//
// The inner hash is: H((K ^ ipad) || m)
// The outer hash is: H((K ^ opad) || inner_hash)
//
// This construction is provably secure given that H is a good hash function.
// It allows the key to be any length, and the MAC is the same size as the hash output.

void HmacSha256::compute(const uint8_t* key, size_t key_len,
                          const uint8_t* message, size_t msg_len,
                          uint8_t out[32]) {
    compute(key, key_len, &message, &msg_len, 1, out);
}

void HmacSha256::compute(const uint8_t* key, size_t key_len,
                          const uint8_t* const* segments, const size_t* seg_lens,
                          size_t num_segments,
                          uint8_t out[32]) {
    // TEACHING NOTE: Key processing
    // If the key is longer than the block size (64 bytes), hash it first.
    // Then pad the key to 64 bytes with zeros.
    uint8_t k[64];
    std::memset(k, 0, 64);

    if (key_len > 64) {
        Sha256Digest kd = Sha256::hash(key, key_len);
        std::memcpy(k, kd.bytes.data(), 32);
    } else {
        std::memcpy(k, key, key_len);
    }

    // TEACHING NOTE: Inner hash: H((K ^ ipad) || message)
    uint8_t ipad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
    }

    Sha256 inner;
    inner.init();
    inner.update(ipad, 64);
    for (size_t s = 0; s < num_segments; s++) {
        inner.update(segments[s], seg_lens[s]);
    }
    Sha256Digest inner_digest = inner.final();

    // TEACHING NOTE: Outer hash: H((K ^ opad) || inner_hash)
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        opad[i] = k[i] ^ 0x5c;
    }

    Sha256 outer;
    outer.init();
    outer.update(opad, 64);
    outer.update(inner_digest.bytes.data(), 32);
    Sha256Digest outer_digest = outer.final();

    std::memcpy(out, outer_digest.bytes.data(), 32);
}

} // namespace chinstrap