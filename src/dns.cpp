// dns.cpp - DNS resolver implementation from scratch using raw UDP sockets
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: How DNS resolution works end to end
//
// When the browser needs to load "https://example.com/page", the flow is:
//
// 1. URL parser extracts hostname "example.com"
// 2. DNS resolver checks its local cache for "example.com"
//    - If cached and not expired: return cached IP immediately
// 3. If not cached, the resolver reads /etc/resolv.conf to find nameserver IPs
//    (typically the local router, or 127.0.0.53 if systemd-resolved is running)
// 4. The resolver builds a DNS query message in wire format:
//    - 12-byte header with a random query ID, flags (recursion desired), and counts
//    - Question section: encoded domain name + type (A or AAAA) + class (IN)
// 5. The resolver sends the query via a UDP socket to port 53 of the nameserver
//    - UDP is used because DNS queries are small and fit in one datagram
//    - If the response is too large (>512 bytes traditional, >1232 bytes with EDNS),
//      the server may set a "truncated" flag and the resolver should retry over TCP
// 6. The resolver waits for a response with a timeout (typically 5 seconds)
//    - If no response, it retries with the next nameserver
// 7. The resolver parses the response:
//    - Validates the response ID matches the query ID
//    - Checks the response code (NOERROR, NXDOMAIN, SERVFAIL, etc.)
//    - Parses answer records, following CNAME chains if needed
//    - Extracts A/AAAA addresses
// 8. The resolver caches the results with the TTL from the DNS records
//    - TTL is the number of seconds the record is valid
//    - When TTL expires, the entry is removed and the next lookup queries again
// 9. The resolver returns the IP address(es) to the HTTP client
//
// TEACHING NOTE: Why UDP and not TCP for DNS?
//
// DNS traditionally uses UDP port 53 because:
// - DNS queries are small (usually < 100 bytes) and responses are usually < 512 bytes
// - UDP has no connection setup overhead (no SYN/SYN-ACK/ACK handshake)
// - This makes DNS resolution very fast for the common case
// - TCP is used as a fallback when the response is truncated or for zone transfers
// - DNS over TCP uses the same wire format but prefixes each message with a 2-byte length
//
// Modern DNS extensions (EDNS0, RFC 6891) allow larger UDP payloads up to 4096 bytes,
// reducing the need for TCP fallback. DNS over HTTPS and DNS over TLS always use TCP
// because they wrap DNS in TLS which is itself TCP-based.

#include "dns.hpp"

#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <random>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <cerrno>
#include <cstdio>

