#ifndef UPDATECHECKER_HPP
#define UPDATECHECKER_HPP

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

enum class UpdateState { IDLE, CHECKING, UP_TO_DATE, UPDATE_AVAILABLE, CHECK_FAILED };

class UpdateChecker {
public:
    UpdateChecker() = default;
    ~UpdateChecker();

    // Starts a background thread that waits initialDelaySec before hitting the API
    void checkAsync(int initialDelaySec = 5);

    UpdateState getState() const;
    std::string getLatestVersion() const;
    std::string getReleaseUrl() const;
    std::string getReleaseNotes() const;

    // Direct download URL for the current platform's asset (empty if none found)
    std::string getDownloadUrl() const;

private:
    void runCheck(int delaySec);

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
    std::thread                 m_thread;
};

#endif // UPDATECHECKER_HPP
