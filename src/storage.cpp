// storage.cpp - Persistent key-value storage implementation
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: Storage implementation details
//
// Our key-value store uses an append-only log file with an in-memory hash index.
// This design is inspired by Bitcask (a Riak key-value store) and is one of the
// simplest possible storage engines that still provides good performance.
//
// Record format on disk:
//   [4 bytes key length] [key bytes]
//   [4 bytes value length] [value bytes]
//   [1 byte deleted flag]
//
// The total record size is: 4 + key_len + 4 + value_len + 1
//
// The index maps each key to the byte offset of the latest record for that key.
// When we put(key, value), we append a new record and update the index.
// When we get(key), we look up the offset, seek to it, and read the value.
// When we del(key), we append a record with deleted=true and update the index.
//
// Compaction: When the data file grows too large (due to overwritten and
// deleted keys), we compact by creating a new file with only the latest
// values for each key. This is similar to LSM-tree compaction.

#include "storage.hpp"

#include <fstream>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

namespace chinstrap {

namespace {

// Write a 32-bit big-endian value
void write_u32_be(std::ofstream& f, uint32_t val) {
    f.put(static_cast<char>((val >> 24) & 0xFF));
    f.put(static_cast<char>((val >> 16) & 0xFF));
    f.put(static_cast<char>((val >> 8) & 0xFF));
    f.put(static_cast<char>(val & 0xFF));
}

// Read a 32-bit big-endian value
uint32_t read_u32_be(std::ifstream& f) {
    unsigned char b[4];
    f.read(reinterpret_cast<char*>(b), 4);
    return (static_cast<uint32_t>(b[0]) << 24) |
           (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) |
           static_cast<uint32_t>(b[3]);
}

// Get current file size
uint64_t file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return static_cast<uint64_t>(st.st_size);
    }
    return 0;
}

} // anonymous namespace

KeyValueStore::KeyValueStore() = default;
KeyValueStore::~KeyValueStore() {
    close();
}

bool KeyValueStore::open(const std::string& db_path) {
    db_path_ = db_path;
    data_file_ = db_path + "/data.log";

    // Create directory if it does not exist
    mkdir(db_path_.c_str(), 0755);

    // Load the index from the existing data file
    load_index();

    return true;
}

void KeyValueStore::load_index() {
    index_.clear();

    std::ifstream file(data_file_, std::ios::binary);
    if (!file.is_open()) {
        return;
    }

    uint64_t offset = 0;
    while (file && !file.eof()) {
        // Read key length
        file.peek();
        if (file.eof()) break;

        uint32_t key_len = read_u32_be(file);
        if (file.gcount() < 4) break;

        if (key_len > 1024 * 1024) {
            // Key too large - corrupted file
            break;
        }

        // Read key
        std::string key(key_len, '\0');
        file.read(&key[0], key_len);
        if (static_cast<uint32_t>(file.gcount()) < key_len) break;

        // Read value length
        uint32_t val_len = read_u32_be(file);

        // Read value
        std::string value;
        if (val_len > 0 && val_len < 16 * 1024 * 1024) {
            value.resize(val_len);
            file.read(&value[0], val_len);
        }

        // Read deleted flag
        char deleted = 0;
        file.read(&deleted, 1);

        // Calculate the next record offset
        uint64_t record_size = 4 + key_len + 4 + val_len + 1;
        uint64_t next_offset = offset + record_size;

        // Update index (latest record wins)
        IndexEntry entry;
        entry.offset = offset;
        entry.value_length = val_len;
        entry.deleted = (deleted != 0);
        index_[key] = entry;

        offset = next_offset;
    }
}

void KeyValueStore::write_record(const std::string& key, const std::string& value, bool deleted) {
    std::ofstream file(data_file_, std::ios::binary | std::ios::app);
    if (!file.is_open()) return;

    // Record: [4 key_len] [key] [4 val_len] [val] [1 deleted]
    write_u32_be(file, static_cast<uint32_t>(key.size()));
    file.write(key.data(), static_cast<std::streamsize>(key.size()));
    write_u32_be(file, static_cast<uint32_t>(value.size()));
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    file.put(deleted ? 1 : 0);
    file.flush();

    // Update index
    uint64_t offset = file_size(data_file_) - (4 + key.size() + 4 + value.size() + 1);
    IndexEntry entry;
    entry.offset = offset;
    entry.value_length = static_cast<uint32_t>(value.size());
    entry.deleted = deleted;
    index_[key] = entry;
}

