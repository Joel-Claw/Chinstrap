// bigint.cpp - Arbitrary precision integer arithmetic for RSA
// Part of Chinstrap, a from-scratch web browser in C++ with zero third-party libraries.
//
// TEACHING NOTE: This implementation provides the arithmetic operations needed
// for RSA signature verification. The most important operation is modular
// exponentiation (base^exp mod m), which is the core of RSA.
//
// TEACHING NOTE: Schoolbook multiplication
// We use the basic O(n^2) schoolbook multiplication algorithm. For the key
// sizes in TLS (2048-4096 bits), this is adequate. Production crypto libraries
// use Karatsuba multiplication (O(n^1.585)) or FFT-based multiplication for
// large numbers, but schoolbook is fine for our purposes.
//
// TEACHING NOTE: Montgomery reduction
// Production RSA implementations use Montgomery multiplication for modular
// arithmetic because it avoids expensive division operations. We use the
// simpler "shift and subtract" modular reduction for clarity. The performance
// difference matters for key generation (which we do not do) but not for
// signature verification (which is fast even with schoolbook methods).

#include "bigint.hpp"
#include <algorithm>
#include <cstring>

namespace chinstrap {

// ============================================================================
// Constructors
// ============================================================================

BigInt::BigInt() : negative(false) {}

BigInt::BigInt(uint32_t value) : negative(false) {
    if (value != 0) {
        words.push_back(value);
    }
}

BigInt::BigInt(uint64_t value) : negative(false) {
    if (value != 0) {
        words.push_back(static_cast<uint32_t>(value & 0xFFFFFFFF));
        if (value > 0xFFFFFFFF) {
            words.push_back(static_cast<uint32_t>(value >> 32));
        }
    }
}

// TEACHING NOTE: from_bytes_be - create BigInt from big-endian bytes
// This is the standard representation in cryptography. DER-encoded integers
// and RSA key components are always big-endian. We convert to our internal
// little-endian word representation.
BigInt BigInt::from_bytes_be(const uint8_t* data, size_t len) {
    BigInt result;
    result.negative = false;

    if (len == 0) return result;

    // Skip leading zeros
    while (len > 0 && data[0] == 0) {
        data++;
        len--;
    }

    if (len == 0) return result;

    // We store in little-endian word order, so process bytes from
    // the end (least significant) to the beginning (most significant).
    size_t num_words = (len + 3) / 4;
    result.words.resize(num_words, 0);

    for (size_t i = 0; i < len; i++) {
        // Byte at position (len - 1 - i) from the end is the i-th byte
        // (0 = least significant)
        size_t byte_pos = len - 1 - i;
        size_t word_idx = i / 4;
        size_t byte_in_word = i % 4;
        result.words[word_idx] |= static_cast<uint32_t>(data[byte_pos]) << (byte_in_word * 8);
    }

    result.normalize();
    return result;
}

std::vector<uint8_t> BigInt::to_bytes_be() const {
    if (is_zero()) {
        return std::vector<uint8_t>(1, 0);
    }

    size_t num_bytes = (bit_length() + 7) / 8;
    std::vector<uint8_t> result(num_bytes, 0);

    for (size_t i = 0; i < words.size(); i++) {
        for (int j = 0; j < 4; j++) {
            uint8_t byte = static_cast<uint8_t>(words[i] >> (j * 8));
            // Byte at word i, position j goes to position (num_bytes - 1 - (i*4 + j))
            size_t pos = num_bytes - 1 - (i * 4 + j);
            if (pos < num_bytes) {
                result[pos] = byte;
            }
        }
    }

    return result;
}

void BigInt::normalize() {
    while (!words.empty() && words.back() == 0) {
        words.pop_back();
    }
}

// ============================================================================
// Properties
// ============================================================================

bool BigInt::is_zero() const {
    return words.empty();
}

size_t BigInt::bit_length() const {
    if (words.empty()) return 0;

    // Find the highest set bit
    uint32_t top_word = words.back();
    size_t bits = (words.size() - 1) * 32;

    // Count bits in the top word
    size_t top_bits = 0;
    for (int i = 31; i >= 0; i--) {
        if ((top_word >> i) & 1) {
            top_bits = static_cast<size_t>(i) + 1;
            break;
        }
    }

    return bits + top_bits;
}

int BigInt::get_bit(size_t index) const {
    size_t word_idx = index / 32;
    size_t bit_idx = index % 32;

    if (word_idx >= words.size()) return 0;

    return (words[word_idx] >> bit_idx) & 1;
}

// ============================================================================
// Comparison
// ============================================================================

int BigInt::compare(const BigInt& a, const BigInt& b) {
    // Compare magnitudes (ignore sign for now)
    if (a.words.size() != b.words.size()) {
        return a.words.size() > b.words.size() ? 1 : -1;
    }

    // Same number of words - compare from most significant
    for (int i = static_cast<int>(a.words.size()) - 1; i >= 0; i--) {
        if (a.words[static_cast<size_t>(i)] != b.words[static_cast<size_t>(i)]) {
            return a.words[static_cast<size_t>(i)] > b.words[static_cast<size_t>(i)] ? 1 : -1;
        }
    }

    return 0; // Equal magnitude
}

bool BigInt::operator==(const BigInt& other) const {
    return negative == other.negative && compare(*this, other) == 0;
}

bool BigInt::operator!=(const BigInt& other) const {
    return !(*this == other);
}

bool BigInt::operator<(const BigInt& other) const {
    if (negative != other.negative) {
        return negative;
    }
    if (negative) {
        return compare(*this, other) > 0;
    }
    return compare(*this, other) < 0;
}

bool BigInt::operator>(const BigInt& other) const {
    return other < *this;
}

bool BigInt::operator<=(const BigInt& other) const {
    return !(*this > other);
}

bool BigInt::operator>=(const BigInt& other) const {
    return !(*this < other);
}

// ============================================================================
// Addition and Subtraction
// ============================================================================

// TEACHING NOTE: Addition with carry
// We add word by word from least significant to most significant.
// The carry propagates through the words. If there is a final carry,
// we add a new word. This is exactly how you do addition on paper, but
// with 32-bit "digits" instead of decimal digits.

BigInt BigInt::add(const BigInt& a, const BigInt& b) {
    // For now, assume both are non-negative (sufficient for RSA)
    BigInt result;
    result.negative = false;

    size_t max_words = std::max(a.words.size(), b.words.size());
    result.words.resize(max_words, 0);

    uint64_t carry = 0;
    for (size_t i = 0; i < max_words; i++) {
        uint64_t sum = carry;
        if (i < a.words.size()) sum += a.words[i];
        if (i < b.words.size()) sum += b.words[i];

        result.words[i] = static_cast<uint32_t>(sum & 0xFFFFFFFF);
        carry = sum >> 32;
    }

    if (carry) {
        result.words.push_back(static_cast<uint32_t>(carry));
    }

    result.normalize();
    return result;
}

// TEACHING NOTE: Subtraction with borrow
// We subtract word by word. If the subtrahend is larger than the minuend
// at any position, we borrow from the next higher word. We assume a >= b
// (which holds for the modular reduction use case).

BigInt BigInt::subtract(const BigInt& a, const BigInt& b) {
    // Assume a >= b (caller must ensure)
    BigInt result;
    result.negative = false;

    result.words.resize(a.words.size(), 0);

    int64_t borrow = 0;
    for (size_t i = 0; i < a.words.size(); i++) {
        int64_t diff = static_cast<int64_t>(a.words[i]) - borrow;
        if (i < b.words.size()) {
            diff -= static_cast<int64_t>(b.words[i]);
        }

        if (diff < 0) {
            diff += static_cast<int64_t>(1) << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }

        result.words[i] = static_cast<uint32_t>(diff);
    }

    result.normalize();
    return result;
}

// ============================================================================
// Multiplication
// ============================================================================

// TEACHING NOTE: Schoolbook multiplication
// This is the same algorithm you learned in elementary school, but with
// 32-bit "digits" instead of decimal digits. For each word of b, we multiply
// it by each word of a and accumulate the result in the right position.
//
// The key insight: when we multiply two 32-bit words, the result is a 64-bit
// number. We split this into a low 32-bit part (which goes into the result)
// and a high 32-bit part (which is the carry to the next word).
//
// Time complexity: O(n * m) where n and m are the number of words.
// For RSA-2048, n = m = 64, so we have 4096 multiplications per multiply.
// With ~17 squarings and multiplications per bit of exponent, and 2048 bits,
// that is about 2048 * 17 * 4096 = ~143 million word multiplications.
// At ~1 GHz that is well under a second.

BigInt BigInt::multiply(const BigInt& a, const BigInt& b) {
    if (a.is_zero() || b.is_zero()) return BigInt();

    BigInt result;
    result.negative = a.negative != b.negative;
    result.words.resize(a.words.size() + b.words.size(), 0);

    for (size_t i = 0; i < a.words.size(); i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.words.size(); j++) {
            uint64_t product = static_cast<uint64_t>(a.words[i]) * b.words[j]
                             + result.words[i + j]
                             + carry;
            result.words[i + j] = static_cast<uint32_t>(product & 0xFFFFFFFF);
            carry = product >> 32;
        }
        if (carry) {
            result.words[i + b.words.size()] += static_cast<uint32_t>(carry);
        }
    }

