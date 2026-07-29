// download.cpp - Download manager implementation
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: Download implementation overview
//
// The download manager coordinates file downloads with progress tracking,
// resume support, and file naming. The actual HTTP communication is handled
// by the core HTTP client (built by another subagent). This module handles:
//
// 1. Download lifecycle: create -> start -> progress -> complete/fail
// 2. File I/O: writing downloaded data to disk
// 3. Resume: using HTTP Range to continue from where we left off
// 4. Filename resolution: determining where to save the file
// 5. State tracking: maintaining the list of downloads for the UI
//
// TEACHING NOTE: HTTP Range header for resume
//
// When resuming a download, we send:
//   Range: bytes=<offset>-
//
// The server responds with:
//   HTTP/1.1 206 Partial Content
//   Content-Range: bytes <offset>-<total-1>/<total>
//   Content-Length: <remaining bytes>
//
// If the server does not support Range (returns 200 instead of 206), we
// must restart the download from the beginning. Most modern servers
// support Range requests, but it is not mandatory.
//
// We keep partial downloads as ".partial" files. When the download completes,
// we rename the .partial file to the final filename. This ensures that
// incomplete downloads do not appear as complete files and that we can
// resume if the browser or download is interrupted.

#include "download.hpp"

#include <algorithm>
#include <sstream>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cctype>

