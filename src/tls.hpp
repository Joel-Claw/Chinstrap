// tls.hpp - TLS 1.2 client implementation from scratch
// Part of Chinstrap, a from-scratch web browser in C++ with zero third-party libraries.
//
// TEACHING NOTE: What is TLS?
// TLS (Transport Layer Security) is the protocol that secures HTTPS.
// It provides:
//   - Confidentiality: encrypted data, only client and server can read it
//   - Integrity: any tampering is detected
//   - Authentication: the server proves its identity with a certificate
//
// TEACHING NOTE: TLS 1.2 Handshake Overview
// The TLS 1.2 handshake establishes a secure channel:
//
//   Client                                          Server
//   ------                                          ------
//   ClientHello             -------->
//                           <--------         ServerHello
//                           <--------         Certificate
//                           <--------    ServerKeyExchange
//                           <--------           ServerHelloDone
//   ClientKeyExchange       -------->
//   [ChangeCipherSpec]      -------->
//   Finished                -------->
//                           <--------    [ChangeCipherSpec]
//                           <--------          Finished
//
//   Application Data        <======>        Application Data
//
// After the handshake, both sides derive the same session keys from the
// shared secret (from ECDHE key exchange) and the random values exchanged
// in ClientHello and ServerHello.
//
// TEACHING NOTE: ECDHE Key Exchange
// We use ECDHE (Elliptic Curve Diffie-Hellman Ephemeral) for forward secrecy.
// This means the session keys are destroyed when the connection closes, so
// even if the server private key is later compromised, past connections
// remain secure.
//
// For ECDHE, we need to implement point multiplication on an elliptic curve.
// We use the P-256 curve (secp256r1/NIST P-256) as it is the most widely
// supported curve in TLS 1.2.
//
// TEACHING NOTE: Record Layer
// TLS data is sent in records. Each record has:
//   - ContentType (1 byte): handshake, change_cipher_spec, alert, application_data
//   - Version (2 bytes): 0x0303 for TLS 1.2
//   - Length (2 bytes): payload length
//   - Payload (variable): the actual data
//
// After the handshake, records are encrypted using AES-GCM with the
// negotiated session keys.
//
// TEACHING NOTE: Cipher Suite
// We implement TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384 (0xc030) and
// TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xc02f).
// These are the most common ECDHE-RSA-AES-GCM cipher suites in TLS 1.2.

#ifndef CHINSTRAP_TLS_HPP
#define CHINSTRAP_TLS_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <optional>
#include "sha256.hpp"
#include "aes.hpp"
#include "x509.hpp"
#include "bigint.hpp"

