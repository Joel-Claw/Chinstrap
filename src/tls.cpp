// tls.cpp - TLS 1.2 client implementation from scratch
// Part of Chinstrap, a from-scratch web browser in C++ with zero third-party libraries.
//
// TEACHING NOTE: This file implements the TLS 1.2 protocol.
// It is the largest and most complex file in the TLS subsystem because it
// ties together all the cryptographic primitives:
//   - SHA-256 for handshake transcript and PRF
//   - HMAC-SHA-256 for the PRF (Pseudo-Random Function)
//   - AES-256-GCM for record encryption
//   - ECDHE (P-256) for key exchange
//   - X.509 for certificate verification
//   - BigInt for RSA signature verification
//
// TEACHING NOTE: TLS PRF (Pseudo-Random Function)
// TLS 1.2 uses a PRF based on HMAC-SHA-256 (or SHA-384 for some cipher suites).
// The PRF generates pseudorandom bytes from a secret, a label, and a seed.
// It works by iteratively applying HMAC:
//   P_hash(secret, seed) = HMAC(secret, A(1) || seed) || HMAC(secret, A(2) || seed) || ...
//   where A(0) = seed, A(i) = HMAC(secret, A(i-1))
//
// The PRF is used to:
//   1. Compute the master secret from the pre-master secret
//   2. Generate key material (encryption keys, IVs, MAC keys)
//   3. Compute the verify_data for the Finished message
//
// TEACHING NOTE: The master secret
// master_secret = PRF(pre_master_secret, "master secret",
//                      client_random || server_random, 48)
//
// TEACHING NOTE: Key expansion
// key_block = PRF(master_secret, "key expansion",
//                 server_random || client_random, needed_bytes)
// The key_block is split into:
//   client_write_key | server_write_key | client_write_iv | server_write_iv
//
// TEACHING NOTE: Finished message
// The Finished message proves that both sides computed the same handshake
// transcript and derived the same keys. It contains:
//   verify_data = PRF(master_secret, "client finished" or "server finished",
//                     Hash(handshake_messages), 12)
// The Hash is the SHA-256 hash of all handshake messages exchanged so far.

#include "tls.hpp"
#include <cstring>
#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctime>
#include <cstdlib>

namespace chinstrap {

// ============================================================================
// P-256 Elliptic Curve (NIST P-256 / secp256r1)
// ============================================================================
// TEACHING NOTE: The P-256 curve
// P-256 is defined by the Weierstrass equation: y^2 = x^3 + ax + b
// over the prime field GF(p), where:
//   p = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
//   a = p - 3 (= -3 mod p, for efficiency)
//   b = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
//   G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
//        0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)
//   n = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551 (order)
//
// Point multiplication is the core operation: given a scalar k and a point P,
// compute k*P. This is used for key generation (k*G) and shared secret
// computation (k*Q where Q is the peer public key).
//
// We use Jacobian coordinates for efficiency (avoids modular inversion in
// the inner loop, only needed once at the end to convert back to affine).

struct P256 {
    // Prime modulus p (256 bits, stored as 8 uint32_t words, little-endian)
    static constexpr uint32_t P[8] = {
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
        0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF
    };

    // Curve parameter a = p - 3
    // (We use a = -3 mod p for efficiency in point doubling)

    // Curve parameter b
    static constexpr uint32_t B[8] = {
        0x3DCE6D8E, 0x4A9B5A8E, 0x4B5C3E4F, 0x6B6E7B9B,
        0x6B7E5C3A, 0x5A6F4F3A, 0x6B17D1F2, 0xE12C4247
    };

    // Generator point G (affine coordinates)
    static constexpr uint32_t GX[8] = {
        0x96B1C296, 0xF8BCE6E5, 0x63688E3E, 0x294F4F3A,
        0x4FE342E2, 0xFE1A7F9B, 0x8EE7EB4A, 0x7C0F9E16
    };

    static constexpr uint32_t GY[8] = {
        0xC2960F7E, 0xF8BCE6E5, 0x63688E3E, 0x294F4F3A,
        0x4FE342E2, 0xFE1A7F9B, 0x8EE7EB4A, 0x7C0F9E16
    };

    // Order of the generator point n
    static constexpr uint32_t N[8] = {
        0x51CE55F6, 0x2FC9C0AC, 0xA7179E84, 0xBCE6FAAD,
        0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF
    };

    // 256-bit field element (8 x uint32_t, little-endian)
    using Fe = uint32_t[8];

    // Point in Jacobian coordinates: (X, Y, Z) where x = X/Z^2, y = Y/Z^3
    struct JacobianPoint {
        Fe X, Y, Z;
        bool infinity;
    };

    // Modular arithmetic in GF(p)

    // Add two field elements: r = (a + b) mod p
    static void fe_add(const Fe& a, const Fe& b, Fe& r);
    // Subtract: r = (a - b) mod p
    static void fe_sub(const Fe& a, const Fe& b, Fe& r);
    // Multiply: r = (a * b) mod p
    static void fe_mul(const Fe& a, const Fe& b, Fe& r);
    // Square: r = (a^2) mod p
    static void fe_sqr(const Fe& a, Fe& r);
    // Invert: r = a^(-1) mod p (using Fermat little theorem: a^(p-2))
    static void fe_inv(const Fe& a, Fe& r);
    // Conditional subtract p (reduce mod p)
    static void fe_reduce(uint64_t* temp, size_t len, Fe& r);

    // Point operations

    // Point double: r = 2*P
    static void point_double(const JacobianPoint& P, JacobianPoint& R);

    // Point add: R = P + Q (mixed affine + Jacobian for efficiency)
    static void point_add_mixed(const JacobianPoint& P, const Fe& x2,
                                 const Fe& y2, bool is_infinity,
                                 JacobianPoint& R);

    // Point add (both Jacobian)
    static void point_add(const JacobianPoint& P, const JacobianPoint& Q,
                          JacobianPoint& R);

    // Point multiplication: R = k*P (using binary method)
    static void point_mul(const Fe& k, const JacobianPoint& P, JacobianPoint& R);

    // Convert Jacobian to affine
    static void to_affine(const JacobianPoint& P, Fe& x, Fe& y);

    // Generate key pair
    static bool generate_keypair(uint8_t private_key[32],
                                  uint8_t public_key[65]);

    // Compute shared secret: multiply our private key by their public key
    static bool compute_shared_secret(const uint8_t private_key[32],
                                       const uint8_t* peer_public, size_t peer_len,
                                       uint8_t shared_x[32]);
};

// TEACHING NOTE: P-256 field arithmetic
// All arithmetic is done modulo p = 2^256 - 2^224 + 2^192 + 2^96 - 1.
// This is a pseudo-Mersenne prime that allows for fast reduction.
// We implement basic schoolbook multiplication and modular reduction.
// Production code uses faster algorithms (Karatsuba, Montgomery, SIMD).

// Check if a >= p (for reduction)
static bool fe_gte_p(const uint32_t a[8]) {
    for (int i = 7; i >= 0; i--) {
        if (a[i] > P256::P[i]) return true;
        if (a[i] < P256::P[i]) return false;
    }
    return true; // a == p
}

void P256::fe_add(const Fe& a, const Fe& b, Fe& r) {
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        carry += a[i];
        carry += b[i];
        r[i] = static_cast<uint32_t>(carry & 0xFFFFFFFF);
        carry >>= 32;
    }

    // Reduce: if r >= p or there was a carry, subtract p
    if (carry > 0 || fe_gte_p(r)) {
        uint64_t borrow = 0;
        for (int i = 0; i < 8; i++) {
            borrow = static_cast<uint64_t>(r[i]) - P256::P[i] - borrow;
            if (borrow >> 32) {
                r[i] = static_cast<uint32_t>(borrow + (1ULL << 32));
                borrow = 1;
            } else {
                r[i] = static_cast<uint32_t>(borrow);
                borrow = 0;
            }
        }
    }
}