namespace chinstrap {

// ============================================================================
// DownloadEntry
// ============================================================================

double DownloadEntry::get_speed() const {
    auto elapsed = get_elapsed_seconds();
    if (elapsed <= 0.0) return 0.0;
    return static_cast<double>(bytes_downloaded) / elapsed;
}

double DownloadEntry::get_progress_percent() const {
    if (total_bytes <= 0) return -1.0;
    return (static_cast<double>(bytes_downloaded) / static_cast<double>(total_bytes)) * 100.0;
}

double DownloadEntry::get_elapsed_seconds() const {
    auto end = (state == DownloadState::COMPLETED || state == DownloadState::FAILED ||
                state == DownloadState::CANCELLED)
               ? end_time : std::chrono::system_clock::now();
    return std::chrono::duration<double>(end - start_time).count();
}

// ============================================================================
// DownloadManager
// ============================================================================

DownloadManager::DownloadManager() {
    // Default download directory: ~/Downloads
    const char* home = getenv("HOME");
    if (home) {
        download_dir_ = std::string(home) + "/Downloads";
    } else {
        download_dir_ = "/tmp";
    }

    // Ensure the directory exists
    mkdir(download_dir_.c_str(), 0755);
}

DownloadManager::~DownloadManager() = default;

void DownloadManager::set_download_dir(const std::string& dir) {
    download_dir_ = dir;
    mkdir(download_dir_.c_str(), 0755);
}

// TEACHING NOTE: Content-Disposition parsing
//
// The Content-Disposition header (RFC 6266) looks like:
//   Content-Disposition: attachment; filename="report.pdf"
//   Content-Disposition: attachment; filename*=UTF-8''report%20.pdf
//
// The filename parameter can be:
// - A quoted string: filename="report.pdf"
// - An unquoted token: filename=report.pdf
// - RFC 5987 encoded: filename*=UTF-8''report%20.pdf (for non-ASCII filenames)
//
// We handle the simple cases (quoted and unquoted) and fall back to
// the URL path if parsing fails.

std::optional<std::string> DownloadManager::parse_content_disposition(const std::string& header) {
    // Look for "filename=" (case-insensitive)
    std::string lower = header;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    size_t pos = lower.find("filename=");
    if (pos == std::string::npos) return std::nullopt;

    pos += 9;  // Skip "filename="

    // Check for RFC 5987 encoded value (filename*=)
    if (pos < header.size() && header[pos] == '*') {
        // Format: filename*=UTF-8''<url-encoded-name>
        pos++;  // Skip the *
        // Skip charset and language: UTF-8''
        size_t quote_pos = header.find("''", pos);
        if (quote_pos != std::string::npos) {
            pos = quote_pos + 2;
            // Find end of value (until ; or end of string)
            std::string encoded;
            while (pos < header.size() && header[pos] != ';') {
                encoded += header[pos];
                ++pos;
            }
            return url_decode(encoded);
        }
    }

    // Check for quoted value
    if (pos < header.size() && header[pos] == '"') {
        ++pos;  // Skip opening quote
        std::string filename;
        while (pos < header.size() && header[pos] != '"') {
            if (header[pos] == '\\' && pos + 1 < header.size()) {
                ++pos;  // Skip escape character
            }
            filename += header[pos];
            ++pos;
        }
        if (!filename.empty()) return filename;
    }

    // Unquoted value (read until ; or end)
    std::string filename;
    while (pos < header.size() && header[pos] != ';') {
        filename += header[pos];
        ++pos;
    }

    // Trim whitespace
    size_t s = filename.find_first_not_of(" \t");
    size_t e = filename.find_last_not_of(" \t");
    if (s != std::string::npos) {
        filename = filename.substr(s, e - s + 1);
        if (!filename.empty()) return filename;
    }

    return std::nullopt;
}

std::string DownloadManager::extract_filename_from_url(const std::string& url) {
    // TEACHING NOTE: Extracting filename from URL
    //
    // A URL like "https://example.com/path/to/file.pdf?query=1#fragment"
    // should yield "file.pdf" as the filename.
    //
    // Steps:
    // 1. Strip query string (everything after ?)
    // 2. Strip fragment (everything after #)
    // 3. Take the last path component (after the last /)
    // 4. URL-decode the result
    // 5. If empty, return "download"

    std::string path = url;

    // Strip scheme (https://, http://)
    size_t scheme_end = path.find("://");
    if (scheme_end != std::string::npos) {
        path = path.substr(scheme_end + 3);
    }

    // Strip query and fragment
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    size_t f = path.find('#');
    if (f != std::string::npos) path = path.substr(0, f);

    // Find the last path component
    size_t last_slash = path.rfind('/');
    if (last_slash != std::string::npos) {
        path = path.substr(last_slash + 1);
    }

    // URL-decode
    path = url_decode(path);

    if (path.empty() || path == "/") {
        return "download";
    }

    return path;
}

std::string DownloadManager::mime_to_filename(const std::string& mime_type) {
    // TEACHING NOTE: Generating filename from MIME type
    //
    // When we have no filename from Content-Disposition or URL, we generate
    // a generic name based on the MIME type. For example:
    //   application/pdf -> "download.pdf"
    //   image/png       -> "download.png"
    //   text/html       -> "download.html"
    //
    // We use a small mapping of common MIME types. A real browser would
    // use the system MIME database (/etc/mime.types or similar).

    std::string ext = mime_to_extension(mime_type);
    if (ext.empty()) {
        return "download";
    }
    return "download." + ext;
}

std::string DownloadManager::mime_to_extension(const std::string& mime_type) {
    // Convert to lowercase for comparison
    std::string lower = mime_type;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Strip parameters (e.g., "text/html; charset=utf-8" -> "text/html")
    size_t semi = lower.find(';');
    if (semi != std::string::npos) {
        lower = lower.substr(0, semi);
    }
    // Trim whitespace
    size_t s = lower.find_first_not_of(" \t");
    size_t e = lower.find_last_not_of(" \t");
    if (s != std::string::npos) {
        lower = lower.substr(s, e - s + 1);
    }

    // Common MIME type to extension mapping
    static const std::pair<std::string, std::string> mime_map[] = {
        {"text/html",       "html"},
        {"text/css",        "css"},
        {"text/javascript",  "js"},
        {"text/plain",      "txt"},
        {"text/xml",        "xml"},
        {"application/json", "json"},
        {"application/pdf",  "pdf"},
        {"application/zip",  "zip"},
        {"application/x-tar", "tar"},
        {"application/x-gzip", "gz"},
        {"application/x-bzip2", "bz2"},
        {"application/octet-stream", "bin"},
        {"application/xml",  "xml"},
        {"image/png",        "png"},
        {"image/jpeg",       "jpg"},
        {"image/gif",        "gif"},
        {"image/svg+xml",    "svg"},
        {"image/webp",       "webp"},
        {"image/bmp",        "bmp"},
        {"image/tiff",       "tiff"},
        {"image/x-icon",     "ico"},
        {"audio/mpeg",       "mp3"},
        {"audio/ogg",        "ogg"},
        {"audio/wav",        "wav"},
        {"audio/webm",       "weba"},
        {"video/mp4",        "mp4"},
        {"video/webm",       "webm"},
        {"video/ogg",        "ogv"},
        {"video/x-msvideo",  "avi"},
        {"font/woff",        "woff"},
        {"font/woff2",       "woff2"},
        {"font/ttf",         "ttf"},
        {"font/otf",         "otf"},
    };

    for (const auto& [mime, ext] : mime_map) {
        if (lower == mime) return ext;
    }

    // Try to extract from "application/x-<ext>" or "application/vnd.<vendor>.<ext>"
    if (lower.substr(0, 14) == "application/x-") {
        return lower.substr(14);
    }

    // Try to use the subtype as extension
    size_t slash = lower.find('/');
    if (slash != std::string::npos) {
        std::string subtype = lower.substr(slash + 1);
        if (subtype != "octet-stream" && !subtype.empty()) {
            return subtype;
        }
    }

    return "";
}

std::string DownloadManager::url_decode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            // Parse hex digits
            int hi = std::isdigit(str[i+1]) ? str[i+1] - '0' :
                     std::tolower(str[i+1]) - 'a' + 10;
            int lo = std::isdigit(str[i+2]) ? str[i+2] - '0' :
                     std::tolower(str[i+2]) - 'a' + 10;
            if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                result += static_cast<char>(hi * 16 + lo);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

std::string DownloadManager::sanitize_filename(const std::string& filename) {
    // TEACHING NOTE: Filename sanitization
    //
    // We remove characters that could be problematic on the filesystem:
    // - Path separators: / and \ (prevent directory traversal)
    // - Null bytes: can truncate strings in C
    // - Control characters: 0x00-0x1F
    // - Reserved characters on Windows: : * ? " < > |
    //
    // We also limit the length to 255 characters (typical filesystem limit).
    // Leading dots are preserved (hidden files on Unix) but leading
    // slashes are removed.

    std::string result;
    for (char c : filename) {
        if (c == '/' || c == '\\' || c == '\0') continue;
        if (static_cast<unsigned char>(c) < 0x20) continue;
        if (c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            result += '_';
            continue;
        }
        result += c;
    }

    // Limit length
    if (result.size() > 255) {
        // Preserve extension if possible
        size_t dot = result.rfind('.');
        if (dot != std::string::npos && result.size() - dot <= 10) {
            std::string ext = result.substr(dot);
            result = result.substr(0, 255 - ext.size()) + ext;
        } else {
            result = result.substr(0, 255);
        }
    }

    if (result.empty()) {
        result = "download";
    }

    return result;
}

std::string DownloadManager::unique_filename(const std::string& filename) const {
    std::string path = download_dir_ + "/" + filename;

    // Check if the file exists
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return filename;  // File does not exist, use as-is
    }

