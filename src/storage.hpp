// storage.hpp - Persistent key-value storage for browser data
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: Why browsers need persistent storage
//
// A browser needs to persist many types of data:
// - Browsing history (visited URLs, titles, timestamps)
// - Bookmarks (saved URLs with titles and folders)
// - Saved tabs (for session restore after crash or restart)
// - Settings and preferences (homepage, search engine, theme, etc.)
// - Cookies (for web sessions)
// - Cache (for performance)
// - Autofill data, passwords (encrypted), extensions, etc.
//
// Chrome uses LevelDB (a fast key-value store created by Google, similar to
// RocksDB) for most of its persistent data. LevelDB is an LSM-tree (Log-Structured
// Merge-tree) based store that provides fast writes and point lookups. It is
// much faster than SQLite for simple key-value workloads.
//
// We implement a simpler approach: a key-value store using a flat file with
// a hash index. This is not as fast as LevelDB for large datasets, but it is
// simple, transparent, and demonstrates the core concepts of storage engines.
//
// TEACHING NOTE: Storage engine concepts
//
// A storage engine has two main components:
//
// 1. The log: An append-only file where new data is written. Writes are
//    fast (just append). The log is the source of truth.
//
// 2. The index: An in-memory structure that maps keys to positions in the
//    log. Lookups go through the index. We use a hash map for O(1) lookups.
//
// When the log gets too large, we compact it by removing old entries
// (keeping only the latest value for each key). This is similar to how
// LSM-tree storage engines work (LevelDB, RocksDB, Cassandra).
//
// B-tree vs Hash Index:
// - Hash index: O(1) lookup, but no range queries. Good for point lookups.
// - B-tree index: O(log n) lookup, but supports range queries. Good for
//   ordered data (like history sorted by time).
//
// We use a hash index for the key-value store (storage) and a sorted
// approach for history (which needs time-ordered queries).

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <optional>

namespace chinstrap {

// TEACHING NOTE: Key-Value store API
//
// The KV store provides a simple put/get/delete interface:
// - put(key, value): Store a value under a key
// - get(key): Retrieve the value for a key (nullopt if not found)
// - del(key): Delete a key and its value
// - keys(prefix): List all keys with a given prefix
//
// The store is organized into "namespaces" (like tables in a database).
// Each namespace is a separate log file. For example:
// - "history" namespace stores browsing history entries
// - "bookmarks" namespace stores bookmarks
// - "settings" namespace stores browser settings
// - "tabs" namespace stores saved tabs
//
// Namespaces allow us to iterate over keys with a common prefix (e.g.,
// all "history:" keys for history queries) without scanning unrelated data.

class KeyValueStore {
public:
    KeyValueStore();
    ~KeyValueStore();

    // Open a storage database at the given directory path
    // Creates the directory if it does not exist
    bool open(const std::string& db_path);

    // Store a key-value pair
    void put(const std::string& key, const std::string& value);

    // Get a value by key
    std::optional<std::string> get(const std::string& key) const;

    // Delete a key
    void del(const std::string& key);

    // Check if a key exists
    bool has(const std::string& key) const;

    // Get all keys with a given prefix
    std::vector<std::string> keys_with_prefix(const std::string& prefix) const;

    // Get all key-value pairs with a given prefix
    std::vector<std::pair<std::string, std::string>> get_with_prefix(const std::string& prefix) const;

    // Close the database and flush pending writes
    void close();

    // Get the number of keys
    size_t size() const { return index_.size(); }

    // Compact the storage (remove deleted and overwritten entries)
    void compact();

private:
    std::string db_path_;
    std::string data_file_;

    // TEACHING NOTE: In-memory index
    //
    // The index maps keys to their position (byte offset) in the data file.
    // When we do a get(), we look up the key in the index to find the offset,
    // then seek to that position in the file and read the value.
    //
    // The index is loaded into memory on open() and updated on every put/del.
    // This means memory usage grows with the number of keys, but lookups are O(1).

    struct IndexEntry {
        uint64_t offset;       // Position in the data file
        uint32_t value_length; // Length of the value
        bool deleted;          // True if this key was deleted
    };

    std::unordered_map<std::string, IndexEntry> index_;

    // Write a record to the data file
    void write_record(const std::string& key, const std::string& value, bool deleted);

    // Read a record from the data file at the given offset
    // Returns (key, value, deleted) tuple
    struct Record {
        std::string key;
        std::string value;
        bool deleted;
        uint64_t next_offset;  // Offset of the next record
    };
    std::optional<Record> read_record(uint64_t offset) const;

    // Load the index by scanning the entire data file
    void load_index();
};

// TEACHING NOTE: B-tree explanation
//
// A B-tree is a self-balancing tree data structure that keeps data sorted and
// allows searches, sequential access, insertions, and deletions in O(log n)
// time. B-trees are used by most database storage engines (MySQL InnoDB,
// PostgreSQL, SQLite) because they are efficient for both point lookups and
// range queries.
//
// Key properties:
// - Each node has multiple keys and multiple children (unlike binary trees)
// - The tree is balanced: all leaf nodes are at the same depth
// - Each node is between half-full and completely full
// - Typical fan-out (children per node): 100-1000 (so depth is 3-4 for millions of keys)
//
// We do not implement a full B-tree in this file. Our hash-indexed KV store
// is simpler and sufficient for browser data. However, for range queries
// (like "get history between date A and date B"), a B-tree would be better.
// Our history module uses a sorted approach for this purpose.
//
// Chrome uses LevelDB, which uses an LSM-tree (not a B-tree). LSM-trees are
// optimized for write-heavy workloads: all writes go to an in-memory table
// (memtable), which is periodically flushed to disk as an immutable sorted
// file (SSTable). Multiple SSTables are merged in the background (compaction).
// This provides excellent write performance but slightly slower reads
// (need to check multiple SSTables). Bloom filters are used to speed up reads.

} // namespace chinstrap