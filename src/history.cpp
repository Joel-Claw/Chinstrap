// history.cpp - Browsing history implementation
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: History storage design
//
// We store history entries in our key-value store with two types of keys:
//
// 1. Visit entries: "h:<timestamp>:<url_hash>" -> "url|title|visit_count|last_visit_time"
//    These are sorted by timestamp (since keys are lexicographically sorted).
//    To get recent history, we scan keys starting with "h:" in reverse order.
//
// 2. URL index: "u:<url>" -> "timestamp:visit_count:last_visit_time"
//    This allows us to quickly check if a URL has been visited before and
//    update the visit count. Without this, we would need to scan all entries
//    to find duplicates.
//
// The visit entry key format "h:<timestamp>:<url_hash>" ensures that:
// - All history entries are grouped under the "h:" prefix
// - Entries are sorted by time (since timestamps are zero-padded)
// - No two entries with the same timestamp collide (url_hash provides uniqueness)
//
// We use a 20-character hex hash of the URL for uniqueness. This is not
// cryptographically secure (it is just FNV-1a), but it is sufficient for
// key uniqueness in a single-user browser.

#include "history.hpp"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cctype>

namespace chinstrap {

// ============================================================================
// HistoryEntry serialization
// ============================================================================
//
// TEACHING NOTE: Serialization format
//
// We store history entries as pipe-separated values:
//   url|title|visit_count|last_visit_time
//
// This is simpler than JSON or XML and avoids third-party parsers.
// We escape pipe characters in URLs and titles as "\p" to avoid ambiguity.
// This is a simple but robust escaping scheme.

std::string HistoryEntry::serialize() const {
    // Escape pipe characters
    auto escape = [](const std::string& s) {
        std::string result;
        for (char c : s) {
            if (c == '|') { result += "\\p"; }
            else if (c == '\\') { result += "\\\\"; }
            else { result += c; }
        }
        return result;
    };

    std::ostringstream oss;
    oss << escape(url) << '|'
        << escape(title) << '|'
        << visit_count << '|'
        << last_visit_time;
    return oss.str();
}

std::optional<HistoryEntry> HistoryEntry::deserialize(const std::string& data) {
    // Unescape pipe characters
    auto unescape = [](const std::string& s) {
        std::string result;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                if (s[i+1] == 'p') { result += '|'; ++i; }
                else if (s[i+1] == '\\') { result += '\\'; ++i; }
                else { result += s[i]; }
            } else {
                result += s[i];
            }
        }
        return result;
    };

    // Split by unescaped pipes
    std::vector<std::string> fields;
    std::string current;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '|' && (i == 0 || data[i-1] != '\\')) {
            fields.push_back(current);
            current.clear();
        } else {
            current += data[i];
        }
    }
    fields.push_back(current);

    if (fields.size() < 4) return std::nullopt;

    HistoryEntry entry;
    entry.url = unescape(fields[0]);
    entry.title = unescape(fields[1]);
    try {
        entry.visit_count = std::stoi(fields[2]);
        entry.last_visit_time = std::stoll(fields[3]);
    } catch (...) {
        return std::nullopt;
    }

    return entry;
}

// ============================================================================
// HistoryManager
// ============================================================================

HistoryManager::HistoryManager() = default;
HistoryManager::~HistoryManager() = default;

void HistoryManager::open(const std::string& db_path) {
    store_.open(db_path);
}