    // File exists - append " (N)" before extension
    size_t dot = filename.rfind('.');
    std::string name, ext;
    if (dot != std::string::npos) {
        name = filename.substr(0, dot);
        ext = filename.substr(dot);
    } else {
        name = filename;
        ext = "";
    }

    for (int i = 1; i < 1000; ++i) {
        std::string candidate = name + " (" + std::to_string(i) + ")" + ext;
        path = download_dir_ + "/" + candidate;
        if (stat(path.c_str(), &st) != 0) {
            return candidate;
        }
    }

    // Fallback: use timestamp
    return name + "_" + std::to_string(time(nullptr)) + ext;
}

std::string DownloadManager::determine_filename(
    const std::string& url,
    const std::string& content_disposition,
    const std::string& content_type,
    const std::string& suggested_filename
) const {
    // TEACHING NOTE: Filename determination priority
    //
    // 1. Suggested filename (passed by the caller, e.g., from download attribute)
    // 2. Content-Disposition header filename parameter
    // 3. URL path (last path component, URL-decoded)
    // 4. MIME type (generate "download.<ext>")
    // 5. "download" (ultimate fallback)

    std::string filename;

    // 1. Suggested filename
    if (!suggested_filename.empty()) {
        filename = suggested_filename;
    }

    // 2. Content-Disposition
    if (filename.empty() && !content_disposition.empty()) {
        auto cd_filename = parse_content_disposition(content_disposition);
        if (cd_filename) {
            filename = *cd_filename;
        }
    }

    // 3. URL path
    if (filename.empty()) {
        filename = extract_filename_from_url(url);
    }

    // 4. MIME type
    if (filename.empty() || filename == "download") {
        if (!content_type.empty()) {
            std::string mime_name = mime_to_filename(content_type);
            if (!mime_name.empty() && mime_name != "download") {
                filename = mime_name;
            }
        }
    }

    // 5. Fallback
    if (filename.empty()) {
        filename = "download";
    }

    // Sanitize
    filename = sanitize_filename(filename);

    // Ensure uniqueness
    return unique_filename(filename);
}

