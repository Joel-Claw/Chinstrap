// x509.hpp - X.509 certificate parser and chain verification
// Part of Chinstrap, a from-scratch web browser in C++ with zero third-party libraries.
//
// TEACHING NOTE: What is X.509?
// X.509 is the standard format for public key certificates, defined in
// RFC 5280. A certificate binds a public key to an identity (like a domain
// name) through a digital signature from a Certificate Authority (CA).
//
// A certificate contains:
//   - Version (v1, v2, v3)
//   - Serial number (unique per issuer)
//   - Signature algorithm (e.g. SHA-256 with RSA)
//   - Issuer (the CA that issued this certificate)
//   - Validity period (not before, not after)
//   - Subject (the entity this certificate belongs to)
//   - Public key (RSA modulus and exponent)
//   - Extensions (including Subject Alternative Names - SANs)
//   - Signature (the CA signature over the TBS - to-be-signed - portion)
//
// TEACHING NOTE: Certificate chain verification
// When connecting to an HTTPS server, the server presents its certificate
// along with intermediate CA certificates. To verify the chain:
//   1. Check the server certificate validity period
//   2. Find the issuer of the server certificate in the chain
//   3. Verify the signature on the server certificate using the issuer public key
//   4. Repeat for each intermediate certificate up to a root CA
//   5. The root CA must be in the system trust store (/etc/ssl/certs/)
//
// TEACHING NOTE: ASN.1 and DER encoding
// Certificates are encoded using ASN.1 (Abstract Syntax Notation One),
// specifically DER (Distinguished Encoding Rules), which is a binary encoding.
//
// ASN.1 uses TLV (Tag-Length-Value) encoding:
//   - Tag: identifies the type (e.g. INTEGER, OCTET STRING, SEQUENCE)
//   - Length: the number of bytes in the value
//   - Value: the encoded content
//
// Common ASN.1 types:
//   SEQUENCE (0x30): ordered collection of values
//   SET (0x31): unordered collection
//   INTEGER (0x02): arbitrary precision integer
//   BIT STRING (0x03): string of bits
//   OCTET STRING (0x04): string of bytes
//   OBJECT IDENTIFIER (0x06): OID - a dotted decimal identifier
//   UTF8String (0x0C): UTF-8 text
//   PrintableString (0x13): ASCII text
//   IA5String (0x16): ASCII text
//   UTCTime (0x17): date/time in YYMMDDHHMMSSZ format
//   GeneralizedTime (0x18): date/time in YYYYMMDDHHMMSSZ format
//   BOOLEAN (0x01): true/false
//   NULL (0x05): null value
//   CONTEXT [0] (0xA0): context-specific tag 0 (used for extensions)
//
// TEACHING NOTE: DER length encoding
// If the length is < 128, it is encoded as a single byte.
// If the length is >= 128, the first byte is 0x80 | num_length_bytes,
// followed by the length in big-endian.
// For example, length 200 = 0x81 0xC8 (one length byte, value 200)
//              length 1000 = 0x82 0x03 0xE8 (two length bytes, value 1000)

#ifndef CHINSTRAP_X509_HPP
#define CHINSTRAP_X509_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include "bigint.hpp"

namespace chinstrap {

// TEACHING NOTE: ASN.1 tag values
// These are the standard ASN.1 universal class tag values.
// The tag byte encodes class (2 bits), constructed flag (1 bit), and tag number (5 bits).
// Class: 00 = universal, 01 = application, 10 = context-specific, 11 = private
// Constructed: 1 = constructed (contains other TLVs), 0 = primitive
enum Asn1Tag : uint8_t {
    ASN1_BOOLEAN        = 0x01,
    ASN1_INTEGER        = 0x02,
    ASN1_BIT_STRING     = 0x03,
    ASN1_OCTET_STRING   = 0x04,
    ASN1_NULL           = 0x05,
    ASN1_OID            = 0x06,
    ASN1_UTF8_STRING    = 0x0C,
    ASN1_PRINTABLE_STRING = 0x13,
    ASN1_IA5_STRING     = 0x16,
    ASN1_UTC_TIME       = 0x17,
    ASN1_GENERALIZED_TIME = 0x18,
    ASN1_SEQUENCE       = 0x30, // 0x20 (constructed) | 0x10 (sequence)
    ASN1_SET            = 0x31, // 0x20 (constructed) | 0x11 (set)
    // Context-specific tags (used in certificate structure)
    ASN1_CONTEXT_0      = 0xA0,
    ASN1_CONTEXT_1      = 0xA1,
    ASN1_CONTEXT_2      = 0xA2,
    ASN1_CONTEXT_3      = 0xA3,
};

// TEACHING NOTE: ASN.1 TLV element
// This represents a parsed TLV (Tag-Length-Value) element.
struct Asn1Element {
    uint8_t tag;
    size_t header_len; // bytes of tag + length encoding
    size_t content_len; // bytes of value
    const uint8_t* content; // pointer to value in the original buffer
    const uint8_t* raw; // pointer to start of this element (tag byte)
    size_t total_len; // header_len + content_len