void KeyValueStore::put(const std::string& key, const std::string& value) {
    write_record(key, value, false);
}

std::optional<std::string> KeyValueStore::get(const std::string& key) const {
    auto it = index_.find(key);
    if (it == index_.end() || it->second.deleted) {
        return std::nullopt;
    }

    // Read the value from the data file
    std::ifstream file(data_file_, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    // Seek to the record position
    // Record: [4 key_len] [key] [4 val_len] [val] [1 deleted]
    file.seekg(static_cast<std::streamoff>(it->second.offset));
    uint32_t key_len = read_u32_be(file);
    file.seekg(key_len, std::ios::cur);  // Skip key
    uint32_t val_len = read_u32_be(file);

    if (val_len != it->second.value_length) {
        return std::nullopt;  // Corruption detected
    }

    std::string value(val_len, '\0');
    file.read(&value[0], val_len);

    return value;
}

void KeyValueStore::del(const std::string& key) {
    write_record(key, "", true);
}

bool KeyValueStore::has(const std::string& key) const {
    auto it = index_.find(key);
    return it != index_.end() && !it->second.deleted;
}

std::vector<std::string> KeyValueStore::keys_with_prefix(const std::string& prefix) const {
    std::vector<std::string> result;
    for (const auto& [key, entry] : index_) {
        if (!entry.deleted && key.compare(0, prefix.size(), prefix) == 0) {
            result.push_back(key);
        }
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> KeyValueStore::get_with_prefix(const std::string& prefix) const {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& [key, entry] : index_) {
        if (!entry.deleted && key.compare(0, prefix.size(), prefix) == 0) {
            auto value = get(key);
            if (value) {
                result.emplace_back(key, std::move(*value));
            }
        }
    }
    return result;
}

void KeyValueStore::close() {
    // Nothing to do - we write through to the file on every operation
}

// TEACHING NOTE: Compaction
//
// Over time, the data file accumulates stale entries (overwritten or deleted
// keys). Compaction rewrites the file with only the latest values, reclaiming
// space. This is similar to:
//
// - LSM-tree compaction: merges multiple SSTables into one, removing
//   obsolete entries. LevelDB uses a tiered compaction strategy.
// - VACUUM in SQLite: rebuilds the database file to defragment it.
// - GC (garbage collection) in languages like Java/Go: removes unreachable
//   objects to reclaim memory.
//
// Our compaction is simple:
// 1. Create a temporary file
// 2. For each key in the index (that is not deleted), write the latest value
// 3. Replace the original file with the temporary file
// 4. Rebuild the index
//
// Chrome does not use compaction for its LevelDB stores (LevelDB handles this
// internally), but it does periodically clean up old data (e.g., old history
// entries, expired cookies).

void KeyValueStore::compact() {
    std::string temp_file = data_file_ + ".tmp";
    std::ofstream out(temp_file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;

    // Write all non-deleted entries to the new file
    std::unordered_map<std::string, IndexEntry> new_index;
    uint64_t offset = 0;

    for (const auto& [key, entry] : index_) {
        if (entry.deleted) continue;

        auto value = get(key);
        if (!value) continue;

        write_u32_be(out, static_cast<uint32_t>(key.size()));
        out.write(key.data(), static_cast<std::streamsize>(key.size()));
        write_u32_be(out, static_cast<uint32_t>(value->size()));
        out.write(value->data(), static_cast<std::streamsize>(value->size()));
        out.put(0);  // Not deleted

        IndexEntry new_entry;
        new_entry.offset = offset;
        new_entry.value_length = static_cast<uint32_t>(value->size());
        new_entry.deleted = false;
        new_index[key] = new_entry;

        offset += 4 + key.size() + 4 + value->size() + 1;
    }

    out.flush();
    out.close();

    // Replace the original file
    rename(temp_file.c_str(), data_file_.c_str());

    // Update the index
    index_ = std::move(new_index);
}

} // namespace chinstrap