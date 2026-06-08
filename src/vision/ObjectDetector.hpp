#pragma once
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <vector>
#include <fstream>
#include <sstream>
#include <curl/curl.h>
#include "Common.hpp"

static inline size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

class ObjectDetector {
public:
    ObjectDetector(const std::string& modelPath, const std::string& classesPath) {
        net = cv::dnn::readNetFromONNX(modelPath);
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);

        // Lade Klassen: eine Klasse pro Zeile
        std::ifstream ifs(classesPath);
        if (!ifs.is_open()) {
            // Fallback: COCO-Standardklassen hartcodiert
            classes = {"person","bicycle","car","motorcycle","airplane","bus","train","truck"};
            return;
        }
        std::string line;
        while (std::getline(ifs, line)) {
            // Trim trailing whitespace/CR
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (!line.empty())
                classes.push_back(line);
        }
    }

    static std::vector<Detection> parseDetectionsJson(const std::string& json, const std::vector<std::string>& classes) {
        std::vector<Detection> detections;
        size_t pos = 0;
        const int numClasses = static_cast<int>(classes.size());
        
        while ((pos = json.find("\"class_id\"", pos)) != std::string::npos) {
            size_t colon = json.find(":", pos);
            if (colon == std::string::npos) break;
            
            size_t nextComma = json.find(",", colon);
            size_t nextBracket = json.find("}", colon);
            size_t limit = std::min(nextComma, nextBracket);
            if (limit == std::string::npos) break;
            int classId = std::stoi(json.substr(colon + 1, limit - colon - 1));
            
            size_t confPos = json.find("\"confidence\"", pos);
            if (confPos == std::string::npos) break;
            colon = json.find(":", confPos);
            if (colon == std::string::npos) break;
            nextComma = json.find(",", colon);
            nextBracket = json.find("}", colon);
            limit = std::min(nextComma, nextBracket);
            if (limit == std::string::npos) break;
            float confidence = std::stof(json.substr(colon + 1, limit - colon - 1));
            
            size_t boxPos = json.find("\"box\"", pos);
            if (boxPos == std::string::npos) break;
            size_t bracketOpen = json.find("[", boxPos);
            if (bracketOpen == std::string::npos) break;
            size_t bracketClose = json.find("]", bracketOpen);
            if (bracketClose == std::string::npos) break;
            
            std::string boxStr = json.substr(bracketOpen + 1, bracketClose - bracketOpen - 1);
            std::stringstream ss(boxStr);
            std::string val;
            int x = 0, y = 0, w = 0, h = 0;
            if (std::getline(ss, val, ',')) x = std::stoi(val);
            if (std::getline(ss, val, ',')) y = std::stoi(val);
            if (std::getline(ss, val, ',')) w = std::stoi(val);
            if (std::getline(ss, val, ',')) h = std::stoi(val);
            
            if (classId >= 0 && classId < numClasses) {
                Detection d;
                d.class_id = classId;
                d.confidence = confidence;
                d.box = cv::Rect(x, y, w, h);
                d.className = classes[classId];
                detections.push_back(d);
            }
            
            pos = bracketClose;
        }
        return detections;
    }

    std::vector<Detection> detectRemote(cv::Mat& frame, const SystemSettings& settings) {
        if (classes.empty()) return {};

        // Scale to 640x640 for YOLOv8 input format (keeping aspect ratio)
        float scale = std::min(640.0f / (float)frame.cols, 640.0f / (float)frame.rows);
        int new_w = static_cast<int>(frame.cols * scale);
        int new_h = static_cast<int>(frame.rows * scale);
        int offset_x = (640 - new_w) / 2;
        int offset_y = (640 - new_h) / 2;

        cv::Mat resized;
        cv::resize(frame, resized, cv::Size(new_w, new_h));
        
        cv::Mat canvas = cv::Mat::zeros(cv::Size(640, 640), frame.type());
        resized.copyTo(canvas(cv::Rect(offset_x, offset_y, new_w, new_h)));

        // Compress canvas to JPEG
        std::vector<uchar> buf;
        cv::imencode(".jpg", canvas, buf, {cv::IMWRITE_JPEG_QUALITY, 75});

        std::string response;
        CURL* curl = curl_easy_init();
        if (curl) {
            std::string url = "http://" + settings.remoteInferenceIp + ":" + 
                              std::to_string(settings.remoteInferencePort) + "/detect";
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            
            // Set reasonable network timeout: 1.5 seconds
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
            
            curl_mime* mime = curl_mime_init(curl);
            curl_mimepart* part = curl_mime_addpart(mime);
            curl_mime_name(part, "file");
            curl_mime_data(part, (const char*)buf.data(), buf.size());
            curl_mime_filename(part, "frame.jpg");
            curl_mime_type(part, "image/jpeg");
            
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            
            CURLcode res = curl_easy_perform(curl);
            curl_mime_free(mime);
            curl_easy_cleanup(curl);
            
            if (res == CURLE_OK) {
                std::vector<Detection> remoteDets = parseDetectionsJson(response, classes);
                
                // Map the coordinates from the 640x640 canvas back to original frame coordinates
                for (auto& d : remoteDets) {
                    float cx = d.box.x + d.box.width / 2.0f;
                    float cy = d.box.y + d.box.height / 2.0f;
                    float w = d.box.width;
                    float h = d.box.height;

                    int left = static_cast<int>((cx - 0.5f * w - (float)offset_x) / scale);
                    int top = static_cast<int>((cy - 0.5f * h - (float)offset_y) / scale);
                    int width = static_cast<int>(w / scale);
                    int height = static_cast<int>(h / scale);

                    d.box = clampRect(cv::Rect(left, top, width, height), frame.cols, frame.rows);
                }
                
                // Priority class filter (just like local)
                if (settings.filterByPriorityClasses) {
                    std::vector<Detection> filtered;
                    for (const auto& d : remoteDets) {
                        if (settings.priorityClasses.find(d.class_id) != settings.priorityClasses.end()) {
                            filtered.push_back(d);
                        }
                    }
                    return filtered;
                }
                return remoteDets;
            }
        }
        return {};
    }

    std::vector<Detection> detect(cv::Mat& frame, const SystemSettings& settings) {
        if (classes.empty()) return {};

        if (settings.remoteInferenceEnabled) {
            return detectRemote(frame, settings);
        }

        // Letterbox resizing: Keep aspect ratio and pad to 640x640
        float scale = std::min(640.0f / (float)frame.cols, 640.0f / (float)frame.rows);
        int new_w = static_cast<int>(frame.cols * scale);
        int new_h = static_cast<int>(frame.rows * scale);
        int offset_x = (640 - new_w) / 2;
        int offset_y = (640 - new_h) / 2;

        cv::Mat resized;
        cv::resize(frame, resized, cv::Size(new_w, new_h));
        
        cv::Mat canvas = cv::Mat::zeros(cv::Size(640, 640), frame.type());
        resized.copyTo(canvas(cv::Rect(offset_x, offset_y, new_w, new_h)));

        cv::Mat blob;
        // YOLOv8 benötigt 640x640, Normalisierung 1/255
        cv::dnn::blobFromImage(canvas, blob, 1.0/255.0, cv::Size(640, 640), cv::Scalar(), true, false);

        std::vector<cv::Mat> outputs;
        {
            std::lock_guard<std::mutex> lock(m_netMutex);
            net.setInput(blob);
            net.forward(outputs, net.getUnconnectedOutLayersNames());
        }

        // YOLOv8 Output Shape: [1, 84, 8400]
        // 84 = 4 (box: cx,cy,w,h) + 80 (Klassen-Scores)
        cv::Mat output = outputs[0];
        if (output.dims == 3) {
            output = output.reshape(1, output.size[1]); // [84, 8400]
        }
        cv::transpose(output, output); // [8400, 84]

        std::vector<int> class_ids;
        std::vector<float> confidences;
        std::vector<cv::Rect> boxes;

        const int numModelClasses = std::max(0, output.cols - 4);
        const int numClasses = std::min(static_cast<int>(classes.size()), numModelClasses);
        if (numClasses <= 0) return {};

        const float* data = reinterpret_cast<const float*>(output.data);
        const int stride = output.cols;

        for (int i = 0; i < output.rows; ++i) {
            const float* row = data + i * stride;
            const float* class_scores = row + 4;

            // Finde beste Klasse in diesem Kandidaten
            int best_class = -1;
            float best_score = 0.0f;
            for (int c = 0; c < numClasses; ++c) {
                if (class_scores[c] > best_score) {
                    best_score = class_scores[c];
                    best_class = c;
                }
            }

            if (best_score < settings.detectorScoreThreshold) continue;

            // Priority-Class-Filter: Nur relevante Klassen melden
            if (settings.filterByPriorityClasses &&
                settings.priorityClasses.find(best_class) == settings.priorityClasses.end()) {
                continue;
            }

            confidences.push_back(best_score);
            class_ids.push_back(best_class);

            const float cx = row[0];
            const float cy = row[1];
            const float w  = row[2];
            const float h  = row[3];

            const int left   = static_cast<int>((cx - 0.5f * w - (float)offset_x) / scale);
            const int top    = static_cast<int>((cy - 0.5f * h - (float)offset_y) / scale);
            const int width  = static_cast<int>(w / scale);
            const int height = static_cast<int>(h / scale);
            boxes.push_back(clampRect(cv::Rect(left, top, width, height), frame.cols, frame.rows));
        }

        std::vector<int> nms_indices;
        cv::dnn::NMSBoxes(boxes, confidences, settings.detectorConfThreshold,
                          settings.detectorNmsThreshold, nms_indices);

        std::vector<Detection> detections;
        detections.reserve(nms_indices.size());
        for (int idx : nms_indices) {
            const int cid = class_ids[idx];
            // Bounds-Check: Verhindert undefined behavior bei fehlerhaftem Modell-Output
            if (cid < 0 || cid >= numClasses) continue;

            Detection d;
            d.class_id  = cid;
            d.confidence = confidences[idx];
            d.box       = boxes[idx];
            d.className = classes[cid];
            detections.push_back(d);
        }

        return detections;
    }

    int numClasses() const { return static_cast<int>(classes.size()); }
    const std::vector<std::string>& getClasses() const { return classes; }

private:
    cv::dnn::Net net;
    std::vector<std::string> classes;
    std::mutex m_netMutex;
};