    result.normalize();
    return result;
}

// ============================================================================
// Shift
// ============================================================================

BigInt BigInt::shift_right(const BigInt& a, size_t bits) {
    if (a.is_zero() || bits == 0) return a;

    BigInt result;
    result.negative = a.negative;

    size_t word_shift = bits / 32;
    size_t bit_shift = bits % 32;

    if (word_shift >= a.words.size()) return result;

    size_t new_size = a.words.size() - word_shift;
    result.words.resize(new_size);

    if (bit_shift == 0) {
        for (size_t i = 0; i < new_size; i++) {
            result.words[i] = a.words[i + word_shift];
        }
    } else {
        for (size_t i = 0; i < new_size; i++) {
            uint32_t low = a.words[i + word_shift] >> bit_shift;
            if (i + word_shift + 1 < a.words.size()) {
                uint32_t high = a.words[i + word_shift + 1] << (32 - bit_shift);
                result.words[i] = low | high;
            } else {
                result.words[i] = low;
            }
        }
    }

    result.normalize();
    return result;
}

// ============================================================================
// Modular arithmetic
// ============================================================================

// TEACHING NOTE: Modular reduction (a mod m)
// We use the simple "repeated subtraction" approach: while a >= m, subtract m.
// For efficiency, we shift m left to align with a and subtract the shifted value.
// This is called "long division" and is O(n^2) where n is the number of words.
//
// For RSA, the modulus is typically 2048 bits, so we need at most 2048 shifts
// and subtractions. Each is O(64) word operations, giving O(131072) operations
// per modular reduction. This is fast enough.