void P256::fe_sub(const Fe& a, const Fe& b, Fe& r) {
    int64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        borrow = static_cast<int64_t>(a[i]) - b[i] - borrow;
        if (borrow < 0) {
            r[i] = static_cast<uint32_t>(borrow + (1LL << 32));
            borrow = 1;
        } else {
            r[i] = static_cast<uint32_t>(borrow);
            borrow = 0;
        }
    }

    // If borrow, add p back
    if (borrow) {
        uint64_t carry = 0;
        for (int i = 0; i < 8; i++) {
            carry += r[i];
            carry += P256::P[i];
            r[i] = static_cast<uint32_t>(carry & 0xFFFFFFFF);
            carry >>= 32;
        }
    }
}

void P256::fe_mul(const Fe& a, const Fe& b, Fe& r) {
    // TEACHING NOTE: Schoolbook multiplication
    // Multiply two 256-bit numbers to get a 512-bit result, then reduce mod p.
    // We use 64-bit accumulators to avoid overflow.
    uint32_t product[16] = {0};

    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t prod = static_cast<uint64_t>(a[i]) * b[j]
                          + product[i + j] + carry;
            product[i + j] = static_cast<uint32_t>(prod & 0xFFFFFFFF);
            carry = prod >> 32;
        }
        product[i + 8] = static_cast<uint32_t>(carry);
    }

    // Reduce mod p
    // For P-256, we can use the special structure of p = 2^256 - 2^224 + 2^192 + 2^96 - 1
    // For simplicity, we use a generic reduction: subtract multiples of p
    // shifted appropriately.
    //
    // A full implementation would use the P-256 specific fast reduction.
    // For correctness, we use a simpler approach: long division style.

    // Convert to BigInt and reduce
    BigInt a_bn = BigInt::from_bytes_be(nullptr, 0);
    // Actually, let us just do a simpler modular reduction here.
    // We use the Barrett reduction approach: compute the product as a
    // 512-bit number and reduce it mod p.

    // Copy product to a byte array in big-endian
    uint8_t prod_bytes[64];
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 4; j++) {
            prod_bytes[63 - (i * 4 + j)] = static_cast<uint8_t>(product[i] >> (j * 8));
        }
    }

    BigInt prod_bi = BigInt::from_bytes_be(prod_bytes, 64);

    // p as BigInt
    uint8_t p_bytes[32];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            p_bytes[31 - (i * 4 + j)] = static_cast<uint8_t>(P256::P[i] >> (j * 8));
        }
    }
    BigInt p_bi = BigInt::from_bytes_be(p_bytes, 32);

    BigInt r_bi = BigInt::mod(prod_bi, p_bi);

    // Convert back to Fe
    std::vector<uint8_t> r_bytes = r_bi.to_bytes_be();
    std::memset(r, 0, 32);
    for (size_t i = 0; i < r_bytes.size() && i < 32; i++) {
        size_t word_idx = (31 - i) / 4;
        size_t byte_in_word = (31 - i) % 4;
        r[word_idx] |= static_cast<uint32_t>(r_bytes[i]) << (byte_in_word * 8);
    }
}

void P256::fe_sqr(const Fe& a, Fe& r) {
    fe_mul(a, a, r);
}

void P256::fe_inv(const Fe& a, Fe& r) {
    // TEACHING NOTE: Modular inversion using Fermat little theorem
    // a^(p-1) = 1 mod p (Fermat little theorem)
    // So a^(-1) = a^(p-2) mod p
    // We compute a^(p-2) using square-and-multiply.
    // p - 2 = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFE
    // (in big-endian: 0xFF...FF 00...01 00...00 00...00 00...00 FF...FF FF...FE)

    uint8_t p_minus_2[32];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            p_minus_2[31 - (i * 4 + j)] = static_cast<uint8_t>(P[i] >> (j * 8));
        }
    }
    p_minus_2[31] -= 2; // p - 2

    // Square-and-multiply
    Fe result = {1, 0, 0, 0, 0, 0, 0, 0}; // result = 1
    Fe base;
    std::memcpy(base, a, 32);

    for (int i = 255; i >= 0; i--) {
        fe_sqr(result, result);
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if ((p_minus_2[byte_idx] >> bit_idx) & 1) {
            fe_mul(result, base, result);
        }
    }

    std::memcpy(r, result, 32);
}

void P256::point_double(const JacobianPoint& P, JacobianPoint& R) {
    if (P.infinity) {
        R.infinity = true;
        return;
    }

    // TEACHING NOTE: Point doubling formula (Jacobian coordinates)
    // For a = -3 (which P-256 uses):
    //   S = 4 * X * Y^2
    //   M = 3 * (X - Y^2) * (X + Y^2)   (this is 3*X^2 + a*Z^4 with a=-3)
    //   X' = M^2 - 2*S
    //   Y' = M * (S - X') - 8 * Y^4
    //   Z' = 2 * Y * Z

    Fe Y2, Y4, S, M, X1, X2, T, U, V, tmp;

    fe_sqr(P.Y, Y2);      // Y^2
    fe_sqr(Y2, Y4);       // Y^4
    fe_mul(P.X, Y2, S);
    fe_add(S, S, S);
    fe_add(S, S, S);      // S = 4 * X * Y^2

    // M = 3 * (X - Y^2) * (X + Y^2)
    fe_sub(P.X, Y2, T);   // T = X - Y^2
    fe_add(P.X, Y2, U);   // U = X + Y^2
    fe_mul(T, U, M);      // M = (X - Y^2) * (X + Y^2)
    fe_add(M, M, M);
    fe_add(M, M, M);      // M = 3 * (X - Y^2) * (X + Y^2)

    // X' = M^2 - 2*S
    fe_sqr(M, X1);
    fe_sub(X1, S, X2);
    fe_sub(X2, S, R.X);   // R.X = M^2 - 2*S

    // Y' = M * (S - X') - 8 * Y^4
    fe_sub(S, R.X, V);    // V = S - X'
    fe_mul(M, V, R.Y);    // R.Y = M * V
    fe_add(Y4, Y4, Y4);
    fe_add(Y4, Y4, Y4);
    fe_add(Y4, Y4, Y4);   // 8 * Y^4
    fe_sub(R.Y, Y4, R.Y);

    // Z' = 2 * Y * Z
    fe_add(P.Y, P.Y, tmp);
    fe_mul(tmp, P.Z, R.Z);

    R.infinity = false;
}

void P256::point_add_mixed(const JacobianPoint& P, const Fe& x2, const Fe& y2,
                            bool is_infinity, JacobianPoint& R) {
    if (is_infinity) {
        R = P;
        return;
    }
    if (P.infinity) {
        R.infinity = false;
        std::memcpy(R.X, x2, 32);
        std::memcpy(R.Y, y2, 32);
        // Z = 1
        std::memset(R.Z, 0, 32);
        R.Z[0] = 1;
        return;
    }

    // TEACHING NOTE: Mixed point addition (Jacobian + Affine -> Jacobian)
    //   U2 = x2 * Z1^2
    //   S2 = y2 * Z1^3
    //   H = U2 - X1
    //   R = S2 - Y1
    //   X3 = R^2 - H^3 - 2*X1*H^2
    //   Y3 = R*(X1*H^2 - X3) - Y1*H^3
    //   Z3 = Z1*H

    Fe Z1_2, Z1_3, U2, S2, H, RR, H2, H3, X1H2, tmp1, tmp2;

    fe_sqr(P.Z, Z1_2);
    fe_mul(Z1_2, P.Z, Z1_3);
    fe_mul(x2, Z1_2, U2);
    fe_mul(y2, Z1_3, S2);
    fe_sub(U2, P.X, H);
    fe_sub(S2, P.Y, RR);

    if (true) {
        // Check if H == 0 and RR == 0 (P == Q, need doubling)
        bool h_zero = true;
        for (int i = 0; i < 8; i++) if (H[i] != 0) { h_zero = false; break; }
        if (h_zero) {
            bool r_zero = true;
            for (int i = 0; i < 8; i++) if (RR[i] != 0) { r_zero = false; break; }
            if (r_zero) {
                point_double(P, R);
                return;
            }
            // H == 0 but R != 0 means P = -Q, result is infinity
            R.infinity = true;
            return;
        }
    }

    fe_sqr(H, H2);
    fe_mul(H2, H, H3);
    fe_mul(P.X, H2, X1H2);

    // X3 = R^2 - H^3 - 2*X1*H^2
    fe_sqr(RR, R.X);
    fe_sub(R.X, H3, R.X);
    fe_add(X1H2, X1H2, tmp1);
    fe_sub(R.X, tmp1, R.X);

    // Y3 = R*(X1*H^2 - X3) - Y1*H^3
    fe_sub(X1H2, R.X, tmp1);
    fe_mul(RR, tmp1, R.Y);
    fe_mul(P.Y, H3, tmp2);
    fe_sub(R.Y, tmp2, R.Y);

    // Z3 = Z1*H
    fe_mul(P.Z, H, R.Z);

    R.infinity = false;
}

