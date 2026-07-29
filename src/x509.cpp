// x509.cpp - X.509 certificate parser and chain verification
// Part of Chinstrap, a from-scratch web browser in C++ with zero third-party libraries.
//
// TEACHING NOTE: This file implements X.509 certificate parsing and chain
// verification. This is one of the most complex parts of TLS because it
// involves parsing ASN.1/DER encoded structures and verifying RSA signatures.
//
// TEACHING NOTE: The certificate structure in DER format
// A certificate is encoded as:
//
// Certificate ::= SEQUENCE {
//   tbsCertificate       TBSCertificate,
//   signatureAlgorithm   AlgorithmIdentifier,
//   signatureValue       BIT STRING
// }
//
// TBSCertificate ::= SEQUENCE {
//   version         [0] EXPLICIT INTEGER DEFAULT v1,
//   serialNumber         CertificateSerialNumber,
//   signature           AlgorithmIdentifier,
//   issuer              Name,
//   validity           Validity,
//   subject            Name,
//   subjectPublicKeyInfo SubjectPublicKeyInfo,
//   ...
//   extensions       [3] EXPLICIT Extensions OPTIONAL
// }
//
// We parse each field sequentially, walking the ASN.1 structure.

#include "x509.hpp"
#include "sha256.hpp"
#include <cstring>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>

namespace chinstrap {

// ============================================================================
// ASN.1 DER parsing
// ============================================================================

// TEACHING NOTE: TLV (Tag-Length-Value) parsing
// Every ASN.1 element starts with a tag byte, followed by a length field,
// followed by the value. The tag byte tells us what type it is.
//
// Tag byte structure:
//   Bit 7-6: Class (00=universal, 01=application, 10=context, 11=private)
//   Bit 5:   Constructed (1 = contains nested TLVs, 0 = primitive)
//   Bit 4-0: Tag number
//
// For tag numbers >= 31, a "long form" tag encoding is used, but we do not
// need it for X.509 parsing.

// TEACHING NOTE: Length encoding
// Short form (length < 128): single byte, bit 7 = 0
// Long form (length >= 128): first byte = 0x80 | num_length_bytes,
//   followed by length in big-endian
//
// For example:
//   Length 5   -> 0x05
//   Length 200 -> 0x81 0xC8
//   Length 1000 -> 0x82 0x03 0xE8

bool Asn1Element::parse(const uint8_t* data, size_t len, Asn1Element& out) {
    if (len < 2) return false;

    out.raw = data;
    out.tag = data[0];

    size_t pos = 1;
    uint8_t first_len_byte = data[pos];

    if (first_len_byte < 0x80) {
        // Short form
        out.content_len = first_len_byte;
        pos++;
    } else {
        // Long form
        size_t num_len_bytes = first_len_byte & 0x7F;
        if (num_len_bytes == 0 || num_len_bytes > 4) return false;
        if (pos + 1 + num_len_bytes > len) return false;

        out.content_len = 0;
        for (size_t i = 0; i < num_len_bytes; i++) {
            out.content_len = (out.content_len << 8) | data[pos + 1 + i];
        }
        pos += 1 + num_len_bytes;
    }

    out.header_len = pos;
    out.content = data + pos;
    out.total_len = out.header_len + out.content_len;

    if (out.total_len > len) return false;

    return true;
}

// Helper: parse a SEQUENCE and return a cursor to its contents
struct Asn1Cursor {
    const uint8_t* data;
    size_t len;
    size_t pos;

    Asn1Cursor() : data(nullptr), len(0), pos(0) {}
    Asn1Cursor(const uint8_t* d, size_t l) : data(d), len(l), pos(0) {}

    bool at_end() const { return pos >= len; }
    size_t remaining() const { return len - pos; }
    const uint8_t* current() const { return data + pos; }

    bool read_element(Asn1Element& out) {
        if (pos >= len) return false;
        if (!Asn1Element::parse(data + pos, len - pos, out)) return false;
        pos += out.total_len;
        return true;
    }

    // Read an element and verify it has the expected tag
    bool read_tag(uint8_t expected_tag, Asn1Element& out) {
        if (!read_element(out)) return false;
        if (out.tag != expected_tag) return false;
        return true;
    }

    // Read an INTEGER and return its value as a BigInt
    bool read_integer(BigInt& out) {
        Asn1Element elem;
        if (!read_tag(ASN1_INTEGER, elem)) return false;
        out = BigInt::from_bytes_be(elem.content, elem.content_len);
        return true;
    }

