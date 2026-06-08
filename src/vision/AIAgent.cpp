#include "AIAgent.hpp"
#include <curl/curl.h>
#include <iostream>
#include <sstream>
#include <chrono>

static size_t AIWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

AIAgent::AIAgent(Blackboard& blackboard, DossierDatabase& db, LogFn logFn)
    : m_blackboard(blackboard), m_db(db), m_log(std::move(logFn)) {}

AIAgent::~AIAgent() {
    stop();
}

void AIAgent::start() {
    if (m_running.load()) return;
    m_running = true;
    m_workerThread = std::thread(&AIAgent::workerLoop, this);
}

void AIAgent::stop() {
    m_running = false;
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void AIAgent::queueRequest(const AIRequest& request) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    // Prevent massive queue buildup
    if (m_queue.size() > 50) {
        m_log(LogLevel::WARN, "[AIAgent] Queue full, dropping request for " + request.entityUuid);
        return;
    }

    m_queue.push(request);
    
    DossierState ds = m_blackboard.getDossierState();
    ds.queueSize = static_cast<int>(m_queue.size());
    m_blackboard.setDossierState(ds);

    m_cv.notify_one();
}

void AIAgent::workerLoop() {
    while (m_running.load()) {
        AIRequest req;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [this] { return !m_queue.empty() || !m_running.load(); });
            
            if (!m_running.load()) break;
            
            req = m_queue.front();
            m_queue.pop();

            DossierState ds = m_blackboard.getDossierState();
            ds.queueSize = static_cast<int>(m_queue.size());
            m_blackboard.setDossierState(ds);
        }

        if (checkRateLimit()) {
            processRequest(req);
        } else {
            m_log(LogLevel::WARN, "[AIAgent] Global rate limit reached. Delaying request for " + req.entityUuid);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            // Re-queue
            queueRequest(req);
        }
    }
}

bool AIAgent::checkRateLimit() {
    std::lock_guard<std::mutex> lock(m_limitMutex);
    auto now = std::chrono::steady_clock::now();
    
    // Remove history older than 1 minute
    m_requestHistory.erase(std::remove_if(m_requestHistory.begin(), m_requestHistory.end(),
        [&now](const auto& t) {
            return std::chrono::duration_cast<std::chrono::seconds>(now - t).count() > 60;
        }), m_requestHistory.end());

    SystemSettings settings = m_blackboard.getSettings();
    if (m_requestHistory.size() >= static_cast<size_t>(settings.aiRequestLimitPerMin)) {
        return false;
    }

    m_requestHistory.push_back(now);
    return true;
}

void AIAgent::processRequest(const AIRequest& req) {
    SystemSettings settings = m_blackboard.getSettings();
    if (settings.aiOpenRouterKey.empty()) {
        m_log(LogLevel::ERR, "[AIAgent] Missing OpenRouter API Key.");
        return;
    }

    m_log(LogLevel::INFO, "[AIAgent] Processing dossier for " + req.type + " [" + req.entityUuid + "]");

    // 1. Encode Image to Base64
    std::vector<uchar> buf;
    cv::imencode(".jpg", req.crop, buf);
    std::string b64 = base64Encode(buf);

    // 2. Prepare JSON Payload (Manual construction to avoid heavy dependencies)
    std::string prompt = "Describe this " + req.type + " precisely for a military-grade dossier. Focus on identifying features, colors, and estimated size.";
    if (req.isUpdate) {
        prompt += " Previous information: " + req.currentDossier + ". Update with new observations.";
    }

    std::stringstream json;
    json << "{"
         << "\"model\": \"" << settings.aiVlmModel << "\","
         << "\"messages\": ["
         << "  {"
         << "    \"role\": \"user\","
         << "    \"content\": ["
         << "      {\"type\": \"text\", \"text\": \"" << prompt << "\"},"
         << "      {\"type\": \"image_url\", \"image_url\": {\"url\": \"data:image/jpeg;base64," << b64 << "\"}}"
         << "    ]"
         << "  }"
         << "]"
         << "}";

    // 3. HTTP Request
    CURL* curl = curl_easy_init();
    if (curl) {
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string authHeader = "Authorization: Bearer " + settings.aiOpenRouterKey;
        headers = curl_slist_append(headers, authHeader.c_str());

        std::string responseString;
        curl_easy_setopt(curl, CURLOPT_URL, "https://openrouter.ai/api/v1/chat/completions");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.str().c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AIWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            // Very basic JSON extraction for "content"
            size_t contentPos = responseString.find("\"content\": \"");
            if (contentPos != std::string::npos) {
                contentPos += 12;
                size_t endPos = responseString.find("\"", contentPos);
                if (endPos != std::string::npos) {
                    std::string aiText = responseString.substr(contentPos, endPos - contentPos);
                    // Replace escaped newlines
                    size_t nPos = 0;
                    while ((nPos = aiText.find("\\n", nPos)) != std::string::npos) {
                        aiText.replace(nPos, 2, "\n");
                        nPos += 1;
                    }

                    if (m_db.updateDossierText(req.entityUuid, aiText)) {
                        m_log(LogLevel::INFO, "[AIAgent] Dossier updated for " + req.entityUuid);
                        
                        // If this was the currently locked entity, update Blackboard state
                        DossierState ds = m_blackboard.getDossierState();
                        if (ds.hasActiveDossier && ds.activeDossier.uuid == req.entityUuid) {
                            ds.activeDossier.dossier_text = aiText;
                            m_blackboard.setDossierState(ds);
                        }
                    }
                }
            } else {
                 m_log(LogLevel::ERR, "[AIAgent] API response error: " + responseString);
            }
        } else {
            m_log(LogLevel::ERR, std::string("[AIAgent] HTTP Request failed: ") + curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

std::string AIAgent::base64Encode(const std::vector<uchar>& data) {
    static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret;
    int i = 0;
    int j = 0;
    uchar char_array_3[3];
    uchar char_array_4[4];
    size_t in_len = data.size();
    auto it = data.begin();

    while (in_len--) {
        char_array_3[i++] = *(it++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for(i = 0; (i <4) ; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for(j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (j = 0; (j < i + 1); j++) ret += base64_chars[char_array_4[j]];
        while((i++ < 3)) ret += '=';
    }
    return ret;
}
