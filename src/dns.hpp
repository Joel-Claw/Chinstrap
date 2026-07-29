// dns.hpp - DNS resolver from scratch using raw UDP sockets
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: What is DNS and why does a browser need to implement it?
//
// DNS (Domain Name System) is the phonebook of the internet. When a user types
// "example.com" into the browser, the browser needs to resolve that human-readable
// name to an IP address (like 93.184.216.34) before it can open a TCP connection.
//
// Most applications use getaddrinfo() from libc, which in turn uses the system
// resolver (usually configured via /etc/resolv.conf). But a from-scratch browser
// implements DNS directly for several reasons:
//
// 1. Control: We can implement DNS-over-HTTPS (DoH), DNS-over-TLS (DoT), or
//    custom caching strategies without relying on the OS.
// 2. Learning: Understanding the DNS wire format is fundamental networking
//    knowledge. DNS is one of the oldest internet protocols (RFC 1035, 1987).
// 3. Performance: We can cache aggressively, prefetch common domains, and
//    manage TTL expiry ourselves rather than depending on system caches.
//
// The DNS wire format is a binary protocol sent over UDP port 53 (or TCP for
// large responses). A DNS message has:
//   - Header (12 bytes): ID, flags, question count, answer count, authority count, additional count
//   - Question section: the query name, type, and class
//   - Answer section: resource records (RRs) with name, type, class, TTL, and data
//   - Authority section: NS records (we parse but do not use these)
//   - Additional section: extra records (we parse but do not use these)
//
// TEACHING NOTE: DNS over HTTPS (DoH)
//
// Traditional DNS sends queries in plaintext over UDP port 53. Anyone on the
// network path can see which domains you are looking up. DNS over HTTPS (RFC 8484)
// encrypts DNS queries inside HTTPS requests to a DoH server (like
// https://cloudflare-dns.com/dns-query). This provides privacy and integrity.
// Chrome supports DoH as an option. We implement traditional UDP DNS here;
// DoH would layer on top of our HTTP client by sending DNS wire format in
// the HTTP request body with content-type application/dns-message.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <optional>
#include <sys/socket.h>
#include <netinet/in.h>

namespace chinstrap {

// TEACHING NOTE: DNS record types
//
// The DNS protocol defines many record types. For a browser, the most important are:
// - A:    IPv4 address (1)
// - AAAA: IPv6 address (28)
// - CNAME: Canonical name - an alias pointing to another name (5)
// - NS:   Name server (2)
// - MX:   Mail exchange (not needed for browsers, but we may see it in responses)
// - TXT:  Text record (16)
// - SOA:  Start of authority (6)
//
// When a browser asks for "example.com", it typically queries for both A and AAAA
// records to get IPv4 and IPv6 addresses. Modern browsers prefer IPv6 (Happy Eyeballs,
// RFC 8305) if available, falling back to IPv4.
enum class DnsType : uint16_t {
    A     = 1,
    NS    = 2,
    CNAME = 5,
    SOA   = 6,
    PTR   = 12,
    MX    = 15,
    TXT   = 16,
    AAAA  = 28,
    SRV   = 33,
};

// TEACHING NOTE: DNS response codes
//
// DNS response codes (RCODE) appear in the header flags. The most common:
// - 0 (NOERROR):  Query succeeded
// - 1 (FORMERR):   Malformed query
// - 2 (SERVFAIL):  Server failed to complete the request
// - 3 (NXDOMAIN):  Domain does not exist
// - 5 (REFUSED):   Query was refused (often policy reasons)
enum class DnsResponseCode : uint8_t {
    NOERROR  = 0,
    FORMERR  = 1,
    SERVFAIL = 2,
    NXDOMAIN = 3,
    NOTIMP   = 4,
    REFUSED  = 5,
};

// A single DNS resource record parsed from a response.
struct DnsRecord {
    std::string name;        // The domain name this record applies to
    DnsType     type;        // A, AAAA, CNAME, etc.
    uint16_t    rr_class;    // Should always be 1 (IN for Internet)
    uint32_t    ttl;         // Time-to-live in seconds
    std::string rdata;       // Raw record data (for A: 4 bytes IPv4, for AAAA: 16 bytes IPv6, for CNAME: domain name)
    // For A/AAAA records, rdata is the raw IP address bytes.
    // For CNAME, rdata is a domain name string.
    // We store rdata as a string and interpret based on type.
};

// A cached DNS entry with expiry time.
struct CachedDnsEntry {
    std::vector<std::string> addresses;  // Resolved IP addresses as strings
    std::chrono::steady_clock::time_point expiry;
    uint32_t ttl;  // Original TTL from the DNS record