namespace chinstrap {

// TEACHING NOTE: DNS header structure (RFC 1035, Section 4.1.1)
//
// The DNS header is always 12 bytes and appears at the start of every DNS message:
//
//                                 1  1  1  1  1  1
//   0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                      ID                       |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |QR|   Opcode  |AA|TC|RD|RA|   Z    |   RCODE    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                    QDCOUNT                    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                    ANCOUNT                    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                    NSCOUNT                    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                    ARCOUNT                    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//
// - ID: 16-bit identifier, echoed back in response for matching
// - QR: Query (0) or Response (1)
// - Opcode: 4 bits, standard query is 0
// - AA: Authoritative Answer flag
// - TC: TrunCation flag - response was too long, retry with TCP
// - RD: Recursion Desired - ask the nameserver to recurse if needed
// - RA: Recursion Available - server supports recursion
// - Z: Reserved (must be 0)
// - RCODE: Response code (0=NOERROR, 3=NXDOMAIN, etc.)
// - QDCOUNT: Number of questions
// - ANCOUNT: Number of answer records
// - NSCOUNT: Number of authority records
// - ARCOUNT: Number of additional records

namespace {

// Bit manipulation helpers for reading/writing DNS wire format.
// DNS uses network byte order (big-endian) for all multi-byte fields.

void write_u16(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

// write_u32 is not currently used but kept for future DNS record building
[[maybe_unused]] void write_u32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

uint16_t read_u16(const std::vector<uint8_t>& buf, size_t offset) {
    return static_cast<uint16_t>((buf[offset] << 8) | buf[offset + 1]);
}

uint32_t read_u32(const std::vector<uint8_t>& buf, size_t offset) {
    return (static_cast<uint32_t>(buf[offset]) << 24) |
           (static_cast<uint32_t>(buf[offset + 1]) << 16) |
           (static_cast<uint32_t>(buf[offset + 2]) << 8) |
           static_cast<uint32_t>(buf[offset + 3]);
}

} // anonymous namespace

DnsResolver::DnsResolver() {
    load_resolv_conf();
    if (nameservers_.empty()) {
        // TEACHING NOTE: Fallback nameservers
        // If resolv.conf is missing or has no nameserver entries, we fall back
        // to common public DNS servers. In production, a browser might not do
        // this (it would show an error), but for a from-scratch implementation
        // this provides reasonable defaults.
        nameservers_.push_back("8.8.8.8");    // Google Public DNS
        nameservers_.push_back("1.1.1.1");    // Cloudflare DNS
    }
}

DnsResolver::~DnsResolver() = default;

void DnsResolver::load_resolv_conf() {
    // TEACHING NOTE: Parsing /etc/resolv.conf
    //
    // This file is typically managed by the OS network configuration tool
    // (NetworkManager, systemd-resolved, dhclient, etc.). We parse it to find
    // the nameserver IP addresses. On many modern Linux systems, the
    // nameserver is 127.0.0.53 (systemd-resolved stub resolver).
    std::ifstream file("/etc/resolv.conf");
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // Parse "nameserver <IP>" lines
        std::istringstream iss(line);
        std::string keyword;
        iss >> keyword;

        if (keyword == "nameserver") {
            std::string ns;
            if (iss >> ns) {
                nameservers_.push_back(ns);
            }
        }
        // We ignore "search", "domain", and "options" directives for now.
        // A production browser would parse "options timeout:" and "options attempts:"
        // to configure its own timeout and retry behavior.
    }
}

void DnsResolver::set_nameserver(const std::string& ns) {
    nameservers_.clear();
    nameservers_.push_back(ns);
}

std::vector<std::string> DnsResolver::get_nameservers() const {
    return nameservers_;
}

void DnsResolver::clear_cache() {
    cache_.clear();
}

// TEACHING NOTE: DNS name encoding (RFC 1035, Section 3.1)
//
// A domain name is encoded as a sequence of labels. Each label is:
//   [1 byte length] [label characters]
// The sequence ends with a zero-length label (a single 0x00 byte).
//
// For example, "www.example.com" breaks into labels:
//   "www", "example", "com"
// Encoded as:
//   \x03 w w w \x07 e x a m p l e \x03 c o m \x00
//
// Each label can be at most 63 characters (the length byte's top 2 bits
// are reserved for compression pointers). The total encoded name should
// not exceed 255 bytes.
//
// DNS name compression (RFC 1035, Section 4.1.4) allows a name to reference
// a previous name in the same message using a pointer. A pointer is a 2-byte
// value where the top two bits are 11 (0xC0), and the remaining 14 bits are
// an offset from the start of the message. For example, if "example.com"
// appears at byte offset 20, a later occurrence could be encoded as 0xC0 0x14
// (0xC0 | (20 >> 8), 20 & 0xFF). This significantly reduces message size
// when a name appears multiple times (common in DNS responses).

std::vector<uint8_t> DnsResolver::encode_name(const std::string& hostname) {
    std::vector<uint8_t> result;

    // Split hostname by dots and encode each label
    size_t start = 0;
    while (start < hostname.size()) {
        size_t end = hostname.find('.', start);
        if (end == std::string::npos) {
            end = hostname.size();
        }

        size_t label_len = end - start;
        if (label_len == 0) {
            // Skip empty labels (e.g., trailing dot in FQDN)
            start = end + 1;
            continue;
        }

        if (label_len > 63) {
            throw std::runtime_error("DNS label too long (max 63 bytes)");
        }

        result.push_back(static_cast<uint8_t>(label_len));
        for (size_t i = start; i < end; ++i) {
            result.push_back(static_cast<uint8_t>(hostname[i]));
        }

        start = end + 1;
    }

    // Terminate with zero-length label
    result.push_back(0);
    return result;
}

std::string DnsResolver::decode_name(const std::vector<uint8_t>& data, size_t& offset) {
    // TEACHING NOTE: DNS name decoding with compression pointer handling
    //
    // When parsing a DNS name, we may encounter:
    // 1. A regular label: length byte (0-63), followed by that many characters
    // 2. A compression pointer: two bytes where the first byte has its top
    //    two bits set (0xC0 mask). The remaining 14 bits are an offset into
    //    the message where the rest of the name can be found.
    // 3. A zero-length label: end of the name
    //
    // When we encounter a pointer, we save the current offset (for the caller
    // to continue parsing after the name), jump to the pointer offset, and
    // continue decoding from there. The jumped-to location may itself contain
    // pointers, so this is recursive. However, we must be careful to avoid
    // infinite loops (a pointer pointing to itself or creating a cycle).
    // We track visited offsets to detect cycles.

    std::string name;
    bool jumped = false;
    size_t original_offset = offset;

    // Track offsets we have visited via pointers to detect cycles
    std::vector<size_t> visited;

    while (offset < data.size()) {
        uint8_t len = data[offset];

        if (len == 0) {
            // End of name
            if (!jumped) {
                offset += 1;  // Skip the zero byte
            } else {
                offset = original_offset + 2;  // After the 2-byte pointer
            }
            break;
        }

        // Check if this is a compression pointer (top two bits set)
        if ((len & 0xC0) == 0xC0) {
            // Compression pointer
            if (offset + 1 >= data.size()) {
                throw std::runtime_error("DNS name pointer truncated");
            }

            uint16_t pointer = static_cast<uint16_t>(((len & 0x3F) << 8) | data[offset + 1]);

            if (!jumped) {
                // Save the position after the pointer for the caller
                original_offset = offset;
                jumped = true;
            }

            // Check for cycles
            if (std::find(visited.begin(), visited.end(), pointer) != visited.end()) {
                throw std::runtime_error("DNS name compression pointer cycle detected");
            }
            visited.push_back(pointer);

            offset = pointer;
            continue;
        }

        // Regular label
        if (offset + 1 + len > data.size()) {
            throw std::runtime_error("DNS label extends beyond message");
        }

        if (!name.empty()) {
            name += '.';
        }

        for (uint8_t i = 0; i < len; ++i) {
            name += static_cast<char>(data[offset + 1 + i]);
        }

        offset += 1 + len;
    }

    if (jumped) {
        // Restore offset to after the pointer
        offset = original_offset + 2;
    }

    return name;
}

// TEACHING NOTE: Building a DNS query message
//
// A DNS query message consists of:
// 1. Header (12 bytes):
//    - ID: random 16-bit number to match response with query
//    - Flags: RD (Recursion Desired) bit set - ask the resolver to do recursive lookup
//    - QDCOUNT=1 (one question), ANCOUNT=0, NSCOUNT=0, ARCOUNT=0
// 2. Question section:
//    - Encoded domain name
//    - QTYPE: type of record we want (A=1 for IPv4, AAAA=28 for IPv6)
//    - QCLASS: 1 (IN for Internet)
//
// We set the RD (Recursion Desired) flag because we want the nameserver to
// follow CNAME chains and contact other servers on our behalf. Without RD,
// the server might just return a referral to another server.

std::vector<uint8_t> DnsResolver::build_query(const std::string& hostname, DnsType type, uint16_t id) {
    std::vector<uint8_t> query;

    // Header
    write_u16(query, id);                    // ID
    write_u16(query, 0x0100);                // Flags: RD (Recursion Desired) set
    write_u16(query, 1);                     // QDCOUNT: 1 question
    write_u16(query, 0);                     // ANCOUNT: 0
    write_u16(query, 0);                     // NSCOUNT: 0
    write_u16(query, 0);                     // ARCOUNT: 0

    // Question section
    auto name = encode_name(hostname);
    query.insert(query.end(), name.begin(), name.end());
    write_u16(query, static_cast<uint16_t>(type));  // QTYPE
    write_u16(query, 1);                             // QCLASS: IN

    return query;
}

// TEACHING NOTE: Parsing a DNS response message
//
// The DNS response has the same header format as the query, but with the
// QR bit set to 1 (response) and the answer/authority/additional counts
// populated. We parse each section:
//
// 1. Validate the header: check ID matches our query, QR is 1, check RCODE
// 2. Skip the question section (we know what we asked)
// 3. Parse answer records: each RR has name, type, class, TTL, RDLENGTH, RDATA
// 4. Follow CNAME records to get the final A/AAAA records
//
// Resource Record format (RFC 1035, Section 3.2.1):
//                                 1  1  1  1  1  1
//   0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                                               |
//  /                                               /
//  /                      NAME                     /
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                      TYPE                     |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                     CLASS                      |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                      TTL                      |
//  |                                               |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  |                   RDLENGTH                    |
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//  /                     RDATA                     /
//  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+

std::vector<DnsRecord> DnsResolver::parse_response(const std::vector<uint8_t>& data, DnsType query_type) {
    (void)query_type;  // We parse all answer records regardless of query type
    if (data.size() < 12) {
        throw std::runtime_error("DNS response too short for header");
    }

    // Parse header
    uint16_t id = read_u16(data, 0);
    (void)id;  // We should verify this matches our query ID

    uint16_t flags = read_u16(data, 2);
    bool is_response = (flags & 0x8000) != 0;
    uint8_t rcode = flags & 0x000F;

    if (!is_response) {
        throw std::runtime_error("DNS message is not a response (QR bit not set)");
    }

    // TEACHING NOTE: DNS response codes
    // We check the RCODE field. NXDOMAIN (3) means the domain does not exist.
    // SERVFAIL (2) means the server had an error. These are not parse errors
    // but rather DNS-level errors that we should report to the caller.
    if (rcode != 0) {
        std::string errmsg = "DNS response code: ";
        switch (rcode) {
            case 1:  errmsg += "FORMERR (format error)"; break;
            case 2:  errmsg += "SERVFAIL (server failure)"; break;
            case 3:  errmsg += "NXDOMAIN (domain does not exist)"; break;
            case 5:  errmsg += "REFUSED (query refused)"; break;
            default: errmsg += "RCODE=" + std::to_string(rcode); break;
        }
        throw std::runtime_error(errmsg);
    }

    uint16_t qdcount = read_u16(data, 4);
    uint16_t ancount = read_u16(data, 6);
    uint16_t nscount = read_u16(data, 8);
    uint16_t arcount = read_u16(data, 10);
    (void)nscount;
    (void)arcount;

    size_t offset = 12;

    // Skip question section
    for (uint16_t i = 0; i < qdcount; ++i) {
        // Skip the name
        decode_name(data, offset);
        // Skip QTYPE (2 bytes) and QCLASS (2 bytes)
        offset += 4;
    }

    // Parse answer section
    std::vector<DnsRecord> answers;
    for (uint16_t i = 0; i < ancount; ++i) {
        DnsRecord record;
        record.name = decode_name(data, offset);

        if (offset + 10 > data.size()) {
            throw std::runtime_error("DNS answer record truncated");
        }

        record.type = static_cast<DnsType>(read_u16(data, offset));
        offset += 2;
        record.rr_class = read_u16(data, offset);
        offset += 2;
        record.ttl = read_u32(data, offset);
        offset += 4;

        uint16_t rdlength = read_u16(data, offset);
        offset += 2;

        if (offset + rdlength > data.size()) {
            throw std::runtime_error("DNS RDATA extends beyond message");
        }

        // TEACHING NOTE: RDATA interpretation
        //
        // The RDATA field content depends on the record type:
        // - A (type 1): 4 bytes, IPv4 address in network byte order
        // - AAAA (type 28): 16 bytes, IPv6 address in network byte order
        // - CNAME (type 5): encoded domain name (with possible compression)
        // - NS (type 2): encoded domain name
        // - MX (type 15): 2-byte preference + encoded domain name
        // - TXT (type 16): one or more length-prefixed strings
        //
        // For A and AAAA, we store raw bytes. For CNAME, we decode the name.
        if (record.type == DnsType::CNAME) {
            // Decode the CNAME target (may use compression pointers)
            size_t cname_offset = offset;
            record.rdata = decode_name(data, cname_offset);
        } else {
            // Store raw RDATA bytes as a string
            record.rdata = std::string(reinterpret_cast<const char*>(&data[offset]), rdlength);
        }

        offset += rdlength;
        answers.push_back(record);
    }

    // We do not parse authority and additional sections for now.
    // A production browser might parse additional records for EDNS
    // (Extension Mechanisms for DNS) to learn about server capabilities.

    return answers;
}

// TEACHING NOTE: Sending DNS queries over UDP
//
// We create a UDP socket, set a receive timeout using SO_RCVTIMEO, send the
// query to the nameserver on port 53, and wait for a response. If the timeout
// expires, we try the next nameserver. If all nameservers fail, we return
// nullopt.
//
// We use non-blocking I/O with select() for the timeout. This is more portable
// than SO_RCVTIMEO and allows us to handle the case where the socket is not
// ready for reading.
//
// In a real browser, DNS resolution would be asynchronous (not blocking the UI
// thread). Chrome uses an async DNS resolver that can issue multiple queries
// in parallel. Our implementation is synchronous for simplicity; the HTTP
// client would call this from a worker thread.

std::optional<std::vector<uint8_t>> DnsResolver::send_query_udp(
    const std::string& nameserver,
    const std::vector<uint8_t>& query,
    int timeout_seconds
) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return std::nullopt;
    }

    // Set socket to non-blocking mode
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        close(sock);
        return std::nullopt;
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        return std::nullopt;
    }

    // Set up destination address
    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(53);
    if (inet_pton(AF_INET, nameserver.c_str(), &dest.sin_addr) <= 0) {
        close(sock);
        return std::nullopt;
    }

    // Send the query
    ssize_t sent = sendto(sock, query.data(), query.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    if (sent < 0 || static_cast<size_t>(sent) != query.size()) {
        close(sock);
        return std::nullopt;
    }

    // Wait for response with timeout using select()
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock, &read_fds);

    struct timeval tv{};
    tv.tv_sec = timeout_seconds;
    tv.tv_usec = 0;

    int ready = select(sock + 1, &read_fds, nullptr, nullptr, &tv);
    if (ready <= 0) {
        // Timeout or error
        close(sock);
        return std::nullopt;
    }

    // Read the response
    std::vector<uint8_t> response(4096);
    socklen_t from_len = sizeof(dest);
    ssize_t received = recvfrom(sock, response.data(), response.size(), 0,
                                reinterpret_cast<struct sockaddr*>(&dest), &from_len);

    close(sock);

    if (received <= 0) {
        return std::nullopt;
    }

    response.resize(static_cast<size_t>(received));
    return response;
}

