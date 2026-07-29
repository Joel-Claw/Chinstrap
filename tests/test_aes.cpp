// test_aes.cpp - AES and AES-GCM test vectors from FIPS 197 and NIST SP 800-38D
// Part of Chinstrap test suite.
//
// TEACHING NOTE: Testing AES
// AES has well-defined test vectors in FIPS 197. We test:
//   1. AES-128 ECB encryption of a known plaintext/key
//   2. AES-256 ECB encryption of a known plaintext/key
//   3. AES-128 decryption (verify round trip)
//   4. AES-256 decryption (verify round trip)
//
// For GCM mode, we test against the NIST GCM test vectors from SP 800-38D.
// These verify both the encryption and the authentication tag.
//
// TEACHING NOTE: Why ECB for block tests?
// We test AES in ECB (Electronic Codebook) mode for single-block tests because
// ECB is the simplest mode: just apply the block cipher directly. GCM mode
// uses the block cipher internally, so if the block cipher is correct, GCM
// correctness depends only on the CTR mode and GHASH implementation.
// We test GCM separately with its own known answer tests.

#include "../src/aes.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace chinstrap;

static int tests_passed = 0;
static int tests_failed = 0;

static void check_block(const char* name, const uint8_t expected[16],
                         const uint8_t actual[16]) {
    if (memcmp(expected, actual, 16) == 0) {
        printf("  PASS: %s\n", name);
        tests_passed++;
    } else {
        printf("  FAIL: %s\n", name);
        printf("    Expected: ");
        for (int i = 0; i < 16; i++) printf("%02x", expected[i]);
        printf("\n    Got:      ");
        for (int i = 0; i < 16; i++) printf("%02x", actual[i]);
        printf("\n");
        tests_failed++;
    }
}

static void check_gcm(const char* name, bool result,
                       const uint8_t* expected_ct, size_t ct_len,
                       const uint8_t* actual_ct,
                       const uint8_t expected_tag[16],
                       const uint8_t actual_tag[16]) {
    bool ct_ok = (memcmp(expected_ct, actual_ct, ct_len) == 0);
    bool tag_ok = (memcmp(expected_tag, actual_tag, 16) == 0);
    if (result && ct_ok && tag_ok) {
        printf("  PASS: %s\n", name);
        tests_passed++;
    } else {
        printf("  FAIL: %s (result=%d, ct_ok=%d, tag_ok=%d)\n", name, result, ct_ok, tag_ok);
        tests_failed++;
    }
}

// TEACHING NOTE: FIPS 197 test vectors
//
// AES-128 test vector (FIPS 197 Appendix B):
//   Key:          000102030405060708090a0b0c0d0e0f
//   Plaintext:    00112233445566778899aabbccddeeff
//   Ciphertext:   69c4e0d86a7b0430d8cdb78070b4c55a
//
// AES-256 test vector (FIPS 197 Appendix C.3):
//   Key:          000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
//   Plaintext:    00112233445566778899aabbccddeeff
//   Ciphertext:   8ea2b7ca516745bfeafc49904b496089

static void test_aes128_encrypt() {
    uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    uint8_t plaintext[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                              0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    uint8_t expected[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                             0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};

    Aes aes;
    aes.init(key, 16, AesKeySize::Aes128);

    uint8_t ciphertext[16];
    aes.encrypt_block(plaintext, ciphertext);
    check_block("AES-128 Encrypt (FIPS 197)", expected, ciphertext);
}

static void test_aes256_encrypt() {
    uint8_t key[32] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
                        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f};
    uint8_t plaintext[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                              0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    uint8_t expected[16] = {0x8e,0xa2,0xb7,0xca,0x51,0x67,0x45,0xbf,
                             0xea,0xfc,0x49,0x90,0x4b,0x49,0x60,0x89};

    Aes aes;
    aes.init(key, 32, AesKeySize::Aes256);

    uint8_t ciphertext[16];
    aes.encrypt_block(plaintext, ciphertext);
    check_block("AES-256 Encrypt (FIPS 197)", expected, ciphertext);
}

static void test_aes128_decrypt() {
    // TEACHING NOTE: AES decryption round trip
    // Encrypt a known plaintext, then decrypt it back and verify we get
    // the original plaintext. This tests both the encrypt and decrypt
    // paths and the key schedule.

    uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    uint8_t plaintext[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                              0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};

    Aes aes;
    aes.init(key, 16, AesKeySize::Aes128);

    uint8_t ciphertext[16], decrypted[16];
    aes.encrypt_block(plaintext, ciphertext);
    aes.decrypt_block(ciphertext, decrypted);
    check_block("AES-128 Decrypt Round Trip", plaintext, decrypted);
}

static void test_aes256_decrypt() {
    uint8_t key[32] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
                        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f};
    uint8_t plaintext[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                              0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};

    Aes aes;
    aes.init(key, 32, AesKeySize::Aes256);

    uint8_t ciphertext[16], decrypted[16];
    aes.encrypt_block(plaintext, ciphertext);
    aes.decrypt_block(ciphertext, decrypted);
    check_block("AES-256 Decrypt Round Trip", plaintext, decrypted);
}

// TEACHING NOTE: NIST GCM test vectors
//
// These test vectors are verified against the Python cryptography library
// (which uses OpenSSL/AES-NI under the hood) to ensure correctness.
//
// Test Case 1 (AES-128-GCM, empty plaintext, empty AAD):
//   Key:          00000000000000000000000000000000
//   IV:           000000000000000000000000
//   Plaintext:    (empty)
//   AAD:          (empty)
//   Ciphertext:   (empty)
//   Tag:          58e2fccefa7e3061367f1d57a4e7455a
//
// Test Case 2 (AES-128-GCM, single zero block, empty AAD):
//   Key:          00000000000000000000000000000000
//   IV:           000000000000000000000000
//   Plaintext:    00000000000000000000000000000000
//   AAD:          (empty)
//   Ciphertext:   0388dace60b6a392f328c2b971b2fe78
//   Tag:          ab6e47d42cec13bdf53a67b21257bddf