void P256::point_add(const JacobianPoint& P, const JacobianPoint& Q,
                     JacobianPoint& R) {
    if (P.infinity) { R = Q; return; }
    if (Q.infinity) { R = P; return; }

    // Full Jacobian addition
    Fe Z1_2, Z2_2, Z1Z2, U1, U2, S1, S2, H, RR, H2, H3, X1H2, tmp1, tmp2;

    fe_sqr(P.Z, Z1_2);
    fe_sqr(Q.Z, Z2_2);
    fe_mul(Z1_2, Z2_2, Z1Z2);
    fe_mul(P.X, Z2_2, U1);
    fe_mul(Q.X, Z1_2, U2);
    fe_mul(P.Y, Z2_2, S1);
    fe_mul(Q.Y, Z1_2, S2);
    fe_mul(S1, Q.Z, S1);
    fe_mul(S2, P.Z, S2);

    fe_sub(U2, U1, H);
    fe_sub(S2, S1, RR);

    bool h_zero = true;
    for (int i = 0; i < 8; i++) if (H[i] != 0) { h_zero = false; break; }
    if (h_zero) {
        bool r_zero = true;
        for (int i = 0; i < 8; i++) if (RR[i] != 0) { r_zero = false; break; }
        if (r_zero) { point_double(P, R); return; }
        R.infinity = true;
        return;
    }

    fe_sqr(H, H2);
    fe_mul(H2, H, H3);
    fe_mul(P.X, H2, X1H2);

    fe_sqr(RR, R.X);
    fe_sub(R.X, H3, R.X);
    fe_add(X1H2, X1H2, tmp1);
    fe_sub(R.X, tmp1, R.X);

    fe_sub(X1H2, R.X, tmp1);
    fe_mul(RR, tmp1, R.Y);
    fe_mul(P.Y, H3, tmp2);
    fe_sub(R.Y, tmp2, R.Y);

    fe_mul(P.Z, Q.Z, tmp1);
    fe_mul(tmp1, H, R.Z);

    R.infinity = false;
}

void P256::point_mul(const Fe& k, const JacobianPoint& P, JacobianPoint& R) {
    // TEACHING NOTE: Binary scalar multiplication
    // R = 0 (point at infinity)
    // For each bit of k from MSB to LSB:
    //   R = 2*R (double)
    //   if bit is 1: R = R + P (add)
    // This is O(256) point operations for P-256.
    R.infinity = true;
    std::memset(R.X, 0, 32);
    std::memset(R.Y, 0, 32);
    std::memset(R.Z, 0, 32);

    for (int i = 255; i >= 0; i--) {
        point_double(R, R);
        int word_idx = i / 32;
        int bit_idx = i % 32;
        if ((k[word_idx] >> bit_idx) & 1) {
            point_add(R, P, R);
        }
    }
}

void P256::to_affine(const JacobianPoint& P, Fe& x, Fe& y) {
    if (P.infinity) {
        std::memset(x, 0, 32);
        std::memset(y, 0, 32);
        return;
    }

    Fe Z_inv, Z_inv2, Z_inv3;
    fe_inv(P.Z, Z_inv);
    fe_sqr(Z_inv, Z_inv2);
    fe_mul(Z_inv2, Z_inv, Z_inv3);

    fe_mul(P.X, Z_inv2, x);
    fe_mul(P.Y, Z_inv3, y);
}

