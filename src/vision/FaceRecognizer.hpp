#pragma once

#include <opencv2/opencv.hpp>
#include <opencv2/objdetect.hpp>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include "Common.hpp"

struct FaceIdentity {
    int id;
    std::string name;
    cv::Mat embedding;
};

class FaceRecognizer {
public:
    FaceRecognizer(const std::string& detectorPath, const std::string& recognizerPath);

    struct FaceResult {
        cv::Rect box;
        int identityId = -1;
        std::string name = "Unknown";
        float matchScore = 0.0f;
        cv::Mat embedding;
    };

    // Process a specific region (person ROI) to find and identify a face.
    // matchThreshold: cosine similarity to consider two faces the same identity.
    // minDetConfidence: min YuNet confidence to auto-register an unknown face.
    std::vector<FaceResult> process(const cv::Mat& frame, const cv::Rect& personBox,
                                    float matchThreshold = 0.36f,
                                    float minDetConfidence = 0.95f);

    // Register a new identity or update an existing one
    int registerIdentity(const std::string& name, const cv::Mat& embedding);

    // Load previously persisted identities (e.g. from the database) at startup.
    // nextId is the value the auto-incrementing id counter should resume from.
    void seedIdentities(const std::vector<FaceIdentity>& ids, int nextId);

    // Rename an existing identity. Returns false if the id is unknown.
    bool renameIdentity(int id, const std::string& newName);

    // Called whenever an identity is created or renamed, so the owner can persist it.
    void setPersistCallback(std::function<void(const FaceIdentity&)> cb) {
        m_onIdentityChanged = std::move(cb);
    }

    // Returns the number of registered identities in the in-memory database.
    int identityCount() const {
        std::lock_guard<std::mutex> lock(m_dbMutex);
        return static_cast<int>(m_database.size());
    }

private:
    cv::Ptr<cv::FaceDetectorYN> m_detector;
    cv::Ptr<cv::FaceRecognizerSF> m_recognizer;

    std::vector<FaceIdentity> m_database;
    mutable std::mutex m_dbMutex;
    int m_nextId = 1;

    std::function<void(const FaceIdentity&)> m_onIdentityChanged;

    float computeCosineDistance(const cv::Mat& feat1, const cv::Mat& feat2);
};