namespace chinstrap {

// TLS content types
enum TlsContentType : uint8_t {
    TLS_CHANGE_CIPHER_SPEC = 20,
    TLS_ALERT             = 21,
    TLS_HANDSHAKE          = 22,
    TLS_APPLICATION_DATA   = 23,
};

// TLS handshake message types
enum TlsHandshakeType : uint8_t {
    TLS_HELLO_REQUEST      = 0,
    TLS_CLIENT_HELLO       = 1,
    TLS_SERVER_HELLO       = 2,
    TLS_CERTIFICATE        = 11,
    TLS_SERVER_KEY_EXCHANGE = 12,
    TLS_CERTIFICATE_REQUEST = 13,
    TLS_SERVER_HELLO_DONE   = 14,
    TLS_CERTIFICATE_VERIFY  = 15,
    TLS_CLIENT_KEY_EXCHANGE = 16,
    TLS_FINISHED           = 20,
};

// TLS alert levels
enum TlsAlertLevel : uint8_t {
    TLS_ALERT_WARNING = 1,
    TLS_ALERT_FATAL   = 2,
};

// TLS alert descriptions
enum TlsAlertDescription : uint8_t {
    TLS_ALERT_CLOSE_NOTIFY         = 0,
    TLS_ALERT_HANDSHAKE_FAILURE    = 40,
    TLS_ALERT_BAD_CERTIFICATE      = 42,
    TLS_ALERT_UNSUPPORTED_CERT    = 43,
    TLS_ALERT_CERTIFICATE_REVOKED = 44,
    TLS_ALERT_CERTIFICATE_EXPIRED = 45,
    TLS_ALERT_CERTIFICATE_UNKNOWN = 46,
    TLS_ALERT_INTERNAL_ERROR      = 80,
    TLS_ALERT_PROTOCOL_VERSION    = 70,
};

// TEACHING NOTE: TLS version numbers
// TLS 1.0 = 0x0301, TLS 1.1 = 0x0302, TLS 1.2 = 0x0303, TLS 1.3 = 0x0304
// Note that in the ClientHello, we may send 0x0303 (TLS 1.2).
// In record layer, the version field is 0x0303 for TLS 1.2.
constexpr uint16_t TLS_VERSION_1_2 = 0x0303;
constexpr uint16_t TLS_VERSION_1_0 = 0x0301; // Used in record layer for compatibility

// TEACHING NOTE: Cipher suite identifiers
// Cipher suites are identified by 2-byte identifiers assigned by IANA.
// We focus on ECDHE-RSA-AES-GCM suites.
constexpr uint16_t TLS_ECDHE_RSA_AES128_GCM_SHA256 = 0xc02f;
constexpr uint16_t TLS_ECDHE_RSA_AES256_GCM_SHA384 = 0xc030;

// TEACHING NOTE: Named curves for ECDHE
// These are the elliptic curves used in TLS for key exchange.
constexpr uint16_t TLS_CURVE_SECP256R1 = 23; // NIST P-256
constexpr uint16_t TLS_CURVE_SECP384R1 = 24; // NIST P-384

// TEACHING NOTE: EC point formats
enum TlsEcPointFormat : uint8_t {
    TLS_EC_UNCOMPRESSED = 0,
};

// TEACHING NOTE: Named group for P-256
// We implement P-256 (secp256r1) as it is the most common curve in TLS 1.2.
// P-256 is a Weierstrass curve y^2 = x^3 + ax + b over a prime field GF(p),
// where p = 2^256 - 2^224 + 2^192 + 2^96 - 1.
// The curve parameters are defined in FIPS 186-4.

// TEACHING NOTE: ECDHE key exchange
// In ECDHE, the server generates an ephemeral EC key pair and sends the
// public key in ServerKeyExchange, signed with the server RSA key.
// The client generates its own EC key pair and sends its public key in
// ClientKeyExchange. Both sides compute the shared secret by multiplying
// their private key by the other side public key.
//
// shared_secret = client_private * server_public = server_private * client_public

// TEACHING NOTE: TLS connection state
// This struct holds the state of a TLS connection during and after the handshake.

struct TlsSession {
    // Negotiated parameters
    uint16_t cipher_suite;
    uint16_t version;

    // Server random and client random (32 bytes each)
    uint8_t client_random[32];
    uint8_t server_random[32];

    // Master secret (48 bytes)
    uint8_t master_secret[48];

    // Session keys (derived from master secret)
    // For AES-GCM, we need:
    //   - client_write_key (16 or 32 bytes depending on AES-128 or AES-256)
    //   - server_write_key (same size)
    //   - client_write_iv (4 bytes implicit nonce)
    //   - server_write_iv (4 bytes implicit nonce)
    uint8_t client_write_key[32];
    uint8_t server_write_key[32];
    uint8_t client_write_iv[4];
    uint8_t server_write_iv[4];
    size_t key_len; // 16 for AES-128, 32 for AES-256

    // Sequence numbers for AEAD (incremented per record)
    uint64_t client_seq_num;
    uint64_t server_seq_num;

    // Server certificate chain
    std::vector<Certificate> certs;

    // Handshake transcript hash (accumulated during handshake)
    std::vector<uint8_t> transcript;

