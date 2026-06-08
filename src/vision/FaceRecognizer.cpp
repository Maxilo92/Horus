#include "FaceRecognizer.hpp"
#include <iostream>
#include <algorithm>

FaceRecognizer::FaceRecognizer(const std::string& detectorPath, const std::string& recognizerPath) {
    // Initialize YuNet detector
    // inputSize is (320, 320) by default, will be updated per frame
    m_detector = cv::FaceDetectorYN::create(detectorPath, "", cv::Size(320, 320), 0.6f, 0.3f, 5000);
    
    // Initialize SFace recognizer
    m_recognizer = cv::FaceRecognizerSF::create(recognizerPath, "");
}

float FaceRecognizer::computeCosineDistance(const cv::Mat& feat1, const cv::Mat& feat2) {
    return static_cast<float>(m_recognizer->match(feat1, feat2, cv::FaceRecognizerSF::DisType::FR_COSINE));
}

int FaceRecognizer::registerIdentity(const std::string& name, const cv::Mat& embedding) {
    FaceIdentity identity;
    {
        std::lock_guard<std::mutex> lock(m_dbMutex);
        identity.id = m_nextId++;
        identity.name = name;
        identity.embedding = embedding.clone();
        m_database.push_back(identity);
    }
    // Persist outside the lock to avoid holding the mutex during DB I/O.
    if (m_onIdentityChanged) m_onIdentityChanged(identity);
    return identity.id;
}

void FaceRecognizer::seedIdentities(const std::vector<FaceIdentity>& ids, int nextId) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    m_database = ids;
    m_nextId = std::max(nextId, 1);
}

bool FaceRecognizer::renameIdentity(int id, const std::string& newName) {
    FaceIdentity updated;
    {
        std::lock_guard<std::mutex> lock(m_dbMutex);
        auto it = std::find_if(m_database.begin(), m_database.end(),
                               [id](const FaceIdentity& fi) { return fi.id == id; });
        if (it == m_database.end()) return false;
        it->name = newName;
        updated = *it;
    }
    if (m_onIdentityChanged) m_onIdentityChanged(updated);
    return true;
}

std::vector<FaceRecognizer::FaceResult> FaceRecognizer::process(const cv::Mat& frame, const cv::Rect& personBox,
                                                                float matchThreshold, float minDetConfidence) {
    const float threshold = matchThreshold;
    if (frame.empty()) return {};

    // Crop the person region for faster/more focused face detection
    // Pad the box slightly to ensure the head isn't cut off
    int pad = static_cast<int>(personBox.width * 0.1f);
    cv::Rect roi = personBox;
    roi.x = std::max(0, roi.x - pad);
    roi.y = std::max(0, roi.y - pad);
    roi.width = std::min(frame.cols - roi.x, roi.width + 2 * pad);
    roi.height = std::min(frame.rows - roi.y, roi.height + 2 * pad);

    if (roi.width < 20 || roi.height < 20) return {};

    // Clone to ensure contiguous memory — YuNet ONNX backend requires it.
    // frame(roi) is a submatrix with row-stride = full frame width, not roi width.
    cv::Mat personImg = frame(roi).clone();

    // Scale up tiny crops so YuNet anchors can resolve faces inside them.
    // Below ~80px width the model's smallest anchor can't fire reliably.
    constexpr int kMinDetSize = 160;
    float inputScale = 1.0f;
    if (personImg.cols < kMinDetSize || personImg.rows < kMinDetSize) {
        inputScale = static_cast<float>(kMinDetSize) /
                     static_cast<float>(std::min(personImg.cols, personImg.rows));
        cv::resize(personImg, personImg,
                   cv::Size(static_cast<int>(personImg.cols * inputScale),
                            static_cast<int>(personImg.rows * inputScale)));
    }

    // Update detector input size
    m_detector->setInputSize(personImg.size());

    cv::Mat faces;
    m_detector->detect(personImg, faces);

    std::vector<FaceResult> results;
    for (int i = 0; i < faces.rows; ++i) {
        FaceResult res;
        // Bounding box is in faces.row(i).colRange(0, 4)
        float x = faces.at<float>(i, 0);
        float y = faces.at<float>(i, 1);
        float w = faces.at<float>(i, 2);
        float h = faces.at<float>(i, 3);
        
        // Scale detected box back to original frame coordinates.
        // If personImg was upscaled, inputScale > 1 and we must divide back.
        res.box = cv::Rect(
            static_cast<int>(x / inputScale) + roi.x,
            static_cast<int>(y / inputScale) + roi.y,
            static_cast<int>(w / inputScale),
            static_cast<int>(h / inputScale));
        
        // Align and feature extraction
        cv::Mat alignedFace;
        m_recognizer->alignCrop(personImg, faces.row(i), alignedFace);
        
        cv::Mat feature;
        m_recognizer->feature(alignedFace, feature);
        res.embedding = feature.clone();

        // Database lookup
        float bestScore = -1.0f;
        int bestId = -1;
        std::string bestName = "Unknown";

        {
            std::lock_guard<std::mutex> lock(m_dbMutex);
            for (const auto& ident : m_database) {
                float score = computeCosineDistance(feature, ident.embedding);
                if (score > bestScore) {
                    bestScore = score;
                    bestId = ident.id;
                    bestName = ident.name;
                }
            }
        }

        // OpenCV SFace cosine similarity threshold is typically around 0.36
        if (bestScore >= threshold) {
            res.identityId = bestId;
            res.name = bestName;
            res.matchScore = bestScore;
        } else {
            // Self-register new identity if it's a "clean" detection (high confidence)
            float detConf = faces.at<float>(i, 14); // Confidence is at index 14
            if (detConf >= minDetConfidence) {
                std::string newName = "Unbekannt " + std::to_string(m_nextId);
                res.identityId = registerIdentity(newName, feature);
                res.name = newName;
                res.matchScore = 1.0f;
            }
        }
        
        results.push_back(res);
    }

    return results;
}
