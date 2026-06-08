#pragma once

#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <vector>
#include <mutex>
#include <string>

class ReIDManager {
public:
    /**
     * @brief Initialize the Re-ID system with an ONNX model.
     * @param modelPath Path to the ONNX feature extractor (e.g., OSNet).
     */
    ReIDManager(const std::string& modelPath);

    /**
     * @brief Extracts a feature vector (embedding) from an image crop.
     * @param crop The Bounding Box crop (cv::Mat).
     * @return Normalized feature vector.
     */
    std::vector<float> extractFeatures(const cv::Mat& crop);

private:
    cv::dnn::Net m_net;
    std::mutex m_netMutex;

    // Model specific constants (OSNet defaults)
    const int m_inputWidth = 128;
    const int m_inputHeight = 256;
};