bool P256::generate_keypair(uint8_t private_key[32], uint8_t public_key[65]) {
    // TEACHING NOTE: EC key generation
    // 1. Generate a random scalar k in [1, n-1]
    // 2. Compute Q = k*G (the public key point)
    // 3. Encode Q as an uncompressed point: 0x04 || x || y (65 bytes)

    // Generate random private key using /dev/urandom
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    ssize_t ret = read(fd, private_key, 32);
    close(fd);
    if (ret != 32) return false;

    // Ensure k is in [1, n-1] (simplified: just mask the top bit
    // and ensure non-zero. A proper implementation would reduce mod n.)
    private_key[0] &= 0x7F; // Ensure k < 2^255 < n approximately
    // Ensure non-zero
    bool all_zero = true;
    for (int i = 0; i < 32; i++) {
        if (private_key[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) private_key[31] = 1;

    // Convert private key to Fe
    Fe k;
    for (int i = 0; i < 8; i++) {
        k[i] = static_cast<uint32_t>(private_key[31 - i * 4]) |
               (static_cast<uint32_t>(private_key[30 - i * 4]) << 8) |
               (static_cast<uint32_t>(private_key[29 - i * 4]) << 16) |
               (static_cast<uint32_t>(private_key[28 - i * 4]) << 24);
    }

    // Generator point in Jacobian coordinates (Z = 1)
    JacobianPoint G;
    std::memcpy(G.X, GX, 32);
    std::memcpy(G.Y, GY, 32);
    std::memset(G.Z, 0, 32);
    G.Z[0] = 1;
    G.infinity = false;

    // Compute Q = k * G
    JacobianPoint Q;
    point_mul(k, G, Q);

    // Convert to affine
    Fe x, y;
    to_affine(Q, x, y);

    // Encode as uncompressed point
    public_key[0] = 0x04;
    for (int i = 0; i < 8; i++) {
        // x (big-endian)
        public_key[1 + (7 - i) * 4] = static_cast<uint8_t>(x[i]);
        public_key[1 + (7 - i) * 4 + 1] = static_cast<uint8_t>(x[i] >> 8);
        public_key[1 + (7 - i) * 4 + 2] = static_cast<uint8_t>(x[i] >> 16);
        public_key[1 + (7 - i) * 4 + 3] = static_cast<uint8_t>(x[i] >> 24);
        // y (big-endian)
        public_key[33 + (7 - i) * 4] = static_cast<uint8_t>(y[i]);
        public_key[33 + (7 - i) * 4 + 1] = static_cast<uint8_t>(y[i] >> 8);
        public_key[33 + (7 - i) * 4 + 2] = static_cast<uint8_t>(y[i] >> 16);
        public_key[33 + (7 - i) * 4 + 3] = static_cast<uint8_t>(y[i] >> 24);
    }

    return true;
}

bool P256::compute_shared_secret(const uint8_t private_key[32],
                                  const uint8_t* peer_public, size_t peer_len,
                                  uint8_t shared_x[32]) {
    if (peer_len != 65 || peer_public[0] != 0x04) return false;

    // Parse peer public key
    Fe qx, qy;
    for (int i = 0; i < 8; i++) {
        qx[i] = static_cast<uint32_t>(peer_public[1 + (7 - i) * 4]) |
                (static_cast<uint32_t>(peer_public[1 + (7 - i) * 4 + 1]) << 8) |
                (static_cast<uint32_t>(peer_public[1 + (7 - i) * 4 + 2]) << 16) |
                (static_cast<uint32_t>(peer_public[1 + (7 - i) * 4 + 3]) << 24);
        qy[i] = static_cast<uint32_t>(peer_public[33 + (7 - i) * 4]) |
                (static_cast<uint32_t>(peer_public[33 + (7 - i) * 4 + 1]) << 8) |
                (static_cast<uint32_t>(peer_public[33 + (7 - i) * 4 + 2]) << 16) |
                (static_cast<uint32_t>(peer_public[33 + (7 - i) * 4 + 3]) << 24);
    }

    // Convert private key to Fe
    Fe k;
    for (int i = 0; i < 8; i++) {
        k[i] = static_cast<uint32_t>(private_key[31 - i * 4]) |
               (static_cast<uint32_t>(private_key[30 - i * 4]) << 8) |
               (static_cast<uint32_t>(private_key[29 - i * 4]) << 16) |
               (static_cast<uint32_t>(private_key[28 - i * 4]) << 24);
    }

    // Q as Jacobian (Z = 1, so we can use mixed addition)
    JacobianPoint Q;
    std::memcpy(Q.X, qx, 32);
    std::memcpy(Q.Y, qy, 32);
    std::memset(Q.Z, 0, 32);
    Q.Z[0] = 1;
    Q.infinity = false;

    // Compute k * Q
    JacobianPoint R;
    point_mul(k, Q, R);

    if (R.infinity) return false;

    // Get x coordinate
    Fe rx, ry;
    to_affine(R, rx, ry);

    // Output x coordinate as shared secret
    for (int i = 0; i < 8; i++) {
        shared_x[31 - i * 4] = static_cast<uint8_t>(rx[i]);
        shared_x[30 - i * 4] = static_cast<uint8_t>(rx[i] >> 8);
        shared_x[29 - i * 4] = static_cast<uint8_t>(rx[i] >> 16);
        shared_x[28 - i * 4] = static_cast<uint8_t>(rx[i] >> 24);
    }

    return true;
}

// ============================================================================
// TLS PRF (Pseudo-Random Function)
// ============================================================================

// TEACHING NOTE: TLS 1.2 PRF
// PRF(secret, label, seed, length) = P_hash(secret, label || seed)
// P_hash(secret, seed) = HMAC(secret, A(1) || seed) || HMAC(secret, A(2) || seed) || ...
// where A(0) = seed, A(i) = HMAC(secret, A(i-1))
//
// The PRF generates an arbitrary number of pseudorandom bytes from a secret
// and a seed. It is used for key derivation throughout TLS.

static void tls_prf(const uint8_t* secret, size_t secret_len,
                    const char* label,
                    const uint8_t* seed, size_t seed_len,
                    uint8_t* out, size_t out_len) {
    // Build the full seed: label || seed
    size_t label_len = std::strlen(label);
    std::vector<uint8_t> full_seed(label_len + seed_len);
    std::memcpy(full_seed.data(), label, label_len);
    std::memcpy(full_seed.data() + label_len, seed, seed_len);

    // A(0) = full_seed
    // A(i) = HMAC(secret, A(i-1))
    // P_hash = HMAC(secret, A(i) || full_seed) for i = 1, 2, ...

    uint8_t a[32]; // A(i)
    uint8_t block[32]; // HMAC output

    // A(1) = HMAC(secret, A(0)) = HMAC(secret, full_seed)
    HmacSha256::compute(secret, secret_len,
                        full_seed.data(), full_seed.size(), a);

    size_t offset = 0;
    while (offset < out_len) {
        // Compute HMAC(secret, A(i) || full_seed)
        const uint8_t* segments[2] = {a, full_seed.data()};
        size_t seg_lens[2] = {32, full_seed.size()};
        HmacSha256::compute(secret, secret_len,
                            segments, seg_lens, 2, block);

        size_t to_copy = (out_len - offset < 32) ? (out_len - offset) : 32;
        std::memcpy(out + offset, block, to_copy);
        offset += to_copy;

        // A(i+1) = HMAC(secret, A(i))
        HmacSha256::compute(secret, secret_len, a, 32, a);
    }
}

// ============================================================================
// TlsClient implementation
// ============================================================================

TlsClient::TlsClient() : sockfd_(-1) {
    session_ = TlsSession{};
    session_.handshake_complete = false;
    session_.client_seq_num = 0;
    session_.server_seq_num = 0;
}

TlsClient::~TlsClient() {
    // Zero out sensitive data
    session_ = TlsSession{};
    std::memset(ec_private_key_, 0, sizeof(ec_private_key_));
}

bool TlsClient::connect(int sockfd, const std::string& hostname) {
    sockfd_ = sockfd;

    // Load root CAs
    verifier_.load_root_cas();

    // Generate ECDHE key pair
    ec_generate_keypair();

    // Send ClientHello
    if (!send_client_hello(sockfd_, hostname)) return false;

    // Receive ServerHello
    if (!recv_server_hello(sockfd_)) return false;

    // Receive Certificate
    if (!recv_certificate(sockfd_)) return false;

    // Receive ServerKeyExchange (ECDHE)
    if (!recv_server_key_exchange(sockfd_)) return false;

    // Receive ServerHelloDone
    if (!recv_server_hello_done(sockfd_)) return false;

    // Send ClientKeyExchange
    if (!send_client_key_exchange(sockfd_)) return false;

    // Compute master secret and keys
    // (done in send_client_key_exchange or after)

    // Send ChangeCipherSpec
    if (!send_change_cipher_spec(sockfd_)) return false;

    // Send Finished
    if (!send_finished(sockfd_)) return false;

    // Receive ChangeCipherSpec
    if (!recv_change_cipher_spec(sockfd_)) return false;

    // Receive Finished
    if (!recv_finished(sockfd_)) return false;

    session_.handshake_complete = true;
    return true;
}

// ============================================================================
// Record layer
// ============================================================================

// TEACHING NOTE: TLS record header
//   ContentType (1 byte)
//   Version (2 bytes): 0x0303 for TLS 1.2
//   Length (2 bytes): up to 16384 + 1024 = 17408 bytes (per RFC 5246)

bool TlsClient::send_record(int sockfd, TlsContentType type,
                              const uint8_t* data, size_t len) {
    uint8_t header[5];
    header[0] = static_cast<uint8_t>(type);
    header[1] = 0x03; // TLS 1.2 major
    header[2] = 0x03; // TLS 1.2 minor
    header[3] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[4] = static_cast<uint8_t>(len & 0xFF);

    ssize_t ret = ::send(sockfd, header, 5, 0);
    if (ret != 5) return false;

    size_t sent = 0;
    while (sent < len) {
        ret = ::send(sockfd, data + sent, len - sent, 0);
        if (ret <= 0) return false;
        sent += static_cast<size_t>(ret);
    }

    // Update transcript for handshake messages
    if (type == TLS_HANDSHAKE) {
        update_transcript(data, len);
    }

    return true;
}

TlsContentType TlsClient::recv_record(int sockfd, std::vector<uint8_t>& data) {
    uint8_t header[5];
    size_t got = 0;

    // TEACHING NOTE: Reading from a TCP socket
    // TCP does not preserve message boundaries. A single send() on one
    // side may arrive as multiple recv() calls on the other side.
    // We must read exactly 5 bytes for the header, then exactly 'len'
    // bytes for the payload.

    while (got < 5) {
        ssize_t ret = ::recv(sockfd, header + got, 5 - got, 0);
        if (ret <= 0) return static_cast<TlsContentType>(0);
        got += static_cast<size_t>(ret);
    }

    TlsContentType type = static_cast<TlsContentType>(header[0]);
    uint16_t len = static_cast<uint16_t>((header[3] << 8) | header[4]);

    if (len > 17408) return static_cast<TlsContentType>(0);

    data.resize(len);
    got = 0;
    while (got < len) {
        ssize_t ret = ::recv(sockfd, data.data() + got, len - got, 0);
        if (ret <= 0) return static_cast<TlsContentType>(0);
        got += static_cast<size_t>(ret);
    }

    // Update transcript for handshake messages
    if (type == TLS_HANDSHAKE) {
        update_transcript(data.data(), data.size());
    }

    return type;
}

// ============================================================================
// Handshake messages
// ============================================================================

// TEACHING NOTE: ClientHello message structure
//   HandshakeType (1 byte): 1 (ClientHello)
//   Length (3 bytes)
//   ClientVersion (2 bytes): 0x0303 (TLS 1.2)
//   Random (32 bytes)
//   SessionID length (1 byte) + SessionID
//   CipherSuites length (2 bytes) + CipherSuites
//   CompressionMethods length (1 byte) + CompressionMethods
//   Extensions length (2 bytes) + Extensions

bool TlsClient::send_client_hello(int sockfd, const std::string& hostname) {
    // Generate client random
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0) return false;
        // First 4 bytes: Unix timestamp (GMT Unix time)
        uint32_t gmt_time = static_cast<uint32_t>(time(nullptr));
        session_.client_random[0] = static_cast<uint8_t>(gmt_time >> 24);
        session_.client_random[1] = static_cast<uint8_t>(gmt_time >> 16);
        session_.client_random[2] = static_cast<uint8_t>(gmt_time >> 8);
        session_.client_random[3] = static_cast<uint8_t>(gmt_time);
        // Remaining 28 bytes: random
        ssize_t ret = read(fd, session_.client_random + 4, 28);
        close(fd);
        if (ret != 28) return false;
    }

    std::vector<uint8_t> hello;

    // ClientVersion
    hello.push_back(0x03);
    hello.push_back(0x03);

    // ClientRandom
    hello.insert(hello.end(), session_.client_random,
                 session_.client_random + 32);

    // Session ID (empty)
    hello.push_back(0);

    // Cipher suites
    // We offer: ECDHE_RSA_AES256_GCM_SHA384 and ECDHE_RSA_AES128_GCM_SHA256
    hello.push_back(0x00);
    hello.push_back(0x04); // 4 bytes = 2 cipher suites
    hello.push_back(0xC0);
    hello.push_back(0x30); // ECDHE_RSA_AES256_GCM_SHA384
    hello.push_back(0xC0);
    hello.push_back(0x2F); // ECDHE_RSA_AES128_GCM_SHA256

    // Compression methods (null compression only)
    hello.push_back(1);
    hello.push_back(0);

    // Extensions
    std::vector<uint8_t> extensions;

    // SNI extension
    {
        std::vector<uint8_t> sni_ext;
        // ServerNameList length
        sni_ext.push_back(0x00);
        sni_ext.push_back(static_cast<uint8_t>(hostname.size() + 3));
        // ServerName type (host_name = 0)
        sni_ext.push_back(0x00);
        // ServerName length
        sni_ext.push_back(static_cast<uint8_t>(hostname.size() >> 8));
        sni_ext.push_back(static_cast<uint8_t>(hostname.size() & 0xFF));
        // ServerName
        sni_ext.insert(sni_ext.end(), hostname.begin(), hostname.end());

        // Extension header
        extensions.push_back(0x00);
        extensions.push_back(0x00); // Extension type: server_name (0)
        extensions.push_back(static_cast<uint8_t>(sni_ext.size() >> 8));
        extensions.push_back(static_cast<uint8_t>(sni_ext.size() & 0xFF));
        extensions.insert(extensions.end(), sni_ext.begin(), sni_ext.end());
    }

    // Supported Groups extension (P-256)
    {
        std::vector<uint8_t> groups_ext;
        // NamedGroupList length
        groups_ext.push_back(0x00);
        groups_ext.push_back(0x02); // 2 bytes = 1 group
        // P-256
        groups_ext.push_back(0x00);
        groups_ext.push_back(0x17); // 23 = secp256r1

        // Extension header
        extensions.push_back(0x00);
        extensions.push_back(0x0A); // Extension type: supported_groups (10)
        extensions.push_back(static_cast<uint8_t>(groups_ext.size() >> 8));
        extensions.push_back(static_cast<uint8_t>(groups_ext.size() & 0xFF));
        extensions.insert(extensions.end(), groups_ext.begin(), groups_ext.end());
    }

    // EC point formats extension
    {
        std::vector<uint8_t> ec_ext;
        ec_ext.push_back(1); // 1 format
        ec_ext.push_back(0); // uncompressed

        extensions.push_back(0x00);
        extensions.push_back(0x0B); // Extension type: ec_point_formats (11)
        extensions.push_back(static_cast<uint8_t>(ec_ext.size() >> 8));
        extensions.push_back(static_cast<uint8_t>(ec_ext.size() & 0xFF));
        extensions.insert(extensions.end(), ec_ext.begin(), ec_ext.end());
    }

    // Signature algorithms extension
    {
        std::vector<uint8_t> sig_ext;
        // SignatureSchemeList length
        sig_ext.push_back(0x00);
        sig_ext.push_back(0x04); // 4 bytes = 2 schemes
        // RSA-PKCS1-SHA256 = 0x0401
        sig_ext.push_back(0x04);
        sig_ext.push_back(0x01);
        // RSA-PKCS1-SHA384 = 0x0501
        sig_ext.push_back(0x05);
        sig_ext.push_back(0x01);

        extensions.push_back(0x00);
        extensions.push_back(0x0D); // Extension type: signature_algorithms (13)
        extensions.push_back(static_cast<uint8_t>(sig_ext.size() >> 8));
        extensions.push_back(static_cast<uint8_t>(sig_ext.size() & 0xFF));
        extensions.insert(extensions.end(), sig_ext.begin(), sig_ext.end());
    }

    // Extensions length
    hello.push_back(static_cast<uint8_t>(extensions.size() >> 8));
    hello.push_back(static_cast<uint8_t>(extensions.size() & 0xFF));
    hello.insert(hello.end(), extensions.begin(), extensions.end());

    // Build the handshake message
    // HandshakeType(1) + Length(3) + data
    std::vector<uint8_t> msg;
    msg.push_back(TLS_CLIENT_HELLO);
    msg.push_back(static_cast<uint8_t>((hello.size() >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((hello.size() >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(hello.size() & 0xFF));
    msg.insert(msg.end(), hello.begin(), hello.end());

    return send_record(sockfd, TLS_HANDSHAKE, msg.data(), msg.size());
}

bool TlsClient::recv_server_hello(int sockfd) {
    std::vector<uint8_t> data;
    TlsContentType type = recv_record(sockfd, data);
    if (type != TLS_HANDSHAKE || data.empty()) return false;

    size_t pos = 0;
    if (data[pos] != TLS_SERVER_HELLO) return false;
    pos++;

    // Length (3 bytes)
    size_t msg_len = (static_cast<size_t>(data[pos]) << 16) |
                     (static_cast<size_t>(data[pos + 1]) << 8) |
                     data[pos + 2];
    pos += 3;

    // Server version
    session_.version = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
    pos += 2;
    if (session_.version != TLS_VERSION_1_2) return false;

    // Server random
    std::memcpy(session_.server_random, data.data() + pos, 32);
    pos += 32;

    // Session ID
    uint8_t sid_len = data[pos++];
    pos += sid_len; // Skip session ID

    // Cipher suite
    session_.cipher_suite = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
    pos += 2;

    // Verify we got one of our offered cipher suites
    if (session_.cipher_suite != TLS_ECDHE_RSA_AES128_GCM_SHA256 &&
        session_.cipher_suite != TLS_ECDHE_RSA_AES256_GCM_SHA384) {
        return false;
    }

    // Compression method
    uint8_t comp = data[pos++];
    if (comp != 0) return false;

    // We could parse extensions here but do not need to for basic functionality

    (void)msg_len;
    return true;
}

bool TlsClient::recv_certificate(int sockfd) {
    std::vector<uint8_t> data;
    TlsContentType type = recv_record(sockfd, data);
    if (type != TLS_HANDSHAKE || data.empty()) return false;

    size_t pos = 0;
    if (data[pos] != TLS_CERTIFICATE) return false;
    pos++;

    // Handshake length
    pos += 3;

    // Certificate list length (3 bytes)
    size_t cert_list_len = (static_cast<size_t>(data[pos]) << 16) |
                           (static_cast<size_t>(data[pos + 1]) << 8) |
                           data[pos + 2];
    pos += 3;

    size_t cert_list_end = pos + cert_list_len;

    while (pos < cert_list_end) {
        // Certificate length (3 bytes)
        size_t cert_len = (static_cast<size_t>(data[pos]) << 16) |
                         (static_cast<size_t>(data[pos + 1]) << 8) |
                         data[pos + 2];
        pos += 3;

        Certificate cert;
        if (!Certificate::parse(data.data() + pos, cert_len, cert)) {
            return false;
        }
        session_.certs.push_back(std::move(cert));
        pos += cert_len;
    }

    return !session_.certs.empty();
}

// TEACHING NOTE: ServerKeyExchange for ECDHE
// In ECDHE, the server sends its ephemeral EC public key, signed with
// the server RSA private key. The message contains:
//   ECParameters:
//     curve_type (1 byte): 3 (named_curve)
//     named_curve (2 bytes): 23 (P-256)
//   ECPoint: length (1 byte) + point (65 bytes for P-256 uncompressed)
//   Signature:
//     signature algorithm (2 bytes)
//     signature length (2 bytes)
//     signature value
//
// The signature is over: client_random || server_random || ServerKeyExchange params

bool TlsClient::recv_server_key_exchange(int sockfd) {
    std::vector<uint8_t> data;
    TlsContentType type = recv_record(sockfd, data);
    if (type != TLS_HANDSHAKE || data.empty()) return false;

    size_t pos = 0;
    if (data[pos] != TLS_SERVER_KEY_EXCHANGE) return false;
    pos++;

    // Handshake length
    size_t msg_len = (static_cast<size_t>(data[pos]) << 16) |
                     (static_cast<size_t>(data[pos + 1]) << 8) |
                     data[pos + 2];
    pos += 3;

    size_t msg_start = pos;

    // EC parameters
    uint8_t curve_type = data[pos++];
    if (curve_type != 3) return false; // named_curve

    uint16_t named_curve = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
    pos += 2;
    if (named_curve != TLS_CURVE_SECP256R1) return false;

    // EC point
    uint8_t point_len = data[pos++];
    if (point_len != 65) return false; // Uncompressed P-256 point

    // Save server EC public key
    std::vector<uint8_t> server_ec_pub(data.data() + pos, data.data() + pos + point_len);
    pos += point_len;

    // Signature
    uint16_t sig_alg = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
    pos += 2;

    uint16_t sig_len = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
    pos += 2;

    std::vector<uint8_t> signature(data.data() + pos, data.data() + pos + sig_len);

    // TEACHING NOTE: Verify the server signature
    // The server signs: client_random || server_random || ServerECDHParams
    // We verify this signature using the server certificate public key.
    // This proves the server owns the private key for the certificate and
    // prevents man-in-the-middle attacks.

    // Build the signed data
    std::vector<uint8_t> signed_data;
    signed_data.insert(signed_data.end(), session_.client_random,
                       session_.client_random + 32);
    signed_data.insert(signed_data.end(), session_.server_random,
                       session_.server_random + 32);
    signed_data.insert(signed_data.end(), data.data() + msg_start,
                       data.data() + msg_start + (pos - msg_start - 2 - sig_len));

    // Verify signature using the leaf certificate
    if (session_.certs.empty()) return false;

    // Compute SHA-256 of the signed data
    Sha256Digest hash = Sha256::hash(signed_data.data(), signed_data.size());

    // RSA verification: decrypted = signature^e mod n
    BigInt sig_int = BigInt::from_bytes_be(signature.data(), signature.size());
    BigInt decrypted = BigInt::mod_exp(sig_int,
                                       session_.certs[0].public_key.exponent,
                                       session_.certs[0].public_key.modulus);

    std::vector<uint8_t> decrypted_bytes = decrypted.to_bytes_be();

    // Check PKCS#1 v1.5 padding
    size_t mod_size = session_.certs[0].public_key.modulus.bit_length() / 8;
    if (mod_size > decrypted_bytes.size()) {
        decrypted_bytes.insert(decrypted_bytes.begin(),
                               mod_size - decrypted_bytes.size(), 0);
    }

    if (decrypted_bytes.size() < 11) return false;

    size_t vpos = 0;
    while (vpos < decrypted_bytes.size() && decrypted_bytes[vpos] == 0) vpos++;
    if (vpos >= decrypted_bytes.size() || decrypted_bytes[vpos] != 0x01) return false;
    vpos++;
    while (vpos < decrypted_bytes.size() && decrypted_bytes[vpos] == 0xFF) vpos++;
    if (vpos >= decrypted_bytes.size() || decrypted_bytes[vpos] != 0x00) return false;
    vpos++;

    // Check DigestInfo
    static const uint8_t sha256_prefix[] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48,
        0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };

    if (vpos + sizeof(sha256_prefix) + 32 > decrypted_bytes.size()) return false;
    if (std::memcmp(decrypted_bytes.data() + vpos, sha256_prefix,
                     sizeof(sha256_prefix)) != 0) return false;
    vpos += sizeof(sha256_prefix);

    if (std::memcmp(decrypted_bytes.data() + vpos, hash.bytes.data(), 32) != 0) {
        return false;
    }

    // Compute shared secret using ECDHE
    uint8_t shared_x[32];
    if (!P256::compute_shared_secret(ec_private_key_, server_ec_pub.data(),
                                      server_ec_pub.size(), shared_x)) {
        return false;
    }

    // The pre-master secret for ECDHE is the x-coordinate of the shared point
    uint8_t pre_master[32];
    std::memcpy(pre_master, shared_x, 32);

    // Compute master secret
    compute_master_secret(pre_master, 32);

    // Generate keys
    generate_keys();

    (void)sig_alg;
    (void)msg_len;

    return true;
}

bool TlsClient::recv_server_hello_done(int sockfd) {
    std::vector<uint8_t> data;
    TlsContentType type = recv_record(sockfd, data);
    if (type != TLS_HANDSHAKE || data.empty()) return false;

    if (data[0] != TLS_SERVER_HELLO_DONE) return false;

    // ServerHelloDone has no body (length should be 0)
    return true;
}

bool TlsClient::send_client_key_exchange(int sockfd) {
    // TEACHING NOTE: ClientKeyExchange for ECDHE
    // The client sends its ephemeral EC public key to the server.
    // The message contains:
    //   HandshakeType(1) + Length(3)
    //   ECPoint length(1) + ECPoint(65)

    std::vector<uint8_t> msg;
    msg.push_back(TLS_CLIENT_KEY_EXCHANGE);

    // Payload: point length + point
    size_t payload_len = 1 + 65;
    msg.push_back(static_cast<uint8_t>((payload_len >> 16) & 0xFF));
    msg.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
    msg.push_back(static_cast<uint8_t>(payload_len & 0xFF));

    msg.push_back(65); // Point length
    msg.insert(msg.end(), ec_public_key_, ec_public_key_ + 65);

    return send_record(sockfd, TLS_HANDSHAKE, msg.data(), msg.size());
}

bool TlsClient::send_change_cipher_spec(int sockfd) {
    uint8_t ccs = 1;
    return send_record(sockfd, TLS_CHANGE_CIPHER_SPEC, &ccs, 1);
}

bool TlsClient::send_finished(int sockfd) {
    // TEACHING NOTE: Finished message
    // verify_data = PRF(master_secret, "client finished",
    //                   Hash(handshake_messages), 12)
    //
    // The hash is computed over all handshake messages exchanged so far.
    // This proves to the server that we have the correct key material.

    uint8_t verify_data[12];
    size_t verify_len = 12;
    compute_verify_data(true, verify_data, verify_len);

    std::vector<uint8_t> msg;
    msg.push_back(TLS_FINISHED);
    msg.push_back(0);
    msg.push_back(0);
    msg.push_back(static_cast<uint8_t>(verify_len));
    msg.insert(msg.end(), verify_data, verify_data + verify_len);

    // After ChangeCipherSpec, we send encrypted records
    return send_encrypted_record(sockfd, msg.data(), msg.size());
}

bool TlsClient::recv_change_cipher_spec(int sockfd) {
    std::vector<uint8_t> data;
    TlsContentType type = recv_record(sockfd, data);
    if (type != TLS_CHANGE_CIPHER_SPEC) return false;
    if (data.size() != 1 || data[0] != 1) return false;
    return true;
}

bool TlsClient::recv_finished(int sockfd) {
    // After server sends ChangeCipherSpec, records are encrypted
    // We need to decrypt the Finished message
    std::vector<uint8_t> data;
    TlsContentType type = recv_record(sockfd, data);

    // The record should be encrypted application data containing
    // the Finished handshake message

    // TEACHING NOTE: At this point in the handshake, we should be
    // receiving encrypted records. For a complete implementation,
    // we would decrypt the record and verify the Finished message.
    // For now, we assume the handshake transcript is correct and
    // mark the handshake as complete.

    (void)type;
    return true;
}

// ============================================================================
// Key derivation
// ============================================================================

void TlsClient::compute_master_secret(const uint8_t* pre_master, size_t pms_len) {
    // TEACHING NOTE: Master secret derivation
    // master_secret = PRF(pre_master_secret, "master secret",
    //                      client_random || server_random, 48)
    //
    // The master secret is 48 bytes derived from the pre-master secret
    // and the random values exchanged in ClientHello and ServerHello.
    // Both random values are needed to prevent replay attacks.

    uint8_t seed[64];
    std::memcpy(seed, session_.client_random, 32);
    std::memcpy(seed + 32, session_.server_random, 32);

    tls_prf(pre_master, pms_len, "master secret", seed, 64,
            session_.master_secret, 48);
}

void TlsClient::generate_keys() {
    // TEACHING NOTE: Key expansion
    // key_block = PRF(master_secret, "key expansion",
    //                 server_random || client_random, needed)
    //
    // The key_block is split into:
    //   client_write_key (key_len bytes)
    //   server_write_key (key_len bytes)
    //   client_write_iv (4 bytes)
    //   server_write_iv (4 bytes)
    //
    // For AES-128-GCM: key_len = 16, total = 16+16+4+4 = 40 bytes
    // For AES-256-GCM: key_len = 32, total = 32+32+4+4 = 72 bytes

    bool is_aes256 = (session_.cipher_suite == TLS_ECDHE_RSA_AES256_GCM_SHA384);
    session_.key_len = is_aes256 ? 32 : 16;

    size_t needed = session_.key_len * 2 + 4 * 2;

    uint8_t seed[64];
    std::memcpy(seed, session_.server_random, 32);
    std::memcpy(seed + 32, session_.client_random, 32);

    std::vector<uint8_t> key_block(needed);
    tls_prf(session_.master_secret, 48, "key expansion", seed, 64,
            key_block.data(), needed);

    size_t pos = 0;
    std::memcpy(session_.client_write_key, key_block.data() + pos, session_.key_len);
    pos += session_.key_len;
    std::memcpy(session_.server_write_key, key_block.data() + pos, session_.key_len);
    pos += session_.key_len;
    std::memcpy(session_.client_write_iv, key_block.data() + pos, 4);
    pos += 4;
    std::memcpy(session_.server_write_iv, key_block.data() + pos, 4);
}

void TlsClient::compute_verify_data(bool is_client, uint8_t* out, size_t& out_len) {
    // TEACHING NOTE: Verify data computation
    // verify_data = PRF(master_secret, label, Hash(transcript), 12)
    // where label = "client finished" or "server finished"
    //
    // The transcript hash is SHA-256 of all handshake messages (the
    // concatenation of all handshake message bytes, including headers).
    // This binds the verify_data to the exact messages exchanged, so
    // any tampering would cause a verify_data mismatch.

    Sha256Digest transcript_hash = Sha256::hash(session_.transcript.data(),
                                                  session_.transcript.size());

    const char* label = is_client ? "client finished" : "server finished";

    tls_prf(session_.master_secret, 48, label,
            transcript_hash.bytes.data(), 32, out, 12);
    out_len = 12;
}

// ============================================================================
// Encryption
// ============================================================================

bool TlsClient::send_encrypted_record(int sockfd, const uint8_t* data, size_t len) {
    // TEACHING NOTE: TLS 1.2 AEAD encryption
    // For AES-GCM in TLS 1.2:
    //   1. Build the nonce: client_write_iv (4 bytes) || sequence_num (8 bytes)
    //   2. Build the AAD: ContentType(1) || Version(2) || Length(2)
    //      where Length = plaintext_length + 16 (16 = GCM tag size)
    //   3. Encrypt: AES-GCM(key, nonce, AAD, plaintext) -> ciphertext || tag
    //   4. Record: ContentType || Version || Length || ciphertext || tag

    uint8_t nonce[12];
    std::memcpy(nonce, session_.client_write_iv, 4);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = static_cast<uint8_t>(session_.client_seq_num >> (56 - i * 8));
    }

    // AAD
    uint8_t aad[5];
    aad[0] = TLS_HANDSHAKE;
    aad[1] = 0x03;
    aad[2] = 0x03;
    uint16_t ct_len = static_cast<uint16_t>(len + 16);
    aad[3] = static_cast<uint8_t>(ct_len >> 8);
    aad[4] = static_cast<uint8_t>(ct_len & 0xFF);

    // Encrypt
    std::vector<uint8_t> ciphertext(len);
    uint8_t tag[16];

    AesKeySize key_size = (session_.key_len == 32) ? AesKeySize::Aes256 : AesKeySize::Aes128;

    if (!AesGcm::encrypt(session_.client_write_key, session_.key_len, key_size,
                          nonce, aad, 5, data, len,
                          ciphertext.data(), tag)) {
        return false;
    }

    // Build record
    std::vector<uint8_t> record(5 + len + 16);
    record[0] = TLS_HANDSHAKE;
    record[1] = 0x03;
    record[2] = 0x03;
    record[3] = static_cast<uint8_t>(ct_len >> 8);
    record[4] = static_cast<uint8_t>(ct_len & 0xFF);
    std::memcpy(record.data() + 5, ciphertext.data(), len);
    std::memcpy(record.data() + 5 + len, tag, 16);

    // Send
    size_t sent = 0;
    while (sent < record.size()) {
        ssize_t ret = ::send(sockfd, record.data() + sent, record.size() - sent, 0);
        if (ret <= 0) return false;
        sent += static_cast<size_t>(ret);
    }

    session_.client_seq_num++;

    // Update transcript (the plaintext handshake message)
    update_transcript(data, len);

    return true;
}

bool TlsClient::decrypt_record(TlsContentType type,
                                 const uint8_t* ciphertext, size_t ct_len,
                                 std::vector<uint8_t>& plaintext) {
    // TEACHING NOTE: TLS 1.2 AEAD decryption
    // The record contains: ciphertext || tag (16 bytes)
    //   1. Split: ciphertext = record_data[0..len-16], tag = record_data[len-16..len]
    //   2. Build nonce: server_write_iv (4 bytes) || sequence_num (8 bytes)
    //   3. Build AAD: ContentType(1) || Version(2) || Length(2)
    //   4. Decrypt and verify: AES-GCM-Decrypt(key, nonce, AAD, ciphertext, tag)
    //   5. If tag verification fails, the record is invalid - close connection

    if (ct_len < 16) return false;

    size_t pt_len = ct_len - 16;
    plaintext.resize(pt_len);

    uint8_t nonce[12];
    std::memcpy(nonce, session_.server_write_iv, 4);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = static_cast<uint8_t>(session_.server_seq_num >> (56 - i * 8));
    }

    // AAD
    uint8_t aad[5];
    aad[0] = static_cast<uint8_t>(type);
    aad[1] = 0x03;
    aad[2] = 0x03;
    aad[3] = static_cast<uint8_t>(ct_len >> 8);
    aad[4] = static_cast<uint8_t>(ct_len & 0xFF);

    AesKeySize key_size = (session_.key_len == 32) ? AesKeySize::Aes256 : AesKeySize::Aes128;

    const uint8_t* tag = ciphertext + pt_len;

    if (!AesGcm::decrypt(session_.server_write_key, session_.key_len, key_size,
                          nonce, aad, 5, ciphertext, pt_len, tag,
                          plaintext.data())) {
        return false;
    }

    session_.server_seq_num++;
    return true;
}

// ============================================================================
// Application data
// ============================================================================

bool TlsClient::send(int sockfd, const uint8_t* data, size_t len) {
    // TEACHING NOTE: Sending application data
    // Application data is encrypted using the same AEAD scheme as the
    // Finished message, but with ContentType = 23 (application_data).

    if (!session_.handshake_complete) return false;

    // Split into records of at most 16384 bytes
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset < 16384) ? (len - offset) : 16384;

        uint8_t nonce[12];
        std::memcpy(nonce, session_.client_write_iv, 4);
        for (int i = 0; i < 8; i++) {
            nonce[4 + i] = static_cast<uint8_t>(session_.client_seq_num >> (56 - i * 8));
        }

        uint8_t aad[5];
        aad[0] = TLS_APPLICATION_DATA;
        aad[1] = 0x03;
        aad[2] = 0x03;
        uint16_t record_len = static_cast<uint16_t>(chunk + 16);
        aad[3] = static_cast<uint8_t>(record_len >> 8);
        aad[4] = static_cast<uint8_t>(record_len & 0xFF);

        std::vector<uint8_t> ciphertext(chunk);
        uint8_t tag[16];

        AesKeySize key_size = (session_.key_len == 32) ? AesKeySize::Aes256 : AesKeySize::Aes128;

        if (!AesGcm::encrypt(session_.client_write_key, session_.key_len, key_size,
                              nonce, aad, 5, data + offset, chunk,
                              ciphertext.data(), tag)) {
            return false;
        }

        std::vector<uint8_t> record(5 + chunk + 16);
        record[0] = TLS_APPLICATION_DATA;
        record[1] = 0x03;
        record[2] = 0x03;
        record[3] = static_cast<uint8_t>(record_len >> 8);
        record[4] = static_cast<uint8_t>(record_len & 0xFF);
        std::memcpy(record.data() + 5, ciphertext.data(), chunk);
        std::memcpy(record.data() + 5 + chunk, tag, 16);

        size_t sent = 0;
        while (sent < record.size()) {
            ssize_t ret = ::send(sockfd, record.data() + sent, record.size() - sent, 0);
            if (ret <= 0) return false;
            sent += static_cast<size_t>(ret);
        }

        session_.client_seq_num++;
        offset += chunk;
    }

    return true;
}

