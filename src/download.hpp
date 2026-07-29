// download.hpp - Download manager
// Part of Chinstrap - a from-scratch web browser in C++17 with zero third-party libraries
//
// TEACHING NOTE: Download management in browsers
//
// Downloading files is a core browser feature. When a user clicks a link
// that points to a non-HTML resource (like a PDF, image, or ZIP file), or
// when the server sends a Content-Disposition: attachment header, the
// browser starts a download instead of rendering the content.
//
// A download manager handles:
//
// 1. Starting downloads: Parse the URL, open a connection, receive the response
// 2. Progress tracking: Report bytes downloaded vs total bytes (from Content-Length)
// 3. Resume support: If the connection drops, resume from where we left off
//    using the HTTP Range header
// 4. File naming: Determine the filename from Content-Disposition, URL path,
//    or Content-Type
// 5. MIME type detection: Determine the file type from Content-Type or file extension
// 6. Download history: Keep track of past downloads for the downloads page
// 7. Speed limiting: Optional bandwidth limiting (Chrome does not support this
//    natively but extensions can)
//
// TEACHING NOTE: MIME types and file handling
//
// MIME (Multipurpose Internet Mail Extensions) types describe the type of
// data in a file. Common examples:
//   text/html       -> HTML document (rendered by browser)
//   application/pdf  -> PDF document (may open in PDF viewer or download)
//   image/png       -> PNG image (rendered by browser)
//   application/zip -> ZIP archive (always download)
//
// The Content-Type header tells the browser the MIME type. The browser uses
// this to decide whether to render or download. Some MIME types are always
// downloaded (application/octet-stream, application/zip), while others may
// be rendered (text/html, image/png).
//
// Content-Disposition header:
//   Content-Disposition: attachment; filename="report.pdf"
// This header tells the browser to download the file (instead of rendering)
// and suggests a filename. The browser uses this as the primary source for
// the filename, falling back to the URL path if not present.
//
// Filename determination priority:
// 1. Content-Disposition filename parameter
// 2. URL path (last path component)
// 3. Content-Type (generate a generic name like "download.pdf")
// 4. "download" (fallback)

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <functional>
#include <cstdint>

namespace chinstrap {

// Download state
enum class DownloadState {
    PENDING,      // Download created but not started
    IN_PROGRESS,   // Download is actively downloading
    PAUSED,       // Download is paused (can be resumed)
    COMPLETED,    // Download finished successfully
    FAILED,       // Download failed (network error, disk full, etc.)
    CANCELLED,    // User cancelled the download
};

// TEACHING NOTE: Download progress tracking
//
// The progress callback is called periodically during the download with:
// - bytes_downloaded: total bytes received so far
// - total_bytes: total expected bytes (from Content-Length, or -1 if unknown)
// - speed: current download speed in bytes/second
//
// The UI uses this to display a progress bar, percentage, estimated time
// remaining, and download speed. Chrome shows this in the download shelf
// (bottom of the window) or in the downloads page (chrome://downloads).

// Download progress callback
using ProgressCallback = std::function<void(size_t bytes_downloaded,
                                            int64_t total_bytes,
                                            double speed_bps)>;

// A download entry tracking a single file download
struct DownloadEntry {
    std::string url;                      // Download URL
    std::string local_path;               // Where the file is saved on disk
    std::string filename;                 // Just the filename (no path)
    std::string mime_type;                // MIME type from Content-Type
    std::string content_disposition;      // Content-Disposition header value

    DownloadState state = DownloadState::PENDING;
    size_t bytes_downloaded = 0;          // Bytes received so far
    int64_t total_bytes = -1;             // Total expected bytes (-1 if unknown)
    size_t resume_offset = 0;             // Offset to resume from (for Range header)

    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    std::string error_message;            // Error description if failed

    // Calculate download speed in bytes per second
    double get_speed() const;

    // Calculate progress percentage (0-100), or -1 if total is unknown
    double get_progress_percent() const;

