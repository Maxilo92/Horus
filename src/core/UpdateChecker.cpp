#include "UpdateChecker.hpp"

#include <curl/curl.h>
#include <chrono>
#include <sstream>
#include <vector>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

static const char* kApiUrl =
    "https://api.github.com/repos/Maxilo92/Horus/releases/latest";

namespace {

size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

UpdateChecker::~UpdateChecker() {
    if (m_thread.joinable())
        m_thread.detach();
}

void UpdateChecker::checkAsync(int initialDelaySec) {
    UpdateState expected = UpdateState::IDLE;
    if (!m_state.compare_exchange_strong(expected, UpdateState::IDLE))
        return; // already running or done

    m_thread = std::thread(&UpdateChecker::runCheck, this, initialDelaySec);
    m_thread.detach();
}

void UpdateChecker::runCheck(int delaySec) {
    if (delaySec > 0)
        std::this_thread::sleep_for(std::chrono::seconds(delaySec));

    m_state = UpdateState::CHECKING;

    CURL* curl = curl_easy_init();
    if (!curl) {
        m_state = UpdateState::CHECK_FAILED;
        return;
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, kApiUrl);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Tactileviewer/" APP_VERSION);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode != 200) {
        m_state = UpdateState::CHECK_FAILED;
        return;
    }

    std::string tagName   = parseJsonString(response, "tag_name");
    std::string htmlUrl   = parseJsonString(response, "html_url");
    std::string body      = parseJsonString(response, "body");

    if (tagName.empty()) {
        m_state = UpdateState::CHECK_FAILED;
        return;
    }

    // Find the platform-specific direct download asset
#if defined(_WIN32)
    std::string downloadUrl = parseAssetUrl(response, ".exe");
    if (downloadUrl.empty()) downloadUrl = parseAssetUrl(response, "-windows.zip");
    if (downloadUrl.empty()) downloadUrl = parseAssetUrl(response, "_win64.zip");
#elif defined(__APPLE__)
    std::string downloadUrl = parseAssetUrl(response, ".dmg");
    if (downloadUrl.empty()) downloadUrl = parseAssetUrl(response, "-macos.zip");
#else
    std::string downloadUrl = parseAssetUrl(response, "-linux");
    if (downloadUrl.empty()) downloadUrl = parseAssetUrl(response, ".AppImage");
#endif

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_latestVersion = tagName;
        m_releaseUrl    = htmlUrl;
        m_releaseNotes  = body;
        m_downloadUrl   = downloadUrl;
    }

    if (compareVersions(tagName, APP_VERSION) > 0)
        m_state = UpdateState::UPDATE_AVAILABLE;
    else
        m_state = UpdateState::UP_TO_DATE;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

std::string UpdateChecker::parseJsonString(const std::string& json, const std::string& key) {
    // Looks for  "key": "value"  and returns value (handles simple escapes)
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};

    // Skip whitespace
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos || json[pos] != '"') return {};

    ++pos; // skip opening quote
    std::string result;
    result.reserve(128);
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            char esc = json[pos++];
            switch (esc) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += esc;  break;
            }
        } else if (c == '"') {
            break;
        } else {
            result += c;
        }
    }
    return result;
}

int UpdateChecker::compareVersions(const std::string& a, const std::string& b) {
    auto strip = [](const std::string& s) {
        return (s.size() > 0 && (s[0] == 'v' || s[0] == 'V')) ? s.substr(1) : s;
    };
    auto split = [](const std::string& s) -> std::vector<int> {
        std::vector<int> parts;
        std::istringstream ss(s);
        std::string token;
        while (std::getline(ss, token, '.'))
            parts.push_back(token.empty() ? 0 : std::stoi(token));
        while (parts.size() < 3) parts.push_back(0);
        return parts;
    };

    auto va = split(strip(a));
    auto vb = split(strip(b));

    for (size_t i = 0; i < 3; ++i) {
        if (va[i] != vb[i]) return va[i] - vb[i];
    }
    return 0;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

// Scans the GitHub "assets" array for the first entry whose "name" field ends
// with the given suffix (case-insensitive) and returns its "browser_download_url".
std::string UpdateChecker::parseAssetUrl(const std::string& json, const std::string& suffix) {
    // Lowercase suffix for comparison
    std::string lsuf = suffix;
    for (char& c : lsuf) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Find the assets array
    auto assetsPos = json.find("\"assets\"");
    if (assetsPos == std::string::npos) return {};

    auto arrStart = json.find('[', assetsPos);
    if (arrStart == std::string::npos) return {};

    // Walk through asset objects inside the array
    size_t pos = arrStart + 1;
    while (pos < json.size()) {
        auto objStart = json.find('{', pos);
        if (objStart == std::string::npos) break;

        // Find the matching closing brace (shallow — assets don't nest objects)
        int depth = 1;
        size_t scan = objStart + 1;
        while (scan < json.size() && depth > 0) {
            if (json[scan] == '{') ++depth;
            else if (json[scan] == '}') --depth;
            ++scan;
        }
        std::string asset = json.substr(objStart, scan - objStart);

        // Check if this asset's "name" ends with the suffix
        std::string name = parseJsonString(asset, "name");
        std::string lname = name;
        for (char& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (!lname.empty() && lname.size() >= lsuf.size() &&
            lname.compare(lname.size() - lsuf.size(), lsuf.size(), lsuf) == 0) {
            return parseJsonString(asset, "browser_download_url");
        }
        pos = scan;
    }
    return {};
}

// ── Thread-safe getters ───────────────────────────────────────────────────────

UpdateState UpdateChecker::getState() const {
    return m_state.load();
}

std::string UpdateChecker::getLatestVersion() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_latestVersion;
}

std::string UpdateChecker::getReleaseUrl() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_releaseUrl;
}

std::string UpdateChecker::getReleaseNotes() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_releaseNotes;
}

std::string UpdateChecker::getDownloadUrl() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_downloadUrl;
}
