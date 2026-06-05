#include "CameraModule.hpp"
#include <iostream>
#include <algorithm>

CameraModule::CameraModule() {}

CameraModule::~CameraModule() {
    if (m_cap.isOpened()) {
        m_cap.release();
    }
}

bool CameraModule::open(const std::string& address) {
    bool isNumber = !address.empty() && std::all_of(address.begin(), address.end(), ::isdigit);
    if (isNumber) {
        m_cap.open(std::stoi(address));
        m_cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
        m_cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
        m_cap.set(cv::CAP_PROP_FPS, 60);
        m_cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    } else {
        m_cap.open(address);
        m_cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    }
    return m_cap.isOpened();
}

void CameraModule::close() {
    if (m_cap.isOpened()) m_cap.release();
}

bool CameraModule::read(cv::Mat& frame) {
    return m_cap.read(frame);
}


bool CameraModule::isOpened() const {
    return m_cap.isOpened();
}

int CameraModule::getWidth() const {
    return (int)m_cap.get(cv::CAP_PROP_FRAME_WIDTH);
}

int CameraModule::getHeight() const {
    return (int)m_cap.get(cv::CAP_PROP_FRAME_HEIGHT);
}