BigInt BigInt::mod(const BigInt& a, const BigInt& m) {
    if (a.is_zero()) return BigInt();

    // If a < m, result is a
    if (compare(a, m) < 0) {
        return a;
    }

    // TEACHING NOTE: Long division approach
    // We shift m left until it is just smaller than a, then subtract.
    // Then shift m right by 1 and repeat. This is the standard division algorithm.
    BigInt remainder = a;

    // Find the shift amount to align m with remainder
    size_t shift = a.bit_length() - m.bit_length();

    // Shift m left by 'shift' bits
    BigInt shifted_m = m;
    // We need to shift m left. Build it word by word.
    {
        size_t word_shift = shift / 32;
        size_t bit_shift = shift % 32;

        std::vector<uint32_t> new_words(word_shift + m.words.size() + 1, 0);

        if (bit_shift == 0) {
            for (size_t i = 0; i < m.words.size(); i++) {
                new_words[i + word_shift] = m.words[i];
            }
        } else {
            for (size_t i = 0; i < m.words.size(); i++) {
                new_words[i + word_shift] |= m.words[i] << bit_shift;
                if (bit_shift > 0) {
                    new_words[i + word_shift + 1] |= m.words[i] >> (32 - bit_shift);
                }
            }
        }

        shifted_m.words = new_words;
        shifted_m.normalize();
    }

    // Subtract shifted m repeatedly
    while (true) {
        while (compare(remainder, shifted_m) >= 0) {
            remainder = subtract(remainder, shifted_m);
        }

        if (shift == 0) break;

        // Shift m right by 1
        shifted_m = shift_right(shifted_m, 1);
        shift--;
    }

    return remainder;
}