    bool is_expired() const {
        return std::chrono::steady_clock::now() >= expiry;
    }
};

// TEACHING NOTE: Why we implement our own DNS cache
//
// The OS typically caches DNS results (via nscd, systemd-resolved, or dnsmasq).
// However, a browser benefits from its own cache because:
// 1. We control cache size and eviction policy
// 2. We can prefetch and pre-warm the cache for popular domains
// 3. We can implement stale-while-revalidate: serve expired entries while
//    revalidating in the background (Chrome does this)
// 4. We avoid the IPC overhead of querying the system cache
//
// Our cache is a simple unordered_map. A production browser might use an LRU
// cache with size limits. Chrome uses a disk-backed DNS cache in addition
// to an in-memory cache.

class DnsResolver {
public:
    DnsResolver();
    ~DnsResolver();

    // Resolve a hostname to IPv4 and/or IPv6 addresses.
    // Returns nullopt if resolution fails.
    // This method checks the cache first, then sends UDP queries to the
    // nameservers found in /etc/resolv.conf.
    std::optional<std::vector<std::string>> resolve(const std::string& hostname);

    // Clear the DNS cache. Useful when the user wants to force re-resolution.
    void clear_cache();

    // Set a custom nameserver (e.g., "8.8.8.8" for Google DNS).
    // Overrides nameservers from resolv.conf.
    void set_nameserver(const std::string& ns);

    // Get the list of nameservers that will be used.
    std::vector<std::string> get_nameservers() const;

    // Public methods for testing and external access
    // These expose internal functionality needed by tests and the HTTP client
    std::vector<uint8_t> build_query(const std::string& hostname, DnsType type, uint16_t id);
    std::vector<DnsRecord> parse_response(const std::vector<uint8_t>& data, DnsType query_type);
    std::string extract_address(const DnsRecord& record);

private:
    // TEACHING NOTE: /etc/resolv.conf parsing
    //
    // /etc/resolv.conf is a text file that configures the system DNS resolver.
    // Lines starting with "#" or ";" are comments. Important directives:
    //   nameserver <IP>  - specifies a DNS server (can appear multiple times)
    //   search <domain>  - appends this domain to short names
    //   domain <domain>  - similar to search but for a single domain
    //   options timeout:<n> - timeout in seconds for queries
    //   options attempts:<n> - number of retry attempts
    //
    // Example resolv.conf:
    //   nameserver 192.168.1.1
    //   nameserver 8.8.8.8
    //   search example.com
    //   options timeout:2 attempts:3
    void load_resolv_conf();

    // Send a DNS query over UDP and receive the response.
    // Handles timeout and retries.
    std::optional<std::vector<uint8_t>> send_query_udp(
        const std::string& nameserver,
        const std::vector<uint8_t>& query,
        int timeout_seconds
    );

    // TEACHING NOTE: DNS name encoding
    //
    // Domain names in DNS messages use "label encoding": each label is
    // prefixed by its length as a single byte, terminated by a zero-length
    // label. For example, "www.example.com" is encoded as:
    //   3 w w w 7 e x a m p l e 3 c o m 0
    // The total length is 3+1+3+7+1+3+1+1 = 17 bytes.
    //
    // DNS also supports "name compression" where a name can point to a
    // previous occurrence using a 2-byte pointer (top two bits set, remaining
    // 14 bits are an offset into the message). We must handle this when
    // parsing responses.
    std::vector<uint8_t> encode_name(const std::string& hostname);

    // Parse a DNS name from a wire-format message, handling compression pointers.
    // Returns the decoded name and advances the offset.
    std::string decode_name(const std::vector<uint8_t>& data, size_t& offset);

    // Cache management
    void cache_store(const std::string& hostname, const std::vector<std::string>& addresses, uint32_t ttl);
    std::optional<CachedDnsEntry> cache_lookup(const std::string& hostname);

    // Check if an IP address string is valid IPv4
    static bool is_ipv4(const std::string& addr);

    // Check if an IP address string is valid IPv6
    static bool is_ipv6(const std::string& addr);

    // If the input is already an IP address, return it directly (no DNS needed).
    // Browsers do this - if you type "127.0.0.1" or "[::1]" into the URL bar,
    // no DNS query is made.
    static std::optional<std::string> try_parse_ip(const std::string& hostname);

    std::vector<std::string> nameservers_;
    std::unordered_map<std::string, CachedDnsEntry> cache_;
    uint16_t next_id_ = 1;

    // Default query timeout in seconds
    static constexpr int DEFAULT_TIMEOUT = 5;
    // Default number of retry attempts
    static constexpr int DEFAULT_ATTEMPTS = 3;
};

} // namespace chinstrap