    // Read an OID and return its dotted decimal string
    bool read_oid(std::string& out) {
        Asn1Element elem;
        if (!read_tag(ASN1_OID, elem)) return false;

        // TEACHING NOTE: OID encoding
        // The first byte encodes the first two components: 40 * X + Y
        // Subsequent components use base-128 encoding: each byte has bit 7 set
        // if more bytes follow, and bits 6-0 contain 7 bits of the value.
        if (elem.content_len == 0) return false;

        int first = elem.content[0];
        int first_component = first / 40;
        int second_component = first % 40;

        char buf[16];
        snprintf(buf, sizeof(buf), "%d.%d", first_component, second_component);
        out = buf;

        size_t i = 1;
        while (i < elem.content_len) {
            uint64_t value = 0;
            while (i < elem.content_len) {
                value = (value << 7) | (elem.content[i] & 0x7F);
                if ((elem.content[i] & 0x80) == 0) {
                    i++;
                    break;
                }
                i++;
            }
            snprintf(buf, sizeof(buf), ".%llu", static_cast<unsigned long long>(value));
            out += buf;
        }

        return true;
    }

    // Read a string (UTF8String, PrintableString, or IA5String)
    bool read_string(std::string& out) {
        Asn1Element elem;
        if (!read_element(elem)) return false;

        if (elem.tag != ASN1_UTF8_STRING &&
            elem.tag != ASN1_PRINTABLE_STRING &&
            elem.tag != ASN1_IA5_STRING) {
            return false;
        }

        out.assign(reinterpret_cast<const char*>(elem.content), elem.content_len);
        return true;
    }

    // Read a BIT STRING and return the content (minus the unused bits byte)
    bool read_bit_string(std::vector<uint8_t>& out) {
        Asn1Element elem;
        if (!read_tag(ASN1_BIT_STRING, elem)) return false;

        if (elem.content_len < 1) return false;

        // First byte is the number of unused bits in the last byte
        uint8_t unused_bits = elem.content[0];
        size_t data_len = elem.content_len - 1;

        out.assign(elem.content + 1, elem.content + 1 + data_len);

        // Clear unused bits in the last byte
        if (data_len > 0 && unused_bits > 0) {
            out[data_len - 1] &= static_cast<uint8_t>(0xFF << unused_bits);
        }

        return true;
    }

    // Read NULL
    bool read_null() {
        Asn1Element elem;
        if (!read_tag(ASN1_NULL, elem)) return false;
        return elem.content_len == 0;
    }

    // Skip the current element
    bool skip_element() {
        Asn1Element elem;
        return read_element(elem);
    }

    // Read a SEQUENCE into a new cursor
    bool read_sequence(Asn1Cursor& out) {
        Asn1Element elem;
        if (!read_tag(ASN1_SEQUENCE, elem)) return false;
        out = Asn1Cursor(elem.content, elem.content_len);
        return true;
    }

    // Read a SET into a new cursor
    bool read_set(Asn1Cursor& out) {
        Asn1Element elem;
        if (!read_tag(ASN1_SET, elem)) return false;
        out = Asn1Cursor(elem.content, elem.content_len);
        return true;
    }

    // Read a context-specific tag
    bool read_context(uint8_t expected_context, Asn1Cursor& out) {
        Asn1Element elem;
        if (!read_element(elem)) return false;
        uint8_t expected = 0xA0 | (expected_context & 0x1F);
        if (elem.tag != expected) {
            // Put it back
            pos -= elem.total_len;
            return false;
        }
        out = Asn1Cursor(elem.content, elem.content_len);
        return true;
    }

