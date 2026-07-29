// sha256.hpp - SHA-256 hash function implementation from scratch
// Part of Chinstrap, a from-scratch web browser in C++ with zero third-party libraries.
//
// TEACHING NOTE: What is SHA-256?
// SHA-256 is a cryptographic hash function from the SHA-2 family, defined in
// FIPS 180-4 (Federal Information Processing Standards Publication 180-4).
// It takes an arbitrary-length input and produces a fixed 256-bit (32-byte) output.
//
// TEACHING NOTE: Why do we need SHA-256 in TLS?
// SHA-256 is used throughout TLS for:
//   1. The PRF (Pseudo-Random Function) in TLS 1.2 to derive session keys
//   2. The handshake transcript hash (verify data in Finished messages)
//   3. Certificate signature verification (RSA PKCS#1 v1.5 with SHA-256)
//   4. HKDF (HMAC-based Key Derivation Function) for key expansion
//
// TEACHING NOTE: The Merkle-Damgard Construction
// SHA-256 uses the Merkle-Damgard construction, which works like this:
//   - The input is padded to a multiple of 512 bits (64 bytes)
//   - It is split into 512-bit blocks
//   - Each block is processed through a compression function
//   - The compression function takes the current state (256 bits) and a block (512 bits)
//     and produces a new 256-bit state
//   - The initial state is a set of fractional parts of square roots of primes
//   - The final state IS the hash output
//
// This sponge-like construction means that every bit of input affects every bit
// of output, and changing even one input bit completely changes the output
// (the avalanche effect).
//
// TEACHING NOTE: Security properties
//   - Pre-image resistance: given hash h, it is hard to find m such that SHA256(m) = h
//   - Second pre-image resistance: given m1, hard to find m2 != m1 with same hash
//   - Collision resistance: hard to find any m1, m2 with SHA256(m1) = SHA256(m2)
//
// All of these are believed to hold for SHA-256 with practical computational limits.

#ifndef CHINSTRAP_SHA256_HPP
#define CHINSTRAP_SHA256_HPP

#include <cstdint>
#include <cstddef>
#include <array>

namespace chinstrap {

// TEACHING NOTE: We use a fixed-size array for the hash output.
// 256 bits = 32 bytes. We also provide a hex string representation for convenience.
struct Sha256Digest {
    std::array<uint8_t, 32> bytes;

    // Default construct to all zeros
    Sha256Digest() : bytes{} {}

    // Compare two digests
    bool operator==(const Sha256Digest& other) const {
        return bytes == other.bytes;
    }
    bool operator!=(const Sha256Digest& other) const {
        return bytes != other.bytes;
    }
};

// TEACHING NOTE: The Sha256 class implements the standard hash interface:
//   - init(): set up initial state
//   - update(data, len): feed data in chunks
//   - final(): produce the digest
// This streaming interface allows hashing data larger than memory.

class Sha256 {
public:
    Sha256();
    ~Sha256();

    // Initialize the hash state
    void init();

    // Feed data into the hash
    void update(const uint8_t* data, size_t len);

    // Produce the final digest
    Sha256Digest final();

    // Convenience: hash a single buffer in one call
    static Sha256Digest hash(const uint8_t* data, size_t len);
    static Sha256Digest hash(const char* data, size_t len);

private:
    // TEACHING NOTE: Internal state
    // The state consists of 8x 32-bit words (256 bits total).
    // These are initialized with the fractional parts of the square roots
    // of the first 8 prime numbers (FIPS 180-4 Section 5.3.3).
    uint32_t state_[8];

    // Buffer for incomplete blocks (we need 64-byte blocks)
    uint8_t buffer_[64];
    size_t buffer_len_;

    // Total message length in bits (we track bytes and convert at the end)
    uint64_t total_len_;

    // Process one 64-byte block
    void process_block(const uint8_t* block);
};

// TEACHING NOTE: HMAC-SHA256
// HMAC (Hash-based Message Authentication Code) uses a hash function to create
// a MAC (Message Authentication Code). It is defined in RFC 2104 as:
//   HMAC(K, m) = H((K ^ opad) || H((K ^ ipad) || m))
// where K is the key (padded to block size), ipad = 0x36 repeated, opad = 0x5c repeated.
// HMAC is used in TLS for the PRF and for HKDF.

struct HmacSha256 {
    // Compute HMAC-SHA256
    static void compute(const uint8_t* key, size_t key_len,
                        const uint8_t* message, size_t msg_len,
                        uint8_t out[32]);

    // Convenience for multiple message segments
    // HMAC(K, m1 || m2 || ... || mn)
    static void compute(const uint8_t* key, size_t key_len,
                        const uint8_t* const* segments, const size_t* seg_lens,
                        size_t num_segments,
                        uint8_t out[32]);
};

} // namespace chinstrap

#endif // CHINSTRAP_SHA256_HPP