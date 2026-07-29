// history.hpp - Browsing history management
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: Browsing history and privacy
//
// Browsing history is one of the most sensitive types of data a browser
// stores. It reveals what websites a user has visited, when, and how often.
// This information can be embarrassing, personal, or even dangerous in some
// contexts (e.g., visiting health information sites, political sites in
// oppressive regimes).
//
// Privacy implications:
// - History can be subpoenaed by law enforcement
// - History can be accessed by anyone with physical access to the device
// - Browser history can leak via browser sync features
// - Some browsers have "incognito" or "private" mode that does not store history
// - Chrome offers "auto-delete" options (delete history older than 3/18/36 months)
//
// How Chrome stores history:
// Chrome uses a SQLite database (the History database) with tables:
// - urls: URL, title, visit count, last visit time, typed count
// - visits: URL ID, visit time, transition type (typed, link, reload, etc.)
// - visit_source: visit ID, source (synced, local, etc.)
// - keyword_search_terms: search engine queries
//
// The visits table allows Chrome to track multiple visits to the same URL
// and record how the user arrived (typed, clicked a link, redirected, etc.).
//
// We use our own key-value store for history with a time-sorted index.
// History entries are stored as: "history:<timestamp>" -> "url|title|visit_count"
//
// TEACHING NOTE: History queries
//
// A history feature needs to support several query types:
// - Recent visits: Get the most recent N entries (for the history page)
// - Search by URL: Find entries matching a URL fragment
// - Search by title: Find entries matching a title fragment
// - Visits to a specific URL: How many times and when was this URL visited?
//
// These queries require different access patterns:
// - Recent: sorted by time (descending)
// - Search by URL: scan all entries, filter by URL substring
// - Search by title: scan all entries, filter by title substring
// - Visits to URL: index by URL -> list of timestamps
//
// A production browser would use a database with indexes for efficient queries.
// We use a simple in-memory scan since our dataset is small (browsers typically
// limit history to a few thousand entries).

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include "storage.hpp"

namespace chinstrap {

// A single browsing history entry
struct HistoryEntry {
    std::string url;                      // The URL visited
    std::string title;                     // Page title (may be empty if not loaded yet)
    int64_t visit_time;                   // Unix timestamp of the visit
    int visit_count = 1;                  // How many times this URL was visited
    int64_t last_visit_time = 0;         // Last visit timestamp (for dedup)

    // Serialize to a string for storage (using | as delimiter)
    std::string serialize() const;

    // Deserialize from a stored string
    static std::optional<HistoryEntry> deserialize(const std::string& data);
};

// TEACHING NOTE: History manager
//
// The history manager wraps the key-value store with history-specific logic:
// - Record a visit (add or update an entry)
// - Query recent visits (sorted by time, most recent first)
// - Search by URL or title (case-insensitive substring match)
// - Delete individual entries or clear all history
// - Limit the number of stored entries (auto-trim old entries)
//
// We store two indexes:
// 1. "history:<timestamp>:<url_hash>" -> serialized entry (for time-sorted access)
// 2. "url:<url>" -> timestamp (for dedup: if we have visited this URL before,
//    we update the visit count and last_visit_time instead of creating a new entry)

class HistoryManager {
public:
    HistoryManager();
    ~HistoryManager();

    // Open the history database at the given directory
    void open(const std::string& db_path);

    // Record a visit to a URL
    void record_visit(const std::string& url, const std::string& title);

    // Get the most recent N history entries (most recent first)
    std::vector<HistoryEntry> get_recent(size_t limit = 100) const;

    // Search history by URL substring (case-insensitive)
    std::vector<HistoryEntry> search_by_url(const std::string& query, size_t limit = 100) const;

    // Search history by title substring (case-insensitive)
    std::vector<HistoryEntry> search_by_title(const std::string& query, size_t limit = 100) const;

    // Get the visit count for a specific URL
    int get_visit_count(const std::string& url) const;

    // Delete a specific history entry by URL
    void delete_entry(const std::string& url);

    // Clear all history
    void clear();

    // Get total number of history entries
    size_t size() const;

    // Trim old entries to keep within a limit
    void trim(size_t max_entries);

private:
    KeyValueStore store_;

    // Maximum entries to keep (auto-trim when exceeded)
    static constexpr size_t MAX_ENTRIES = 10000;

    // Simple hash function for URL (for unique key generation)
    static std::string hash_url(const std::string& url);
};

} // namespace chinstrap