    // Parse a TLV element starting at 'data' with 'len' bytes available.
    // Returns true on success, false on malformed input.
    static bool parse(const uint8_t* data, size_t len, Asn1Element& out);
};

// TEACHING NOTE: Distinguished Name
// A DN (Distinguished Name) is a hierarchical name used in X.509.
// It consists of RDNs (Relative Distinguished Names), each being a
// key-value pair like CN=example.com, O=Example Inc, C=US.
struct DistinguishedName {
    std::string common_name;        // CN
    std::string organization;        // O
    std::string organizational_unit; // OU
    std::string country;            // C
    std::string state;              // ST
    std::string locality;           // L

    // Parse a DER-encoded Name (SEQUENCE OF RDN)
    static bool parse(const uint8_t* data, size_t len, DistinguishedName& out);

    std::string to_string() const;
};

// TEACHING NOTE: Subject Alternative Names (SANs)
// SANs are extensions that allow a certificate to cover multiple domain names.
// For example, a certificate for example.com might also list www.example.com
// and example.org as SANs. Modern TLS connections use SANs rather than the
// Common Name for domain name matching.
struct SubjectAltName {
    enum Type {
        DNS_NAME = 2,    // dNSName
        IP_ADDRESS = 7,   // iPAddress
    };

    Type type;
    std::string value; // hostname for DNS, IP string for IP
};

// TEACHING NOTE: RSA Public Key
// An RSA public key consists of a modulus (n) and a public exponent (e).
// These are stored as big integers. The modulus is the product of two primes
// (p * q), and the exponent is typically 65537.
struct RsaPublicKey {
    BigInt modulus;       // n
    BigInt exponent;      // e (typically 65537)

    // Parse from DER-encoded SubjectPublicKeyInfo
    // The format is:
    //   SEQUENCE {
    //     SEQUENCE { OID (rsaEncryption), NULL }
    //     BIT STRING (containing DER-encoded RSAPublicKey)
    //   }
    static bool parse(const uint8_t* data, size_t len, RsaPublicKey& out);
};

// TEACHING NOTE: X.509 Certificate
// This struct holds the parsed fields of an X.509 v3 certificate.
struct Certificate {
    // Version (v1=0, v2=1, v3=2 - stored as integer in the certificate)
    int version;

    // Serial number (arbitrary precision integer)
    BigInt serial_number;

    // Signature algorithm OID
    std::string signature_algorithm_oid;

    // Issuer and Subject DNs
    DistinguishedName issuer;
    DistinguishedName subject;

    // Validity period (Unix timestamps)
    int64_t not_before;
    int64_t not_after;

    // Public key
    RsaPublicKey public_key;

    // Subject Alternative Names
    std::vector<SubjectAltName> sans;

    // The raw TBS (to-be-signed) data and the signature
    // These are needed for signature verification
    std::vector<uint8_t> tbs_data;       // raw DER of the TBS certificate
    std::vector<uint8_t> signature_value; // raw signature bytes

    // Signature algorithm (from the outermost certificate structure)
    std::string cert_signature_algorithm_oid;

    // Parse a DER-encoded certificate
    static bool parse(const uint8_t* data, size_t len, Certificate& out);

    // Check if the certificate is currently valid (within not_before/not_after)
    bool is_valid_at_time(int64_t unix_time) const;

    // Check if this certificate was issued by the given issuer certificate
    // (checks if the issuer DN matches the subject DN of the issuer cert)
    bool is_issued_by(const Certificate& issuer_cert) const;

    // Verify the signature on this certificate using the given CA public key
    bool verify_signature(const Certificate& ca_cert) const;

    // Check if a hostname matches this certificate (via CN or SANs)
    bool matches_hostname(const std::string& hostname) const;
};

// TEACHING NOTE: Certificate chain verification
// The Verifier class loads root CAs from the system trust store and
// verifies certificate chains.
class CertificateVerifier {
public:
    CertificateVerifier();

    // Load root CAs from a directory (default: /etc/ssl/certs/)
    // Reads all .pem or .crt files and parses them as certificates.
    // Returns the number of root CAs loaded.
    int load_root_cas(const std::string& dir = "/etc/ssl/certs/");

    // Load a single root CA from a PEM or DER file
    bool load_root_ca(const std::string& filepath);

    // Add a root CA directly
    void add_root_ca(const Certificate& cert);

    // Verify a certificate chain.
    // chain[0] is the server certificate, chain[1..n-1] are intermediates.
    // Returns true if the chain is valid and trusted.
    bool verify_chain(const std::vector<Certificate>& chain,
                      const std::string& hostname = "",
                      int64_t current_time = 0);

private:
    std::vector<Certificate> root_cas_;

    // Find a root CA whose subject matches the issuer of 'cert'
    const Certificate* find_issuer(const Certificate& cert);
};

} // namespace chinstrap

#endif // CHINSTRAP_X509_HPP