std::string DnsResolver::extract_address(const DnsRecord& record) {
    // TEACHING NOTE: Converting DNS record data to IP address strings
    //
    // For A records, RDATA is 4 bytes in network byte order.
    // We convert to "a.b.c.d" format using inet_ntop.
    //
    // For AAAA records, RDATA is 16 bytes in network byte order.
    // We convert to IPv6 string format (e.g., "2001:db8::1").
    //
    // For CNAME records, the rdata is already the decoded domain name string.

    if (record.type == DnsType::A) {
        if (record.rdata.size() != 4) {
            return "";
        }
        char buf[INET_ADDRSTRLEN];
        struct in_addr addr;
        std::memcpy(&addr, record.rdata.data(), 4);
        inet_ntop(AF_INET, &addr, buf, sizeof(buf));
        return buf;
    }

    if (record.type == DnsType::AAAA) {
        if (record.rdata.size() != 16) {
            return "";
        }
        char buf[INET6_ADDRSTRLEN];
        struct in6_addr addr;
        std::memcpy(&addr, record.rdata.data(), 16);
        inet_ntop(AF_INET6, &addr, buf, sizeof(buf));
        return buf;
    }

    // For CNAME, return the target name
    if (record.type == DnsType::CNAME) {
        return record.rdata;
    }

    return "";
}

