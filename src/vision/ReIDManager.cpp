#include "ReIDManager.hpp"
#include <iostream>

ReIDManager::ReIDManager(const std::string& modelPath) {
    try {
        m_net = cv::dnn::readNetFromONNX(modelPath);
        m_net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        m_net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    } catch (const cv::Exception& e) {
        std::cerr << "[ReIDManager] Error loading model: " << e.what() << std::endl;
    }
}

std::vector<float> ReIDManager::extractFeatures(const cv::Mat& crop) {
    if (crop.empty() || m_net.empty()) return {};

    cv::Mat blob;
    // OSNet usually expects BGR input, mean/std normalization
    // Standard ImageNet normalization: mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]
    // OpenCV blobFromImage handles scaling (1/255.0) and mean subtraction.
    // However, many ONNX exports expect [0, 255] or handled internally.
    // We'll use a standard resize + 1/255 scale for now.
    cv::dnn::blobFromImage(crop, blob, 1.0/255.0, cv::Size(m_inputWidth, m_inputHeight), cv::Scalar(0,0,0), true, false);

    cv::Mat feat;
    {
        std::lock_guard<std::mutex> lock(m_netMutex);
        m_net.setInput(blob);
        feat = m_net.forward();
    }

    // Flatten and normalize the feature vector (L2 normalization for Cosine Similarity)
    std::vector<float> embedding;
    if (feat.empty()) return {};

    feat = feat.reshape(1, 1); // Ensure 1D row
    float norm = static_cast<float>(cv::norm(feat, cv::NORM_L2));
    if (norm > 1e-6) {
        feat /= norm;
    }

    for (int i = 0; i < feat.cols; ++i) {
        embedding.push_back(feat.at<float>(0, i));
    }

    return embedding;
}