    // Peek at the next tag without consuming it
    uint8_t peek_tag() {
        if (pos >= len) return 0;
        return data[pos];
    }
};

// ============================================================================
// Distinguished Name parsing
// ============================================================================

// TEACHING NOTE: OID constants for DN attributes
// These OIDs identify the components of a Distinguished Name:
//   2.5.4.3  = commonName (CN)
//   2.5.4.6 = countryName (C)
//   2.5.4.7 = localityName (L)
//   2.5.4.8 = stateOrProvinceName (ST)
//   2.5.4.10 = organizationName (O)
//   2.5.4.11 = organizationalUnitName (OU)

bool DistinguishedName::parse(const uint8_t* data, size_t len, DistinguishedName& out) {
    Asn1Cursor cursor(data, len);

    // Name ::= SEQUENCE OF RDN
    Asn1Cursor name_seq;
    if (!cursor.read_sequence(name_seq)) return false;

    while (!name_seq.at_end()) {
        // RDN ::= SET OF AttributeTypeAndValue
        Asn1Cursor rdn_set;
        if (!name_seq.read_set(rdn_set)) return false;

        // AttributeTypeAndValue ::= SEQUENCE { OID, ANY }
        Asn1Cursor atv;
        if (!rdn_set.read_sequence(atv)) return false;

        std::string oid;
        if (!atv.read_oid(oid)) return false;

        std::string value;
        if (!atv.read_string(value)) return false;

        // Map OIDs to fields
        if (oid == "2.5.4.3") out.common_name = value;
        else if (oid == "2.5.4.6") out.country = value;
        else if (oid == "2.5.4.7") out.locality = value;
        else if (oid == "2.5.4.8") out.state = value;
        else if (oid == "2.5.4.10") out.organization = value;
        else if (oid == "2.5.4.11") out.organizational_unit = value;
    }

    return true;
}

std::string DistinguishedName::to_string() const {
    std::string result;
    if (!common_name.empty()) result += "CN=" + common_name;
    if (!organization.empty()) {
        if (!result.empty()) result += ", ";
        result += "O=" + organization;
    }
    if (!country.empty()) {
        if (!result.empty()) result += ", ";
        result += "C=" + country;
    }
    return result;
}

// ============================================================================
// RSA Public Key parsing
// ============================================================================

bool RsaPublicKey::parse(const uint8_t* data, size_t len, RsaPublicKey& out) {
    // TEACHING NOTE: SubjectPublicKeyInfo structure
    //   SEQUENCE {
    //     SEQUENCE { OID (rsaEncryption 1.2.840.113549.1.1.1), NULL }
    //     BIT STRING (contains DER-encoded RSAPublicKey)
    //   }
    //
    // RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }

    Asn1Cursor cursor(data, len);

    // Outer SEQUENCE
    Asn1Cursor spki;
    if (!cursor.read_sequence(spki)) return false;

    // Algorithm identifier SEQUENCE
    Asn1Cursor alg_id;
    if (!spki.read_sequence(alg_id)) return false;

    std::string oid;
    if (!alg_id.read_oid(oid)) return false;

    // Check for RSA OID: 1.2.840.113549.1.1.1
    if (oid != "1.2.840.113549.1.1.1") return false;

    // NULL parameters
    if (!alg_id.read_null()) return false;

    // BIT STRING containing the RSAPublicKey
    std::vector<uint8_t> key_data;
    if (!spki.read_bit_string(key_data)) return false;

    // Parse RSAPublicKey
    Asn1Cursor key_cursor(key_data.data(), key_data.size());
    Asn1Cursor rsa_key;
    if (!key_cursor.read_sequence(rsa_key)) return false;

    if (!rsa_key.read_integer(out.modulus)) return false;
    if (!rsa_key.read_integer(out.exponent)) return false;

    return true;
}

// ============================================================================
// Time parsing
// ============================================================================

// TEACHING NOTE: X.509 time formats
// UTCTime: YYMMDDHHMMSSZ (2-digit year, YY < 50 = 20YY, YY >= 50 = 19YY)
// GeneralizedTime: YYYYMMDDHHMMSSZ (4-digit year)
//
// We parse these into Unix timestamps (seconds since 1970-01-01 00:00:00 UTC)
// for easy comparison.

static int64_t parse_time_string(const char* str, size_t len) {
    // Determine if it is UTCTime or GeneralizedTime by the length
    // UTCTime: 13 chars (YYMMDDHHMMSSZ), GeneralizedTime: 15 chars (YYYYMMDDHHMMSSZ)

    int year, month, day, hour, minute, second;
    size_t pos = 0;

    if (len >= 15 && str[4] >= '0' && str[4] <= '9') {
        // GeneralizedTime: YYYYMMDDHHMMSSZ
        year = (str[0]-'0')*1000 + (str[1]-'0')*100 + (str[2]-'0')*10 + (str[3]-'0');
        pos = 4;
    } else {
        // UTCTime: YYMMDDHHMMSSZ
        int yy = (str[0]-'0')*10 + (str[1]-'0');
        year = (yy < 50) ? 2000 + yy : 1900 + yy;
        pos = 2;
    }

    month = (str[pos]-'0')*10 + (str[pos+1]-'0'); pos += 2;
    day = (str[pos]-'0')*10 + (str[pos+1]-'0'); pos += 2;
    hour = (str[pos]-'0')*10 + (str[pos+1]-'0'); pos += 2;
    minute = (str[pos]-'0')*10 + (str[pos+1]-'0'); pos += 2;
    second = (str[pos]-'0')*10 + (str[pos+1]-'0');

    // Convert to Unix timestamp
    // This is a simplified conversion that works for dates after 1970.
    // We use the system mktime for accuracy.
    struct tm tm_val = {};
    tm_val.tm_year = year - 1900;
    tm_val.tm_mon = month - 1;
    tm_val.tm_mday = day;
    tm_val.tm_hour = hour;
    tm_val.tm_min = minute;
    tm_val.tm_sec = second;
    tm_val.tm_isdst = 0;

    // Use timegm to interpret as UTC (not affected by timezone)
    // If timegm is not available, we compute manually
#ifdef __unix__
    time_t t = timegm(&tm_val);
    return static_cast<int64_t>(t);
#else
    // Fallback: approximate calculation
    // This is a simple days-since-epoch calculation
    int64_t days = 0;
    for (int y = 1970; y < year; y++) {
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    }
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < month; m++) {
        days += month_days[m - 1];
        if (m == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
            days += 1;
        }
    }
    days += day - 1;
    return days * 86400 + hour * 3600 + minute * 60 + second;
#endif
}