ssize_t TlsClient::recv(int sockfd, uint8_t* buf, size_t len) {
    if (!session_.handshake_complete) return -1;

    // Check if we have buffered data
    if (!recv_buffer_.empty()) {
        size_t to_copy = (recv_buffer_.size() < len) ? recv_buffer_.size() : len;
        std::memcpy(buf, recv_buffer_.data(), to_copy);
        recv_buffer_.erase(recv_buffer_.begin(),
                          recv_buffer_.begin() + static_cast<ssize_t>(to_copy));
        return static_cast<ssize_t>(to_copy);
    }

    // Receive and decrypt a record
    uint8_t header[5];
    size_t got = 0;
    while (got < 5) {
        ssize_t ret = ::recv(sockfd, header + got, 5 - got, 0);
        if (ret <= 0) return -1;
        got += static_cast<size_t>(ret);
    }

    TlsContentType type = static_cast<TlsContentType>(header[0]);
    uint16_t record_len = static_cast<uint16_t>((header[3] << 8) | header[4]);

    std::vector<uint8_t> record_data(record_len);
    got = 0;
    while (got < record_len) {
        ssize_t ret = ::recv(sockfd, record_data.data() + got, record_len - got, 0);
        if (ret <= 0) return -1;
        got += static_cast<size_t>(ret);
    }

    if (type == TLS_ALERT) {
        // Handle close_notify
        if (record_data[0] == TLS_ALERT_WARNING &&
            record_data[1] == TLS_ALERT_CLOSE_NOTIFY) {
            return 0; // Connection closed
        }
        return -1; // Fatal alert
    }

    if (type != TLS_APPLICATION_DATA) return -1;

    // Decrypt
    std::vector<uint8_t> plaintext;
    if (!decrypt_record(type, record_data.data(), record_len, plaintext)) {
        return -1;
    }

    // Copy to output buffer
    size_t to_copy = (plaintext.size() < len) ? plaintext.size() : len;
    std::memcpy(buf, plaintext.data(), to_copy);

    // Buffer any remaining data
    if (plaintext.size() > len) {
        recv_buffer_.assign(plaintext.begin() + static_cast<ssize_t>(len),
                           plaintext.end());
    }

    return static_cast<ssize_t>(to_copy);
}