    bool handshake_complete;
    bool is_server; // false for client mode (what we use)
};

// TEACHING NOTE: TLS client class
// The TlsClient class implements the client side of TLS 1.2.
// It uses a raw TCP socket (provided by the caller) to send and receive
// TLS records.
class TlsClient {
public:
    TlsClient();
    ~TlsClient();

    // TEACHING NOTE: Connection lifecycle
    // 1. Call connect() with an already-connected TCP socket and hostname
    // 2. Call read() and write() for application data
    // 3. Call close() to shut down

    // Perform TLS handshake on an already-connected socket.
    // hostname is used for certificate verification (SNI + hostname check).
    // Returns true on success.
    bool connect(int sockfd, const std::string& hostname);

    // Send application data over the encrypted channel
    bool send(int sockfd, const uint8_t* data, size_t len);

    // Receive application data from the encrypted channel
    // Returns the number of bytes read, or -1 on error.
    // Blocks until at least one byte is available or the connection closes.
    ssize_t recv(int sockfd, uint8_t* buf, size_t len);

    // Close the TLS connection (sends close_notify alert)
    void close(int sockfd);

    // Get the server certificate (for inspection after handshake)
    const std::vector<Certificate>& get_certificates() const {
        return session_.certs;
    }

private:
    TlsSession session_;
    int sockfd_;

    // Certificate verifier
    CertificateVerifier verifier_;

    // ECDHE private key (client side)
    uint8_t ec_private_key_[32];
    uint8_t ec_public_key_[65]; // Uncompressed point

    // Buffer for received data (decryption)
    std::vector<uint8_t> recv_buffer_;

    // --- Record layer ---

    // Send a TLS record (plaintext during handshake, encrypted after)
    bool send_record(int sockfd, TlsContentType type,
                     const uint8_t* data, size_t len);

    // Receive a TLS record
    // Returns the content type, or 0 on error.
    // Fills 'data' with the record payload.
    TlsContentType recv_record(int sockfd, std::vector<uint8_t>& data);

    // --- Handshake ---

    bool send_client_hello(int sockfd, const std::string& hostname);
    bool recv_server_hello(int sockfd);
    bool recv_certificate(int sockfd);
    bool recv_server_key_exchange(int sockfd);
    bool recv_server_hello_done(int sockfd);
    bool send_client_key_exchange(int sockfd);
    bool send_change_cipher_spec(int sockfd);
    bool send_finished(int sockfd);
    bool recv_change_cipher_spec(int sockfd);
    bool recv_finished(int sockfd);

    // --- Key derivation ---

    // Compute the master secret from pre-master secret and randoms
    void compute_master_secret(const uint8_t* pre_master, size_t pms_len);

    // Generate key material from master secret
    void generate_keys();

    // --- EC operations (P-256) ---

    // Generate an ephemeral EC key pair on P-256
    void ec_generate_keypair();

    // Compute shared secret from our private key and their public key
    bool ec_compute_shared_secret(const uint8_t* peer_public, size_t peer_len,
                                  uint8_t* shared_x, uint8_t* shared_y);

    // --- Encryption ---

    // Encrypt and send an application data record
    bool send_encrypted_record(int sockfd, const uint8_t* data, size_t len);

    // Decrypt a received record
    bool decrypt_record(TlsContentType type, const uint8_t* ciphertext,
                       size_t ct_len, std::vector<uint8_t>& plaintext);

    // --- Utility ---

    // Add data to the handshake transcript
    void update_transcript(const uint8_t* data, size_t len);

    // Compute the verify_data for the Finished message
    void compute_verify_data(bool is_client, uint8_t* out, size_t& out_len);
};

// TEACHING NOTE: SNI (Server Name Indication)
// SNI is a TLS extension that allows the client to tell the server which
// hostname it is connecting to. This is needed because a single IP address
// can host multiple HTTPS servers with different certificates. Without SNI,
// the server would not know which certificate to present.
// We include the SNI extension in our ClientHello.

} // namespace chinstrap

#endif // CHINSTRAP_TLS_HPP