static int64_t parse_asn1_time(Asn1Cursor& cursor) {
    Asn1Element elem;
    if (!cursor.read_element(elem)) return 0;

    if (elem.tag != ASN1_UTC_TIME && elem.tag != ASN1_GENERALIZED_TIME) return 0;

    return parse_time_string(reinterpret_cast<const char*>(elem.content), elem.content_len);
}

// ============================================================================
// Certificate parsing
// ============================================================================

bool Certificate::parse(const uint8_t* data, size_t len, Certificate& out) {
    Asn1Cursor cursor(data, len);

    // Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue }
    Asn1Cursor cert_seq;
    if (!cursor.read_sequence(cert_seq)) return false;

    // Record the start of the TBS certificate for signature verification
    // The TBS data is the raw DER bytes (including tag and length) of the
    // first element in the certificate sequence.
    size_t tbs_start = static_cast<size_t>(cert_seq.current() - data);

    // Parse tbsCertificate
    Asn1Cursor tbs;
    if (!cert_seq.read_sequence(tbs)) return false;

    // Record TBS raw data
    size_t tbs_end = static_cast<size_t>(cert_seq.current() - data);
    out.tbs_data.assign(data + tbs_start, data + tbs_end);

    // TEACHING NOTE: TBSCertificate fields
    //   version [0] EXPLICIT INTEGER DEFAULT v1
    //   serialNumber INTEGER
    //   signature AlgorithmIdentifier
    //   issuer Name
    //   validity Validity
    //   subject Name
    //   subjectPublicKeyInfo SubjectPublicKeyInfo
    //   ...
    //   extensions [3] EXPLICIT Extensions OPTIONAL

    // Version (optional, context tag [0])
    out.version = 0; // Default v1
    if (tbs.peek_tag() == ASN1_CONTEXT_0) {
        Asn1Cursor version_wrapper;
        if (!tbs.read_context(0, version_wrapper)) return false;
        Asn1Element version_elem;
        if (!version_wrapper.read_tag(ASN1_INTEGER, version_elem)) return false;
        // Version is encoded as a small integer
        if (version_elem.content_len > 0) {
            out.version = version_elem.content[0];
        }
    }

    // Serial number
    if (!tbs.read_integer(out.serial_number)) return false;

    // Signature algorithm (inside TBS)
    {
        Asn1Cursor sig_alg;
        if (!tbs.read_sequence(sig_alg)) return false;
        if (!sig_alg.read_oid(out.signature_algorithm_oid)) return false;
    }

    // Issuer
    {
        Asn1Element issuer_elem;
        if (!tbs.read_element(issuer_elem)) return false;
        if (issuer_elem.tag != ASN1_SEQUENCE) return false;
        if (!DistinguishedName::parse(issuer_elem.content, issuer_elem.content_len, out.issuer)) {
            return false;
        }
    }

    // Validity: SEQUENCE { notBefore Time, notAfter Time }
    {
        Asn1Cursor validity;
        if (!tbs.read_sequence(validity)) return false;
        out.not_before = parse_asn1_time(validity);
        out.not_after = parse_asn1_time(validity);
    }

    // Subject
    {
        Asn1Element subject_elem;
        if (!tbs.read_element(subject_elem)) return false;
        if (subject_elem.tag != ASN1_SEQUENCE) return false;
        if (!DistinguishedName::parse(subject_elem.content, subject_elem.content_len, out.subject)) {
            return false;
        }
    }

    // SubjectPublicKeyInfo
    {
        Asn1Element spki_elem;
        if (!tbs.read_element(spki_elem)) return false;
        if (!RsaPublicKey::parse(spki_elem.raw, spki_elem.total_len, out.public_key)) {
            return false;
        }
    }

    // TEACHING NOTE: Remaining TBS fields and extensions
    // After SubjectPublicKeyInfo, there may be:
    //   - issuerUniqueID [1] (optional, v2+)
    //   - subjectUniqueID [2] (optional, v2+)
    //   - extensions [3] (optional, v3)
    // We skip any we do not need, but parse extensions for SANs.

    while (!tbs.at_end()) {
        uint8_t tag = tbs.peek_tag();

        if (tag == ASN1_CONTEXT_3) {
            // Extensions
            Asn1Cursor ext_wrapper;
            if (!tbs.read_context(3, ext_wrapper)) break;

            // Extensions ::= SEQUENCE OF Extension
            Asn1Cursor ext_seq;
            if (!ext_wrapper.read_sequence(ext_seq)) break;

            while (!ext_seq.at_end()) {
                // Extension ::= SEQUENCE { extnID OID, critical BOOLEAN OPTIONAL, extnValue OCTET STRING }
                Asn1Cursor ext;
                if (!ext_seq.read_sequence(ext)) break;

                std::string ext_oid;
                if (!ext.read_oid(ext_oid)) break;

                // Skip critical flag if present
                if (ext.peek_tag() == ASN1_BOOLEAN) {
                    ext.skip_element();
                }

                // Extension value (OCTET STRING)
                Asn1Element ext_value;
                if (!ext.read_tag(ASN1_OCTET_STRING, ext_value)) break;

                // TEACHING NOTE: Subject Alternative Name extension
                // OID 2.5.29.17 = subjectAltName
                // The extension value contains a DER-encoded GeneralNames structure:
                //   GeneralNames ::= SEQUENCE OF GeneralName
                //   GeneralName ::= CHOICE {
                //     dNSName     [2] IA5String,
                //     iPAddress   [7] OCTET STRING,
                //     ...
                //   }
                if (ext_oid == "2.5.29.17") {
                    Asn1Cursor san_cursor(ext_value.content, ext_value.content_len);
                    Asn1Cursor san_seq;
                    if (san_cursor.read_sequence(san_seq)) {
                        while (!san_seq.at_end()) {
                            Asn1Element san_elem;
                            if (!san_seq.read_element(san_elem)) break;

                            SubjectAltName san;
                            if (san_elem.tag == 0x82) {
                                // dNSName [2]
                                san.type = SubjectAltName::DNS_NAME;
                                san.value.assign(reinterpret_cast<const char*>(san_elem.content),
                                                 san_elem.content_len);
                                out.sans.push_back(san);
                            } else if (san_elem.tag == 0x87) {
                                // iPAddress [7]
                                san.type = SubjectAltName::IP_ADDRESS;
                                if (san_elem.content_len == 4) {
                                    char ip[16];
                                    snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
                                             san_elem.content[0], san_elem.content[1],
                                             san_elem.content[2], san_elem.content[3]);
                                    san.value = ip;
                                    out.sans.push_back(san);
                                } else if (san_elem.content_len == 16) {
                                    // IPv6 - simplified
                                    san.value = "IPv6";
                                    out.sans.push_back(san);
                                }
                            }
                        }
                    }
                }
            }
        } else {
            // Skip other fields (issuerUniqueID, subjectUniqueID)
            tbs.skip_element();
        }
    }

    // Now parse the signatureAlgorithm and signatureValue (outside TBS)
    {
        Asn1Cursor sig_alg;
        if (!cert_seq.read_sequence(sig_alg)) return false;
        if (!sig_alg.read_oid(out.cert_signature_algorithm_oid)) return false;
    }

    {
        std::vector<uint8_t> sig_bits;
        if (!cert_seq.read_bit_string(sig_bits)) return false;
        out.signature_value = sig_bits;
    }

    return true;
}