bool DnsResolver::is_ipv4(const std::string& addr) {
    struct in_addr out;
    return inet_pton(AF_INET, addr.c_str(), &out) == 1;
}

bool DnsResolver::is_ipv6(const std::string& addr) {
    struct in6_addr out;
    return inet_pton(AF_INET6, addr.c_str(), &out) == 1;
}

std::optional<std::string> DnsResolver::try_parse_ip(const std::string& hostname) {
    // TEACHING NOTE: IP address shortcuts
    //
    // If the user types an IP address directly (e.g., "192.168.1.1" or
    // "[::1]"), we do not need to do DNS resolution at all. This is an
    // important optimization - DNS resolution adds latency and we should
    // skip it when the input is already an IP address.
    //
    // Browsers also handle IPv6 addresses in URLs wrapped in brackets
    // (e.g., "http://[::1]:8080/") to disambiguate the colon used in IPv6
    // addresses from the colon used to separate host and port.

    // Strip brackets from IPv6 address in URL format
    std::string addr = hostname;
    if (addr.size() >= 2 && addr.front() == '[' && addr.back() == ']') {
        addr = addr.substr(1, addr.size() - 2);
    }

    if (is_ipv4(addr)) {
        return addr;
    }

    if (is_ipv6(addr)) {
        return addr;
    }

    return std::nullopt;
}

