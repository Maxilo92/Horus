#ifndef UPDATECHECKER_HPP
#define UPDATECHECKER_HPP

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

enum class UpdateState { IDLE, CHECKING, UP_TO_DATE, UPDATE_AVAILABLE, CHECK_FAILED };

enum class DownloadState { IDLE, DOWNLOADING, COMPLETED, FAILED };

class UpdateChecker {
public:
    UpdateChecker() = default;
    ~UpdateChecker();

    // Starts a background thread that waits initialDelaySec before hitting the API
    void checkAsync(int initialDelaySec = 5);

    // Starts a background thread to download a file from a URL to a destination path
    void downloadFileAsync(const std::string& url, const std::string& destinationPath);

    UpdateState getState() const;
    std::string getLatestVersion() const;
    std::string getReleaseUrl() const;
    std::string getReleaseNotes() const;

    // Direct download URL for the current platform's asset (empty if none found)
    std::string getDownloadUrl() const;

    // ── Download Status ──────────────────────────────────────────────────────
    DownloadState getDownloadState() const;
    float         getDownloadProgress() const; // 0.0 to 1.0
    std::string   getDownloadStatus() const;   // Human-readable status string
    void          resetDownload();

private:
    void runCheck(int delaySec);
    void runDownload(std::string url, std::string dest);

    // Minimal JSON string-field extractor (no deps)
    static std::string parseJsonString(const std::string& json, const std::string& key);

    // Returns >0 if versionA is newer than versionB (strips leading 'v')
    static int compareVersions(const std::string& a, const std::string& b);

    // Parses the first asset URL from the "assets" JSON array whose name
    // matches the given suffix (case-insensitive).
    static std::string parseAssetUrl(const std::string& json, const std::string& suffix);

    mutable std::mutex          m_mutex;
    std::atomic<UpdateState>    m_state{UpdateState::IDLE};
    std::string                 m_latestVersion;
    std::string                 m_releaseUrl;
    std::string                 m_releaseNotes;
    std::string                 m_downloadUrl;
    
    // Download-specific state
    std::atomic<DownloadState>  m_downloadState{DownloadState::IDLE};
    std::atomic<double>         m_downloadProgress{0.0};
    std::string                 m_downloadStatusMsg;
    uint64_t                    m_downloadedBytes{0};
    uint64_t                    m_totalBytes{0};

    std::thread                 m_thread;
    std::thread                 m_downloadThread;
};

#endif // UPDATECHECKER_HPP
