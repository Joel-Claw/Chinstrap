// bigint.hpp - Arbitrary precision integer arithmetic for RSA cryptography
// Part of Chinstrap, a from-scratch web browser in C++ with zero third-party libraries.
//
// TEACHING NOTE: Why do we need big integers?
// RSA cryptography works with very large numbers - typically 2048 or 4096 bits.
// C++ built-in types max out at 64 bits (uint64_t). We need our own arbitrary
// precision integer type to perform the mathematical operations that RSA requires:
//   - Modular exponentiation: a^b mod m (the core RSA operation)
//   - Multiplication of large numbers
//   - Comparison and subtraction for verification
//
// TEACHING NOTE: How RSA works (simplified)
// RSA is a public-key cryptosystem. The key generation process:
//   1. Choose two large primes p and q
//   2. Compute n = p * q (the modulus)
//   3. Compute phi(n) = (p-1)(q-1) (Euler totient)
//   4. Choose e = 65537 (public exponent, a common choice)
//   5. Compute d = e^(-1) mod phi(n) (private exponent)
//
// Public key: (n, e), Private key: (n, d)
//
// Signing: signature = message^d mod n
// Verification: decrypted = signature^e mod n (should equal message)
//
// For TLS, the server signs its key exchange parameters with its private key.
// The client verifies the signature using the server certificate public key.
// We need big integer arithmetic to perform signature^e mod n.
//
// TEACHING NOTE: Representation
// We store big integers as arrays of 32-bit words (little-endian order: least
// significant word first). This is the conventional representation and allows
// us to use 64-bit intermediate results for multiplication without overflow.
// For example, a 2048-bit RSA modulus uses 64 words.
//
// TEACHING NOTE: Why not use a library?
// Libraries like GMP (GNU Multiple Precision Arithmetic Library) are highly
// optimized but we are building from scratch with zero third-party dependencies.
// Our implementation is correct but not as fast as GMP. For TLS, the main
// bottleneck is the RSA verification (signature^e mod n with e=65537), which
// is fast even with a basic implementation.

#ifndef CHINSTRAP_BIGINT_HPP
#define CHINSTRAP_BIGINT_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace chinstrap {

class BigInt {
public:
    // TEACHING NOTE: We store the number as a vector of 32-bit words.
    // words[0] is the least significant word (little-endian word order).
    // Each word stores 32 bits. So a 256-bit number uses 8 words.
    // The number is always normalized: the highest word is non-zero
    // (or the number is zero, represented as an empty vector).
    std::vector<uint32_t> words;
    bool negative; // true if the number is negative

    // Constructors
    BigInt();
    BigInt(uint32_t value);
    BigInt(uint64_t value);

    // Construct from a big-endian byte array (common in crypto - DER encoding)
    static BigInt from_bytes_be(const uint8_t* data, size_t len);

    // Convert to big-endian byte array
    std::vector<uint8_t> to_bytes_be() const;

    // Get the number of bits
    size_t bit_length() const;

    // Get a specific bit (0 = LSB)
    int get_bit(size_t index) const;

    // Comparison
    static int compare(const BigInt& a, const BigInt& b);
    bool is_zero() const;

    // Basic arithmetic
    static BigInt add(const BigInt& a, const BigInt& b);
    static BigInt subtract(const BigInt& a, const BigInt& b);
    static BigInt multiply(const BigInt& a, const BigInt& b);

    // Modular arithmetic (for RSA)
    static BigInt mod(const BigInt& a, const BigInt& m);
    static BigInt mod_exp(const BigInt& base, const BigInt& exp, const BigInt& mod);
    static BigInt mod_mul(const BigInt& a, const BigInt& b, const BigInt& m);

    // Right shift by bits
    static BigInt shift_right(const BigInt& a, size_t bits);

    // Helpers
    std::string to_hex() const;
    static BigInt from_hex(const char* hex);

    // Operators
    bool operator==(const BigInt& other) const;
    bool operator!=(const BigInt& other) const;
    bool operator<(const BigInt& other) const;
    bool operator>(const BigInt& other) const;
    bool operator<=(const BigInt& other) const;
    bool operator>=(const BigInt& other) const;

private:
    void normalize(); // Remove leading zero words
};

} // namespace chinstrap

#endif // CHINSTRAP_BIGINT_HPP