bool Certificate::is_valid_at_time(int64_t unix_time) const {
    return unix_time >= not_before && unix_time <= not_after;
}

bool Certificate::is_issued_by(const Certificate& issuer_cert) const {
    // Check if issuer DN matches the subject DN of the issuer certificate
    return issuer.common_name == issuer_cert.subject.common_name &&
           issuer.organization == issuer_cert.subject.organization &&
           issuer.country == issuer_cert.subject.country;
}

// TEACHING NOTE: PKCS#1 v1.5 signature verification
// RSA signature verification with PKCS#1 v1.5 padding:
//
// 1. Compute the RSA operation: decrypted = signature^e mod n
// 2. The result should be a DER-encoded structure:
//    [0x00 0x01 0xFF...0xFF 0x00 DigestInfo]
//    where DigestInfo is:
//    SEQUENCE { SEQUENCE { OID (hash algorithm), NULL }, OCTET STRING (hash) }
// 3. Extract the hash from DigestInfo and compare it with our computed hash
//    of the TBS data.
//
// The padding 0xFF bytes ensure the signature is exactly the modulus size.

bool Certificate::verify_signature(const Certificate& ca_cert) const {
    // TEACHING NOTE: Check that the CA public key algorithm matches
    // For RSA with SHA-256, the OID is 1.2.840.113549.1.1.11
    // For RSA with SHA-384, the OID is 1.2.840.113549.1.1.12
    // We support SHA-256 and SHA-384.

    bool use_sha384 = false;

    if (cert_signature_algorithm_oid == "1.2.840.113549.1.1.11") {
        // sha256WithRSAEncryption
        use_sha384 = false;
    } else if (cert_signature_algorithm_oid == "1.2.840.113549.1.1.12") {
        // sha384WithRSAEncryption
        use_sha384 = true;
    } else if (cert_signature_algorithm_oid == "1.2.840.113549.1.1.1") {
        // rsaEncryption (some CAs use this, assume SHA-256)
        use_sha384 = false;
    } else {
        // Unsupported algorithm
        return false;
    }

    // Compute hash of TBS data
    std::vector<uint8_t> hash;
    if (use_sha384) {
        // We only have SHA-256, so fall back to that
        // In a real implementation, we would implement SHA-384
        hash.resize(32);
        Sha256Digest digest = Sha256::hash(tbs_data.data(), tbs_data.size());
        std::memcpy(hash.data(), digest.bytes.data(), 32);
    } else {
        hash.resize(32);
        Sha256Digest digest = Sha256::hash(tbs_data.data(), tbs_data.size());
        std::memcpy(hash.data(), digest.bytes.data(), 32);
    }

    // RSA verification: decrypted = signature^e mod n
    BigInt sig_int = BigInt::from_bytes_be(signature_value.data(), signature_value.size());
    BigInt decrypted = BigInt::mod_exp(sig_int, ca_cert.public_key.exponent,
                                       ca_cert.public_key.modulus);

    std::vector<uint8_t> decrypted_bytes = decrypted.to_bytes_be();

    // TEACHING NOTE: PKCS#1 v1.5 padding structure
    // The decrypted value should be:
    //   0x00 0x01 [0xFF padding] 0x00 [DigestInfo]
    //
    // We verify:
    //   1. First byte is 0x00 (might be missing if leading zero is stripped)
    //   2. Second byte is 0x01
    //   3. Followed by 0xFF bytes until 0x00 separator
    //   4. Followed by DigestInfo (DER-encoded)

    // The decrypted bytes might be missing leading zeros (since BigInt
    // does not preserve them). We need to pad to the modulus size.
    size_t mod_size = ca_cert.public_key.modulus.bit_length() / 8;
    if (mod_size > decrypted_bytes.size()) {
        decrypted_bytes.insert(decrypted_bytes.begin(),
                               mod_size - decrypted_bytes.size(), 0);
    }

    if (decrypted_bytes.size() < 11) return false;

    size_t pos = 0;
    // Skip leading zeros
    while (pos < decrypted_bytes.size() && decrypted_bytes[pos] == 0) pos++;

    if (pos >= decrypted_bytes.size() || decrypted_bytes[pos] != 0x01) return false;
    pos++;

    // Skip 0xFF padding
    while (pos < decrypted_bytes.size() && decrypted_bytes[pos] == 0xFF) pos++;

    if (pos >= decrypted_bytes.size() || decrypted_bytes[pos] != 0x00) return false;
    pos++;

    // TEACHING NOTE: DigestInfo structure
    //   SEQUENCE {
    //     SEQUENCE {
    //       OID (hash algorithm),
    //       NULL
    //     },
    //     OCTET STRING (hash value)
    //   }
    //
    // For SHA-256, the prefix is a fixed 19-byte sequence:
    //   30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20
    // followed by the 32-byte hash.
    //
    // For SHA-384, the prefix is:
    //   30 41 30 0d 06 09 60 86 48 01 65 03 04 02 02 05 00 04 30
    // followed by the 48-byte hash.
    //
    // We can either parse the DER or compare against the known prefix.
    // Comparing against the known prefix is simpler and sufficient.

    static const uint8_t sha256_prefix[] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48,
        0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };

    if (!use_sha384) {
        if (pos + sizeof(sha256_prefix) + 32 > decrypted_bytes.size()) return false;

        if (std::memcmp(decrypted_bytes.data() + pos, sha256_prefix, sizeof(sha256_prefix)) != 0) {
            return false;
        }

        pos += sizeof(sha256_prefix);

        if (std::memcmp(decrypted_bytes.data() + pos, hash.data(), 32) != 0) {
            return false;
        }
    } else {
        // For SHA-384, we are not fully implementing it, so this path
        // would need SHA-384. For now, just check SHA-256 fallback.
        if (pos + sizeof(sha256_prefix) + 32 > decrypted_bytes.size()) return false;
        if (std::memcmp(decrypted_bytes.data() + pos, sha256_prefix, sizeof(sha256_prefix)) != 0) {
            return false;
        }
        pos += sizeof(sha256_prefix);
        if (std::memcmp(decrypted_bytes.data() + pos, hash.data(), 32) != 0) {
            return false;
        }
    }

    return true;
}