// TEACHING NOTE: Modular multiplication (a * b mod m)
// We compute a * b first, then take mod m. This is simple but requires
// 2x the memory of the modulus. For RSA-2048, the product of two 2048-bit
// numbers is 4096 bits, which is fine.
//
// A more efficient approach (Barrett or Montgomery reduction) computes
// the result without ever storing the full product, but our approach is
// simpler and correct.

BigInt BigInt::mod_mul(const BigInt& a, const BigInt& b, const BigInt& m) {
    if (m.is_zero()) return BigInt();

    // First reduce a and b mod m (in case they are larger than m)
    BigInt a_mod = mod(a, m);
    BigInt b_mod = mod(b, m);

    BigInt product = multiply(a_mod, b_mod);
    return mod(product, m);
}

// TEACHING NOTE: Modular exponentiation (base^exp mod m)
// This is the core RSA operation. We use the "square and multiply" algorithm:
//
//   result = 1
//   For each bit of exp from MSB to LSB:
//     result = result^2 mod m  (square)
//     if bit == 1:
//       result = result * base mod m  (multiply)
//
// This requires O(log(exp)) squarings and up to O(log(exp)) multiplications.
// For RSA with e = 65537 = 2^16 + 1, this is only 17 squarings + 1 multiply.
// For RSA with d (private exponent, ~2048 bits), this is ~2048 squarings
// and ~1024 multiplications.
//
// TEACHING NOTE: Side-channel resistance
// In a production implementation, we would use constant-time algorithms to
// prevent timing side channels. The simple "square and multiply" leaks
// information about the exponent through timing (multiply step is conditional).
// For signature verification (public exponent), this is not a concern because
// the exponent is public. For signing (private exponent), we would need
// constant-time algorithms. Since we only do verification, this is fine.

BigInt BigInt::mod_exp(const BigInt& base, const BigInt& exp, const BigInt& m) {
    if (m.is_zero()) return BigInt();
    if (exp.is_zero()) return BigInt(static_cast<uint32_t>(1));

    BigInt result(static_cast<uint32_t>(1));
    BigInt b = mod(base, m);

    // Process exponent bits from MSB to LSB
    size_t exp_bits = exp.bit_length();

    for (size_t i = exp_bits; i > 0; i--) {
        size_t bit_idx = i - 1;

        // Square
        result = mod_mul(result, result, m);

        // Multiply if bit is set
        if (exp.get_bit(bit_idx) == 1) {
            result = mod_mul(result, b, m);
        }
    }

    return result;
}

// ============================================================================
// Hex conversion (for testing)
// ============================================================================

std::string BigInt::to_hex() const {
    if (is_zero()) return "0";

    std::string result;
    if (negative) result += "-";

    // Most significant word first
    for (int i = static_cast<int>(words.size()) - 1; i >= 0; i--) {
        char buf[9];
        snprintf(buf, sizeof(buf), "%08x", words[static_cast<size_t>(i)]);
        result += buf;
    }

    // Remove leading zeros
    size_t first_non_zero = 0;
    while (first_non_zero < result.size() && result[first_non_zero] == '0') {
        first_non_zero++;
    }
    if (result[first_non_zero] == '-') first_non_zero--;

    return result.substr(first_non_zero);
}

BigInt BigInt::from_hex(const char* hex) {
    BigInt result;
    result.negative = false;

    // Skip leading "0x" if present
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }

    size_t len = 0;
    while (hex[len]) len++;

    // Skip leading zeros
    while (len > 0 && hex[0] == '0') {
        hex++;
        len--;
    }

    if (len == 0) return result;

    size_t num_words = (len + 7) / 8;
    result.words.resize(num_words, 0);

    // Process 8 hex chars at a time from the end
    size_t hex_idx = len;
    for (size_t w = 0; w < num_words && hex_idx > 0; w++) {
        uint32_t word_val = 0;
        size_t chars_to_read = (hex_idx >= 8) ? 8 : hex_idx;
        hex_idx -= chars_to_read;

        for (size_t i = 0; i < chars_to_read; i++) {
            char c = hex[hex_idx + i];
            uint32_t digit;
            if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
            else return BigInt(); // Invalid

            word_val |= digit << (i * 4);
        }

        result.words[w] = word_val;
    }

    result.normalize();
    return result;
}

} // namespace chinstrap