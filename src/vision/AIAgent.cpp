#include "AIAgent.hpp"
#include <curl/curl.h>
#include <iostream>
#include <sstream>
#include <chrono>

static size_t AIWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Escapes a string for embedding inside a JSON string literal.
static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) break; // andere Steuerzeichen verwerfen
                out += c;
        }
    }
    return out;
}

// Extracts the string value of the first "content" key from a JSON response.
// Tolerates compact JSON ("content":"...") and decodes the standard escapes —
// the previous fixed search for '"content": "' (with space) never matched
// OpenRouter's compact responses, so dossiers were silently never written.
static bool ExtractJsonContent(const std::string& json, std::string& out) {
    size_t keyPos = json.find("\"content\"");
    if (keyPos == std::string::npos) return false;

    size_t p = json.find_first_not_of(" \t\r\n", keyPos + 9);
    if (p == std::string::npos || json[p] != ':') return false;
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || json[p] != '"') return false;
    ++p;

    out.clear();
    while (p < json.size()) {
        char c = json[p];
        if (c == '"') return true; // unescaped closing quote
        if (c == '\\' && p + 1 < json.size()) {
            char esc = json[p + 1];
            switch (esc) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u': // \uXXXX – keep it simple: skip the escape, emit '?' for non-ASCII
                    if (p + 5 < json.size()) {
                        unsigned int cp = 0;
                        sscanf(json.c_str() + p + 2, "%4x", &cp);
                        if (cp < 128) out += static_cast<char>(cp); else out += '?';
                        p += 4;
                    }
                    break;
                default: out += esc; break;
            }
            p += 2;
        } else {
            out += c;
            ++p;
        }
    }
    return false; // unterminated string
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

    // 1. Encode Image to Base64 and persist a resized thumbnail in the DB.
    std::vector<uchar> buf;
    cv::imencode(".jpg", req.crop, buf);
    std::string b64 = base64Encode(buf);

    // Thumbnail: max 160px wide so the blob stays small (~5–15 KB).
    if (!req.crop.empty()) {
        cv::Mat thumb;
        const int maxW = 160;
        if (req.crop.cols > maxW) {
            float scale = static_cast<float>(maxW) / req.crop.cols;
            cv::resize(req.crop, thumb, cv::Size(), scale, scale, cv::INTER_AREA);
        } else {
            thumb = req.crop;
        }
        std::vector<uchar> thumbBuf;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 80};
        cv::imencode(".jpg", thumb, thumbBuf, params);
        m_db.updateEntityThumbnail(req.entityUuid, thumbBuf);
    }

    // 2. Prepare JSON Payload (Manual construction to avoid heavy dependencies)
    const bool isVehicle = (req.type == "car" || req.type == "truck" ||
                            req.type == "bus" || req.type == "motorcycle");

    // Markdown-Struktur, damit das Archiv den Text formatiert rendern kann.
    std::string prompt =
        "Describe this " + req.type + " precisely for a surveillance dossier. "
        "Respond in Markdown with exactly these sections: '## Overview' (1-2 sentences), "
        "'## Identifying Features' (bullet list: colors, markings, estimated size, distinguishing details), "
        "'## Assessment' (1 sentence on notable behavior or threat relevance). Keep it under 150 words.";
    if (isVehicle) {
        // Kennzeichenerkennung: maschinenlesbare erste Zeile, wird unten geparst.
        prompt = "FIRST LINE of your response must be exactly 'PLATE: <license plate text>' "
                 "if a license plate is readable in the image, or 'PLATE: NONE' if not. Then continue with: " + prompt;
    }
    if (req.isUpdate) {
        prompt += " Previous information: " + req.currentDossier + ". Update with new observations.";
    }

    std::stringstream json;
    json << "{"
         << "\"model\": \"" << JsonEscape(settings.aiVlmModel) << "\","
         << "\"messages\": ["
         << "  {"
         << "    \"role\": \"user\","
         << "    \"content\": ["
         << "      {\"type\": \"text\", \"text\": \"" << JsonEscape(prompt) << "\"},"
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
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

            std::string aiText;
            if (httpCode >= 200 && httpCode < 300 && ExtractJsonContent(responseString, aiText) && !aiText.empty()) {
                // Kennzeichen aus der ersten 'PLATE:'-Zeile ziehen (nur Fahrzeuge)
                // und aus dem Dossier-Text entfernen — es wird separat gespeichert/angezeigt.
                if (isVehicle) {
                    size_t platePos = aiText.find("PLATE:");
                    if (platePos != std::string::npos && platePos < 32) {
                        size_t lineEnd = aiText.find('\n', platePos);
                        std::string plate = aiText.substr(platePos + 6,
                            (lineEnd == std::string::npos ? aiText.size() : lineEnd) - platePos - 6);
                        // Trimmen
                        size_t b = plate.find_first_not_of(" \t*`");
                        size_t e = plate.find_last_not_of(" \t*`\r");
                        plate = (b == std::string::npos) ? "" : plate.substr(b, e - b + 1);

                        aiText.erase(0, (lineEnd == std::string::npos) ? aiText.size() : lineEnd + 1);
                        // führende Leerzeilen nach dem Strip entfernen
                        while (!aiText.empty() && (aiText.front() == '\n' || aiText.front() == '\r'))
                            aiText.erase(0, 1);

                        if (!plate.empty() && plate != "NONE" && plate != "UNKNOWN") {
                            if (m_db.updateEntityPlate(req.entityUuid, plate)) {
                                m_log(LogLevel::INFO, "[AIAgent] License plate for " + req.entityUuid + ": " + plate);
                                DossierState ds = m_blackboard.getDossierState();
                                if (ds.hasActiveDossier && ds.activeDossier.uuid == req.entityUuid) {
                                    ds.activeDossier.plate = plate;
                                    m_blackboard.setDossierState(ds);
                                }
                            }
                        }
                    }
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
            } else {
                // Fehler sichtbar machen: HTTP-Code + (gekürzte) Antwort, z.B. wenn das
                // konfigurierte VLM-Modell nicht (mehr) existiert oder der Key ungültig ist.
                std::string snippet = responseString.substr(0, 500);
                m_log(LogLevel::ERR, "[AIAgent] API error (HTTP " + std::to_string(httpCode) +
                      ", model=" + settings.aiVlmModel + "): " + snippet);
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