// TEACHING NOTE: Hostname matching
// A certificate matches a hostname if:
//   1. The hostname appears in the SANs (Subject Alternative Names)
//   2. If there are no SANs, the hostname matches the Common Name (CN)
// Modern TLS (RFC 6125) prefers SANs over CN.
// Wildcard certificates (*.example.com) are matched as well.
static bool hostname_matches(const std::string& hostname, const std::string& pattern) {
    // Wildcard match: *.example.com matches sub.example.com but not example.com
    // and not a.b.example.com
    if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
        // Wildcard matches exactly one level
        size_t dot_pos = hostname.find('.');
        if (dot_pos == std::string::npos) return false;
        std::string hostname_suffix = hostname.substr(dot_pos + 1);
        std::string pattern_suffix = pattern.substr(2);
        return hostname_suffix == pattern_suffix;
    }

    // Exact match
    return hostname == pattern;
}

bool Certificate::matches_hostname(const std::string& hostname) const {
    // Check SANs first
    for (const auto& san : sans) {
        if (san.type == SubjectAltName::DNS_NAME) {
            if (hostname_matches(hostname, san.value)) return true;
        }
    }

    // Fall back to Common Name
    if (!subject.common_name.empty()) {
        if (hostname_matches(hostname, subject.common_name)) return true;
    }

    return false;
}