    // Get elapsed time in seconds
    double get_elapsed_seconds() const;
};

// TEACHING NOTE: Download manager design
//
// The download manager maintains a list of active and completed downloads.
// It coordinates with the HTTP client to perform the actual download and
// tracks progress. Key design decisions:
//
// - Each download has a unique ID (for UI reference)
// - Downloads can be paused and resumed (using HTTP Range)
// - Partial downloads are kept as .partial files until complete
// - Completed downloads are renamed from .partial to the final filename
// - Download history is persisted (so it survives browser restarts)
//
// Chrome's download manager:
// - Stores downloads in ~/Downloads by default (configurable)
// - Keeps a history of recent downloads (shown on chrome://downloads)
// - Supports parallel downloads (multiple at once)
// - Can open files with the default system application after download
// - Shows download notifications

class DownloadManager {
public:
    DownloadManager();
    ~DownloadManager();

    // Set the download directory (default: ~/Downloads)
    void set_download_dir(const std::string& dir);

    // TEACHING NOTE: Starting a download
    //
    // The start_download function:
    // 1. Parses the URL to extract a suggested filename
    // 2. Checks if the file already exists and appends a number if so
    //    (e.g., "file.pdf" -> "file (1).pdf")
    // 3. Creates a .partial file for writing
    // 4. Sends an HTTP request (with Range header if resuming)
    // 5. Writes received data to the .partial file
    // 6. Calls the progress callback periodically
    // 7. On completion, renames .partial to the final filename

    // Start a new download. Returns the download ID.
    int start_download(const std::string& url,
                       const std::string& suggested_filename = "",
                       ProgressCallback callback = nullptr);

    // Pause a download
    void pause_download(int id);

    // Resume a paused download
    void resume_download(int id);

    // Cancel a download
    void cancel_download(int id);

    // Retry a failed download
    void retry_download(int id);

    // Get a download entry by ID
    std::optional<DownloadEntry> get_download(int id) const;

    // Get all downloads (active and completed)
    std::vector<DownloadEntry> get_all_downloads() const;

    // Get only active (in-progress or paused) downloads
    std::vector<DownloadEntry> get_active_downloads() const;

    // Clear download history (does not delete files)
    void clear_history();

private:
    std::string download_dir_;
    std::vector<DownloadEntry> downloads_;
    int next_id_ = 1;

    // TEACHING NOTE: Filename generation
    //
    // When a download starts, we need to determine the filename:
    // 1. If suggested_filename is provided, use it
    // 2. Parse Content-Disposition header for filename parameter
    // 3. Extract from URL path (last path component, URL-decoded)
    // 4. If URL has no path, use "index.html" or "download"
    //
    // We also handle filename collisions: if the file already exists,
    // we append " (N)" before the extension: "file.pdf" -> "file (1).pdf"
    //
    // For security, we sanitize the filename:
    // - Remove directory separators (prevent path traversal)
    // - Remove special characters that are problematic on the filesystem
    // - Limit length to 255 characters (typical filesystem limit)

    // Determine the filename from URL, Content-Disposition, and Content-Type
    std::string determine_filename(
        const std::string& url,
        const std::string& content_disposition,
        const std::string& content_type,
        const std::string& suggested_filename
    ) const;

    // Parse filename from Content-Disposition header
    // Example: "attachment; filename=\"report.pdf\"" -> "report.pdf"
    static std::optional<std::string> parse_content_disposition(const std::string& header);

    // Extract filename from URL path
    static std::string extract_filename_from_url(const std::string& url);

    // Generate a filename from MIME type
    static std::string mime_to_filename(const std::string& mime_type);

    // Sanitize a filename (remove dangerous characters)
    static std::string sanitize_filename(const std::string& filename);

    // Generate a unique filename in the download directory
    std::string unique_filename(const std::string& filename) const;

    // TEACHING NOTE: MIME type to extension mapping
    //
    // When the server does not provide a filename and we only have the
    // Content-Type, we generate a filename with the appropriate extension.
    // We maintain a small mapping of common MIME types to extensions.
    // A full browser would use a more comprehensive mapping (like the
    // system MIME database or an embedded list).

    // Get file extension for a MIME type
    static std::string mime_to_extension(const std::string& mime_type);

    // URL-decode a string (convert %20 to space, etc.)
    static std::string url_decode(const std::string& str);
};

} // namespace chinstrap