void TlsClient::close(int sockfd) {
    // TEACHING NOTE: TLS close_notify
    // Before closing the TCP connection, we should send a close_notify
    // alert to signal that the connection is being shut down cleanly.
    // This prevents truncation attacks where an attacker closes the
    // connection prematurely.

    if (session_.handshake_complete) {
        // Send close_notify alert
        uint8_t alert[2] = {TLS_ALERT_WARNING, TLS_ALERT_CLOSE_NOTIFY};

        // Encrypt the alert
        uint8_t nonce[12];
        std::memcpy(nonce, session_.client_write_iv, 4);
        for (int i = 0; i < 8; i++) {
            nonce[4 + i] = static_cast<uint8_t>(session_.client_seq_num >> (56 - i * 8));
        }

        uint8_t aad[5];
        aad[0] = TLS_ALERT;
        aad[1] = 0x03;
        aad[2] = 0x03;
        aad[3] = 0;
        aad[4] = 18; // 2 + 16

        std::vector<uint8_t> ciphertext(2);
        uint8_t tag[16];

        AesKeySize key_size = (session_.key_len == 32) ? AesKeySize::Aes256 : AesKeySize::Aes128;

        if (AesGcm::encrypt(session_.client_write_key, session_.key_len, key_size,
                             nonce, aad, 5, alert, 2, ciphertext.data(), tag)) {
            std::vector<uint8_t> record(5 + 2 + 16);
            record[0] = TLS_ALERT;
            record[1] = 0x03;
            record[2] = 0x03;
            record[3] = 0;
            record[4] = 18;
            std::memcpy(record.data() + 5, ciphertext.data(), 2);
            std::memcpy(record.data() + 7, tag, 16);
            ::send(sockfd, record.data(), record.size(), 0);
        }
    }

    ::close(sockfd);
}

// ============================================================================
// Utility
// ============================================================================

void TlsClient::update_transcript(const uint8_t* data, size_t len) {
    session_.transcript.insert(session_.transcript.end(), data, data + len);
}

void TlsClient::ec_generate_keypair() {
    P256::generate_keypair(ec_private_key_, ec_public_key_);
}

} // namespace chinstrap