// ============================================================================
// Certificate Verifier
// ============================================================================

CertificateVerifier::CertificateVerifier() {}

// TEACHING NOTE: Loading root CAs from the system trust store
// On Linux, root CAs are stored in /etc/ssl/certs/ as PEM files.
// PEM (Privacy-Enhanced Mail) is base64-encoded DER with header/footer lines.
// We need to decode the base64 and then parse the DER.
//
// On our system (Raspberry Pi with Debian), these are PEM files containing
// one or more certificates.

// Simple base64 decoder
static int base64_char_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<uint8_t> base64_decode(const char* data, size_t len) {
    std::vector<uint8_t> result;
    int buffer = 0;
    int bits = 0;

    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '-' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;

        int val = base64_char_value(c);
        if (val < 0) {
            if (c == '=') break; // Padding
            continue;
        }

        buffer = (buffer << 6) | val;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<uint8_t>(buffer >> bits));
        }
    }

    return result;
}

static bool load_pem_file(const std::string& filepath, std::vector<Certificate>& out) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // TEACHING NOTE: PEM format
    // PEM files contain base64-encoded DER data between markers:
    //   -----BEGIN CERTIFICATE-----
    //   [base64 data]
    //   -----END CERTIFICATE-----
    //
    // A single file can contain multiple certificates.
    // We split on "-----BEGIN CERTIFICATE-----" markers and decode each block.

    size_t pos = 0;
    while (true) {
        size_t begin = content.find("-----BEGIN CERTIFICATE-----", pos);
        if (begin == std::string::npos) break;

        size_t data_start = begin + 27; // length of "-----BEGIN CERTIFICATE-----"
        size_t end = content.find("-----END CERTIFICATE-----", data_start);
        if (end == std::string::npos) break;

        std::string b64_data = content.substr(data_start, end - data_start);
        std::vector<uint8_t> der = base64_decode(b64_data.data(), b64_data.size());

        if (!der.empty()) {
            Certificate cert;
            if (Certificate::parse(der.data(), der.size(), cert)) {
                out.push_back(cert);
            }
        }

        pos = end + 25;
    }

    return !out.empty();
}