static void test_gcm_empty() {
    // AES-128-GCM with empty plaintext and AAD
    uint8_t key[16] = {0};
    uint8_t iv[12] = {0};

    uint8_t tag[16];
    uint8_t expected_tag[16] = {0x58,0xe2,0xfc,0xce,0xfa,0x7e,0x30,0x61,
                                 0x36,0x7f,0x1d,0x57,0xa4,0xe7,0x45,0x5a};

    bool result = AesGcm::encrypt(key, 16, AesKeySize::Aes128, iv,
                                   nullptr, 0, nullptr, 0,
                                   nullptr, tag);
    check_gcm("AES-128-GCM Empty", result,
              reinterpret_cast<const uint8_t*>(""), 0,
              reinterpret_cast<const uint8_t*>(""),
              expected_tag, tag);
}

static void test_gcm_single_block_128() {
    // AES-128-GCM with single 16-byte block
    uint8_t key[16] = {0};
    uint8_t iv[12] = {0};
    uint8_t plaintext[16] = {0};

    uint8_t ciphertext[16];
    uint8_t tag[16];
    uint8_t expected_ct[16] = {0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,
                                0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78};
    uint8_t expected_tag[16] = {0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,
                                 0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};

    bool result = AesGcm::encrypt(key, 16, AesKeySize::Aes128, iv,
                                   nullptr, 0, plaintext, 16,
                                   ciphertext, tag);
    check_gcm("AES-128-GCM Single Block", result,
              expected_ct, 16, ciphertext,
              expected_tag, tag);
}

static void test_gcm_decrypt_128() {
    // TEACHING NOTE: GCM decryption round trip
    // Encrypt plaintext, then decrypt it and verify we get the original back.
    uint8_t key[32] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
                        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f};
    uint8_t iv[12] = {0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88};
    const char* plaintext_str = "Hello, TLS! This is a test.";
    size_t pt_len = strlen(plaintext_str);
    const char* aad_str = "additional data";
    size_t aad_len = strlen(aad_str);

    std::vector<uint8_t> ciphertext(pt_len);
    uint8_t tag[16];

    bool enc_result = AesGcm::encrypt(key, 32, AesKeySize::Aes256, iv,
                                       reinterpret_cast<const uint8_t*>(aad_str), aad_len,
                                       reinterpret_cast<const uint8_t*>(plaintext_str), pt_len,
                                       ciphertext.data(), tag);

    if (!enc_result) {
        printf("  FAIL: AES-256-GCM Decrypt Round Trip (encrypt failed)\n");
        tests_failed++;
        return;
    }

    std::vector<uint8_t> decrypted(pt_len);
    bool dec_result = AesGcm::decrypt(key, 32, AesKeySize::Aes256, iv,
                                       reinterpret_cast<const uint8_t*>(aad_str), aad_len,
                                       ciphertext.data(), pt_len, tag,
                                       decrypted.data());

    if (!dec_result) {
        printf("  FAIL: AES-256-GCM Decrypt Round Trip (decrypt/tag verify failed)\n");
        tests_failed++;
        return;
    }

    if (memcmp(plaintext_str, decrypted.data(), pt_len) == 0) {
        printf("  PASS: AES-256-GCM Decrypt Round Trip\n");
        tests_passed++;
    } else {
        printf("  FAIL: AES-256-GCM Decrypt Round Trip (plaintext mismatch)\n");
        tests_failed++;
    }
}

static void test_gcm_tamper_detection() {
    // TEACHING NOTE: GCM tamper detection
    // This is the most important GCM security test. We encrypt data, flip a
    // single bit in the ciphertext, and verify that decryption fails (tag
    // mismatch). This proves that GCM provides integrity - any tampering
    // is detected.

    uint8_t key[16] = {0x42};
    uint8_t iv[12] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c};
    const char* plaintext = "Integrity check!";
    size_t pt_len = strlen(plaintext);

    std::vector<uint8_t> ciphertext(pt_len);
    uint8_t tag[16];

    AesGcm::encrypt(key, 16, AesKeySize::Aes128, iv,
                     nullptr, 0,
                     reinterpret_cast<const uint8_t*>(plaintext), pt_len,
                     ciphertext.data(), tag);

    // Flip one bit in the ciphertext
    ciphertext[0] ^= 0x01;

    std::vector<uint8_t> decrypted(pt_len);
    bool result = AesGcm::decrypt(key, 16, AesKeySize::Aes128, iv,
                                    nullptr, 0,
                                    ciphertext.data(), pt_len, tag,
                                    decrypted.data());

    if (!result) {
        printf("  PASS: AES-128-GCM Tamper Detection\n");
        tests_passed++;
    } else {
        printf("  FAIL: AES-128-GCM Tamper Detection (tampered ciphertext was accepted!)\n");
        tests_failed++;
    }
}

int main() {
    printf("=== AES and AES-GCM Test Suite ===\n\n");

    printf("FIPS 197 Block Cipher Tests:\n");
    test_aes128_encrypt();
    test_aes256_encrypt();
    test_aes128_decrypt();
    test_aes256_decrypt();

    printf("\nNIST SP 800-38D GCM Tests:\n");
    test_gcm_empty();
    test_gcm_single_block_128();
    test_gcm_decrypt_128();

    printf("\nSecurity Tests:\n");
    test_gcm_tamper_detection();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);

    return (tests_failed == 0) ? 0 : 1;
}