void DnsResolver::cache_store(
    const std::string& hostname,
    const std::vector<std::string>& addresses,
    uint32_t ttl
) {
    if (addresses.empty() || ttl == 0) {
        return;
    }

    CachedDnsEntry entry;
    entry.addresses = addresses;
    entry.ttl = ttl;
    entry.expiry = std::chrono::steady_clock::now() + std::chrono::seconds(ttl);
    cache_[hostname] = std::move(entry);
}

std::optional<CachedDnsEntry> DnsResolver::cache_lookup(const std::string& hostname) {
    auto it = cache_.find(hostname);
    if (it == cache_.end()) {
        return std::nullopt;
    }

    if (it->second.is_expired()) {
        cache_.erase(it);
        return std::nullopt;
    }

    return it->second;
}

// TEACHING NOTE: The full resolution flow
//
// Our resolve() method implements the complete DNS resolution flow:
//
// 1. Check if the input is already an IP address (skip DNS)
// 2. Check the local cache for a non-expired entry
// 3. If not cached, query for A records (IPv4) and AAAA records (IPv6)
//    - We query both types to support dual-stack connections
//    - Modern browsers use "Happy Eyeballs" (RFC 8305) to race IPv4 and IPv6
//      connections and use whichever connects first
// 4. Follow CNAME chains if the initial query returns CNAME records
// 5. Cache the results with TTL
// 6. Return all addresses (both IPv4 and IPv6)
//
// We handle CNAME chains by repeatedly querying for the target name.
// In practice, the recursive resolver (the nameserver we query) usually
// follows CNAME chains for us (because we set RD=1), but we handle the
// case where the response contains CNAME records mixed with A/AAAA records.