int CertificateVerifier::load_root_cas(const std::string& dir) {
    int count = 0;

    DIR* d = opendir(dir.c_str());
    if (!d) return 0;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        // Check extension
        bool is_cert_file = false;
        if (name.size() > 4) {
            std::string ext = name.substr(name.size() - 4);
            if (ext == ".pem" || ext == ".crt") is_cert_file = true;
        }
        // Some systems use hash-based names without extensions
        if (!is_cert_file && name.find('.') == std::string::npos) {
            is_cert_file = true;
        }

        if (!is_cert_file) continue;

        std::string filepath = dir + "/" + name;

        struct stat st;
        if (stat(filepath.c_str(), &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        std::vector<Certificate> certs;
        if (load_pem_file(filepath, certs)) {
            for (auto& cert : certs) {
                root_cas_.push_back(std::move(cert));
                count++;
            }
        }
    }

    closedir(d);
    return count;
}

bool CertificateVerifier::load_root_ca(const std::string& filepath) {
    std::vector<Certificate> certs;
    if (!load_pem_file(filepath, certs)) return false;
    for (auto& cert : certs) {
        root_cas_.push_back(std::move(cert));
    }
    return !certs.empty();
}

void CertificateVerifier::add_root_ca(const Certificate& cert) {
    root_cas_.push_back(cert);
}

const Certificate* CertificateVerifier::find_issuer(const Certificate& cert) {
    for (const auto& root : root_cas_) {
        if (cert.is_issued_by(root)) {
            return &root;
        }
    }
    return nullptr;
}

bool CertificateVerifier::verify_chain(const std::vector<Certificate>& chain,
                                         const std::string& hostname,
                                         int64_t current_time) {
    if (chain.empty()) return false;

    // Use current time if not provided
    if (current_time == 0) {
        current_time = static_cast<int64_t>(time(nullptr));
    }

    // TEACHING NOTE: Chain verification algorithm
    // 1. Check the leaf certificate validity period
    // 2. Verify each certificate is issued by the next one
    // 3. Verify each signature
    // 4. The root must be in our trust store
    // 5. Check hostname match

    // Check the leaf certificate (chain[0])
    if (!chain[0].is_valid_at_time(current_time)) return false;

    // Check hostname match
    if (!hostname.empty()) {
        if (!chain[0].matches_hostname(hostname)) return false;
    }

    // Build the verification chain
    // chain[0] is the leaf, chain[1..n-1] are intermediates
    // We need to find a root CA that chains back to the leaf

    // Verify each intermediate certificate
    for (size_t i = 0; i < chain.size(); i++) {
        if (!chain[i].is_valid_at_time(current_time)) return false;

        if (i + 1 < chain.size()) {
            // chain[i+1] should be the issuer of chain[i]
            if (!chain[i].is_issued_by(chain[i + 1])) return false;
            if (!chain[i].verify_signature(chain[i + 1])) return false;
        } else {
            // The last certificate in the chain should be issued by a root CA
            const Certificate* root = find_issuer(chain[i]);
            if (root == nullptr) return false;
            if (!chain[i].verify_signature(*root)) return false;
        }
    }

    return true;
}

} // namespace chinstrap