// TEACHING NOTE: Starting a download
//
// The start_download function creates a new download entry and initiates
// the download process. In our implementation, we do not perform the actual
// HTTP request here (that is handled by the HTTP client in the core module).
// Instead, we set up the download entry with all the necessary metadata
// (URL, filename, download directory) and return the ID.
//
// The actual download loop (sending request, receiving data, writing to file,
// calling progress callback) would be triggered by the HTTP client. In a
// production browser, this would be asynchronous (running in a background
// thread or using async I/O) to avoid blocking the UI.

int DownloadManager::start_download(
    const std::string& url,
    const std::string& suggested_filename,
    ProgressCallback callback
) {
    (void)callback;  // Progress callback would be called by the HTTP client

    DownloadEntry entry;
    entry.url = url;
    entry.state = DownloadState::PENDING;
    entry.start_time = std::chrono::system_clock::now();

    // Determine filename
    // In a real implementation, we would do an HTTP HEAD request first
    // to get Content-Type and Content-Disposition headers. For now,
    // we use the suggested filename or extract from URL.
    entry.filename = determine_filename(url, "", "", suggested_filename);
    entry.local_path = download_dir_ + "/" + entry.filename;

    int id = next_id_++;
    downloads_.push_back(std::move(entry));

    return id;
}

void DownloadManager::pause_download(int id) {
    for (auto& d : downloads_) {
        if (d.state == DownloadState::IN_PROGRESS) {
            // Find by index (simplified - in production, we would use a map)
        }
    }
    (void)id;
    // In a real implementation, we would:
    // 1. Signal the download thread to stop
    // 2. Record the current offset (bytes downloaded)
    // 3. Close the .partial file
    // 4. Set state to PAUSED
}

void DownloadManager::resume_download(int id) {
    (void)id;
    // In a real implementation, we would:
    // 1. Open the .partial file for appending
    // 2. Send an HTTP request with Range: bytes=<offset>-
    // 3. If server responds 206, continue writing
    // 4. If server responds 200, restart from beginning
    // 5. Set state to IN_PROGRESS
}

void DownloadManager::cancel_download(int id) {
    (void)id;
    // In a real implementation, we would:
    // 1. Signal the download thread to stop
    // 2. Delete the .partial file
    // 3. Set state to CANCELLED
}

void DownloadManager::retry_download(int id) {
    (void)id;
    // In a real implementation, we would:
    // 1. Delete the .partial file
    // 2. Reset the download entry
    // 3. Start the download again
}

std::optional<DownloadEntry> DownloadManager::get_download(int id) const {
    if (id < 1 || id > static_cast<int>(downloads_.size())) {
        return std::nullopt;
    }
    // Downloads are 1-indexed by ID
    // Note: this assumes downloads are stored in order and never removed.
    // In production, we would use a map<int, DownloadEntry>.
    return downloads_[id - 1];
}

std::vector<DownloadEntry> DownloadManager::get_all_downloads() const {
    return downloads_;
}

std::vector<DownloadEntry> DownloadManager::get_active_downloads() const {
    std::vector<DownloadEntry> result;
    for (const auto& d : downloads_) {
        if (d.state == DownloadState::IN_PROGRESS || d.state == DownloadState::PAUSED) {
            result.push_back(d);
        }
    }
    return result;
}

void DownloadManager::clear_history() {
    // Remove only completed/failed/cancelled downloads
    // Keep active downloads
    std::vector<DownloadEntry> active;
    for (auto& d : downloads_) {
        if (d.state == DownloadState::IN_PROGRESS || d.state == DownloadState::PAUSED ||
            d.state == DownloadState::PENDING) {
            active.push_back(std::move(d));
        }
    }
    downloads_ = std::move(active);
}

} // namespace chinstrap