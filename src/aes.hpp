// aes.hpp - AES-128/256 block cipher with GCM authenticated encryption
// Part of Chinstrap, a from-scratch web browser in C++ with zero third-party libraries.
//
// TEACHING NOTE: What is AES?
// AES (Advanced Encryption Standard) is a symmetric block cipher defined in
// FIPS 197. It operates on 128-bit blocks and supports key sizes of 128, 192,
// and 256 bits. In TLS we use 128-bit and 256-bit keys.
//
// AES is a substitution-permutation network (SPN):
//   - Substitution: S-box (byte substitution)
//   - Permutation: ShiftRows, MixColumns (byte shuffling and mixing)
//   - Key mixing: AddRoundKey (XOR with round subkey)
//   - Multiple rounds: 10 for AES-128, 14 for AES-256
//
// TEACHING NOTE: What is GCM?
// GCM (Galois/Counter Mode) is an AEAD (Authenticated Encryption with
// Associated Data) mode defined in NIST SP 800-38D. It provides:
//   - Confidentiality: the ciphertext reveals nothing about the plaintext
//   - Integrity: any modification of ciphertext is detected
//   - Authentication: a tag proves the ciphertext was produced by the key holder
//
// GCM combines:
//   - CTR (Counter) mode for encryption: AES encrypts a counter, XOR with plaintext
//   - GHASH for authentication: a polynomial MAC over GF(2^128)
//
// TEACHING NOTE: Why AES-NI?
// AES-NI (Advanced Encryption Standard New Instructions) are CPU instructions
// that perform AES operations in hardware. They are available on all modern
// x86 and x86-64 processors (Intel since Westmere 2010, AMD since Bulldozer 2011).
// Using AES-NI is NOT using a library - it is using CPU instructions directly,
// just like using ADD or MUL. We access them through compiler intrinsics
// (functions that map to single CPU instructions).
//
// On ARM (like our Raspberry Pi), we use the ARMv8 Crypto Extensions (AESE, AESD,
// PMULL) which provide the same hardware acceleration. We detect these at compile time.
//
// TEACHING NOTE: Why is GCM used in TLS?
// In TLS 1.2, AES-GCM is the most common AEAD cipher suite:
//   - TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
//   - TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
// In TLS 1.3, AES-GCM is one of the mandatory cipher suites.
// GCM provides both encryption and authentication in one operation, which is
// more efficient and less error-prone than combining separate ciphers and MACs.

#ifndef CHINSTRAP_AES_HPP
#define CHINSTRAP_AES_HPP

#include <cstdint>
#include <cstddef>
#include <array>

namespace chinstrap {

// AES block size is always 128 bits (16 bytes) regardless of key size
constexpr size_t AES_BLOCK_SIZE = 16;

// TEACHING NOTE: AES key sizes
// AES-128: 128-bit key, 10 rounds
// AES-256: 256-bit key, 14 rounds
// More rounds with a longer key means more security but slightly slower.
// We support both because TLS cipher suites use both.

enum class AesKeySize {
    Aes128, // 16-byte key, 10 rounds
    Aes256  // 32-byte key, 14 rounds
};

// TEACHING NOTE: The AES class handles block cipher operations (encrypt/decrypt
// a single 16-byte block). GCM mode (which uses CTR + GHASH) is handled separately.
class Aes {
public:
    Aes();
    ~Aes();

    // Initialize with key. Returns false on invalid key size.
    bool init(const uint8_t* key, size_t key_len, AesKeySize key_size);

    // Encrypt a single 16-byte block
    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;

    // Decrypt a single 16-byte block
    void decrypt_block(const uint8_t in[16], uint8_t out[16]) const;

private:
    AesKeySize key_size_;
    int num_rounds_;

    // TEACHING NOTE: Round keys
    // The AES key schedule expands the original key into round keys.
    // Each round uses a different 128-bit subkey derived from the original key.
    // AES-128: 11 round keys (44 words), AES-256: 15 round keys (60 words)
    // We store the maximum (60 words = 240 bytes).
    uint32_t round_keys_[60];

    void key_expansion_128(const uint8_t* key);
    void key_expansion_256(const uint8_t* key);

#if defined(__AES__) || defined(__ARM_FEATURE_CRYPTO)
    bool hw_accel_; // true if hardware AES is available
    void init_hw(const uint8_t* key);
#endif
};

// TEACHING NOTE: GCM Mode
// GCM (Galois/Counter Mode) is defined in NIST SP 800-38D.
//
// Encryption:
//   1. Compute H = AES_ECB(0^128) - the hash subkey
//   2. Compute J0 = counter from nonce and invocation counter
//   3. Encrypt using CTR mode starting from J0+1
//   4. Compute GHASH over AAD and ciphertext
//   5. Compute tag = AES_ECB(J0) XOR GHASH
//
// GHASH is a polynomial evaluation over GF(2^128):
//   G = 0
//   For each block X_i: G = (G XOR X_i) * H
//   where * is multiplication in GF(2^128) with the reduction polynomial
//   x^128 + x^7 + x^2 + x + 1
//
// The GF(2^128) multiplication is the trickiest part. We implement it
// using bit-by-bit multiplication (simple but correct). Hardware-accelerated
// versions use the PCLMULQDQ instruction (x86) or PMULL (ARM).

struct AesGcm {
    // TEACHING NOTE: AEAD encrypt
    // Encrypts plaintext with key and nonce (IV).
    // AAD (Additional Authenticated Data) is authenticated but not encrypted.
    // The tag proves the ciphertext was not modified.
    //
    // Parameters:
    //   key: AES key (16 or 32 bytes)
    //   key_len: key length in bytes
    //   key_size: AesKeySize::Aes128 or AesKeySize::Aes256
    //   iv: initialization vector, 12 bytes (96-bit nonce is standard for TLS)
    //   aad: additional authenticated data (authenticated but not encrypted)
    //   aad_len: length of AAD
    //   plaintext: data to encrypt
    //   pt_len: plaintext length
    //   ciphertext: output buffer, same size as plaintext
    //   tag: output 16-byte authentication tag
    //
    // Returns true on success, false on error.
    static bool encrypt(const uint8_t* key, size_t key_len, AesKeySize key_size,
                        const uint8_t iv[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* plaintext, size_t pt_len,
                        uint8_t* ciphertext,
                        uint8_t tag[16]);

    // TEACHING NOTE: AEAD decrypt
    // Decrypts ciphertext and verifies the tag.
    // If the tag does not match, returns false and does not produce plaintext.
    //
    // Parameters: same as encrypt, but ciphertext -> plaintext
    // Returns true on success (tag verified), false on tag mismatch or error.
    static bool decrypt(const uint8_t* key, size_t key_len, AesKeySize key_size,
                        const uint8_t iv[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* ciphertext, size_t ct_len,
                        const uint8_t tag[16],
                        uint8_t* plaintext);
};

} // namespace chinstrap

#endif // CHINSTRAP_AES_HPP