std::string HistoryManager::hash_url(const std::string& url) {
    // FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : url) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }

    // Convert to hex (20 characters for 80 bits)
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

// TEACHING NOTE: Recording a visit
//
// When the user navigates to a URL, we record the visit:
// 1. Check if the URL has been visited before (using the "u:" index)
// 2. If yes, update the visit count and last_visit_time
// 3. If no, create a new entry with visit_count=1
//
// We also create a visit entry under "h:" for the timeline view.
// This allows us to show the full visit history (every time the user
// visited a URL, not just the latest visit).
//
// The visit time is the current Unix timestamp in milliseconds for
// sub-second precision.

void HistoryManager::record_visit(const std::string& url, const std::string& title) {
    auto now = std::chrono::system_clock::now();
    int64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Check if URL was visited before
    std::string url_key = "u:" + url;
    int visit_count = 1;
    int64_t last_visit = timestamp;

    auto existing = store_.get(url_key);
    if (existing) {
        // Parse existing visit data: "timestamp:visit_count:last_visit_time"
        // We only need the visit count
        std::istringstream iss(*existing);
        std::string ts_str, count_str, last_str;
        if (std::getline(iss, ts_str, ':') &&
            std::getline(iss, count_str, ':') &&
            std::getline(iss, last_str, ':')) {
            try {
                visit_count = std::stoi(count_str) + 1;
            } catch (...) {
                visit_count = 1;
            }
        }
    }

    // Update URL index
    std::ostringstream url_val;
    url_val << timestamp << ':' << visit_count << ':' << last_visit;
    store_.put(url_key, url_val.str());

    // Create visit entry
    HistoryEntry entry;
    entry.url = url;
    entry.title = title;
    entry.visit_time = timestamp;
    entry.visit_count = visit_count;
    entry.last_visit_time = last_visit;

    std::string visit_key = "h:" + std::to_string(timestamp) + ":" + hash_url(url);
    store_.put(visit_key, entry.serialize());

    // Auto-trim if we have too many entries
    trim(MAX_ENTRIES);
}

// TEACHING NOTE: Getting recent history
//
// To get the most recent entries, we need entries sorted by time in
// descending order. Our keys are "h:<timestamp>:<hash>" which means they
// are sorted by timestamp in ascending order (oldest first).
//
// Since our key-value store does not support reverse iteration, we:
// 1. Get all keys with the "h:" prefix
// 2. Sort them in descending order (most recent first)
// 3. Read the first N entries
//
// This is O(n log n) but acceptable for typical history sizes (thousands).
// A production browser would use a database with proper indexes.

std::vector<HistoryEntry> HistoryManager::get_recent(size_t limit) const {
    auto keys = store_.keys_with_prefix("h:");

    // Sort keys in descending order (most recent first)
    std::sort(keys.begin(), keys.end(), std::greater<std::string>());

    std::vector<HistoryEntry> result;
    for (const auto& key : keys) {
        if (result.size() >= limit) break;
        auto value = store_.get(key);
        if (value) {
            auto entry = HistoryEntry::deserialize(*value);
            if (entry) {
                result.push_back(std::move(*entry));
            }
        }
    }

    return result;
}

// TEACHING NOTE: History search
//
// Searching history by URL or title requires scanning all entries because
// our key-value store does not support full-text search. For each entry,
// we check if the query string appears as a substring (case-insensitive).
//
// A production browser might use:
// - A full-text search index (like SQLite FTS5) for fast text search
// - An inverted index mapping words to entry IDs
// - A trigram index for substring search
//
// For our purposes, a linear scan is acceptable. Browsers typically limit
// history to a few thousand entries, and the scan is fast enough for
// interactive use.

std::vector<HistoryEntry> HistoryManager::search_by_url(const std::string& query, size_t limit) const {
    auto keys = store_.keys_with_prefix("h:");
    std::sort(keys.begin(), keys.end(), std::greater<std::string>());

    // Convert query to lowercase for case-insensitive matching
    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::vector<HistoryEntry> result;
    for (const auto& key : keys) {
        if (result.size() >= limit) break;
        auto value = store_.get(key);
        if (!value) continue;

        auto entry = HistoryEntry::deserialize(*value);
        if (!entry) continue;

        std::string lower_url = entry->url;
        std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (lower_url.find(lower_query) != std::string::npos) {
            result.push_back(std::move(*entry));
        }
    }

    return result;
}

std::vector<HistoryEntry> HistoryManager::search_by_title(const std::string& query, size_t limit) const {
    auto keys = store_.keys_with_prefix("h:");
    std::sort(keys.begin(), keys.end(), std::greater<std::string>());

    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::vector<HistoryEntry> result;
    for (const auto& key : keys) {
        if (result.size() >= limit) break;
        auto value = store_.get(key);
        if (!value) continue;

        auto entry = HistoryEntry::deserialize(*value);
        if (!entry) continue;

        std::string lower_title = entry->title;
        std::transform(lower_title.begin(), lower_title.end(), lower_title.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (lower_title.find(lower_query) != std::string::npos) {
            result.push_back(std::move(*entry));
        }
    }

    return result;
}

int HistoryManager::get_visit_count(const std::string& url) const {
    std::string url_key = "u:" + url;
    auto value = store_.get(url_key);
    if (!value) return 0;

    // Parse "timestamp:visit_count:last_visit_time"
    std::istringstream iss(*value);
    std::string ts_str, count_str;
    if (std::getline(iss, ts_str, ':') && std::getline(iss, count_str, ':')) {
        try {
            return std::stoi(count_str);
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

void HistoryManager::delete_entry(const std::string& url) {
    // Delete the URL index entry
    store_.del("u:" + url);

    // Delete all visit entries for this URL
    // We need to scan because the visit key includes a timestamp and hash
    auto keys = store_.keys_with_prefix("h:");
    std::string url_hash = hash_url(url);

    for (const auto& key : keys) {
        // Check if this key is for our URL
        auto value = store_.get(key);
        if (!value) continue;

        auto entry = HistoryEntry::deserialize(*value);
        if (entry && entry->url == url) {
            store_.del(key);
        }
    }
}

void HistoryManager::clear() {
    auto keys = store_.keys_with_prefix("h:");
    for (const auto& key : keys) {
        store_.del(key);
    }

    auto url_keys = store_.keys_with_prefix("u:");
    for (const auto& key : url_keys) {
        store_.del(key);
    }
}

size_t HistoryManager::size() const {
    return store_.keys_with_prefix("h:").size();
}

// TEACHING NOTE: History trimming
//
// Browsers limit the amount of stored history to prevent unbounded growth.
// Chrome keeps history for about 3 months by default and removes entries
// older than that. We limit by count (10,000 entries) and remove the oldest
// entries when the limit is exceeded.
//
// Trimming is important for:
// - Memory: the in-memory index grows with the number of entries
// - Disk: the data file grows with every write
// - Performance: queries scan all entries, so fewer entries = faster queries
// - Privacy: old history is less relevant and more of a privacy risk

void HistoryManager::trim(size_t max_entries) {
    auto keys = store_.keys_with_prefix("h:");
    if (keys.size() <= max_entries) return;

    // Sort ascending (oldest first)
    std::sort(keys.begin(), keys.end());

    // Delete the oldest entries
    size_t to_delete = keys.size() - max_entries;
    for (size_t i = 0; i < to_delete; ++i) {
        store_.del(keys[i]);
    }
}

} // namespace chinstrap