std::optional<std::vector<std::string>> DnsResolver::resolve(const std::string& hostname) {
    // Step 1: Check if it is already an IP address
    auto ip = try_parse_ip(hostname);
    if (ip) {
        return std::vector<std::string>{*ip};
    }

    // Step 2: Check cache
    auto cached = cache_lookup(hostname);
    if (cached) {
        return cached->addresses;
    }

    // Step 3: Query DNS for A and AAAA records
    std::vector<std::string> all_addresses;
    uint32_t min_ttl = UINT32_MAX;

    // Generate a query ID using a random number generator
    // TEACHING NOTE: DNS query IDs should be unpredictable to prevent
    // DNS spoofing/poisoning attacks. An attacker who can predict the
    // query ID can send a fake response before the real server responds.
    // Modern DNS implementations use random query IDs and source ports.
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dist(1, 65535);

    // Query for A records (IPv4)
    for (int type_idx = 0; type_idx < 2; ++type_idx) {
        DnsType qtype = (type_idx == 0) ? DnsType::A : DnsType::AAAA;
        uint16_t query_id = dist(gen);

        auto query = build_query(hostname, qtype, query_id);

        // Try each nameserver until we get a response
        std::optional<std::vector<uint8_t>> response_data;
        for (const auto& ns : nameservers_) {
            response_data = send_query_udp(ns, query, DEFAULT_TIMEOUT);
            if (response_data) {
                break;
            }
        }

        if (!response_data) {
            continue;
        }

        // Verify response ID matches query ID
        if (response_data->size() < 2) {
            continue;
        }
        uint16_t resp_id = read_u16(*response_data, 0);
        if (resp_id != query_id) {
            // ID mismatch - possible spoofing attack or stale response
            continue;
        }

        // Parse the response
        std::vector<DnsRecord> answers;
        try {
            answers = parse_response(*response_data, qtype);
        } catch (const std::runtime_error&) {
            // Parse error - try next query type
            continue;
        }

        // Process answers: follow CNAME chains and collect A/AAAA records
        std::string current_name = hostname;
        for (const auto& record : answers) {
            if (record.type == DnsType::CNAME) {
                // CNAME - update the current name and continue looking
                current_name = record.rdata;
                min_ttl = std::min(min_ttl, record.ttl);
            } else if (record.type == qtype) {
                std::string addr = extract_address(record);
                if (!addr.empty()) {
                    all_addresses.push_back(addr);
                }
                min_ttl = std::min(min_ttl, record.ttl);
            }
        }
    }

    if (all_addresses.empty()) {
        return std::nullopt;
    }

    // Step 5: Cache the results
    if (min_ttl == UINT32_MAX) {
        min_ttl = 300;  // Default TTL of 5 minutes if not specified
    }
    cache_store(hostname, all_addresses, min_ttl);

    return all_addresses;
}

} // namespace chinstrap