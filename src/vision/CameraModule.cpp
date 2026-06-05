#include "CameraModule.hpp"
#include <iostream>
#include <algorithm>

CameraModule::CameraModule() {}

CameraModule::~CameraModule() {
    if (m_cap.isOpened()) {
        m_cap.release();
    }
}

bool CameraModule::open(const std::string& address, int requestedWidth, int requestedHeight) {
    bool isNumber = !address.empty() && std::all_of(address.begin(), address.end(), ::isdigit);
    if (isNumber) {
#ifdef __APPLE__
        // Try opening with AVFoundation backend explicitly on macOS
        m_cap.open(std::stoi(address), cv::CAP_AVFOUNDATION);
        if (!m_cap.isOpened()) {
            m_cap.open(std::stoi(address));
        }
#else
        m_cap.open(std::stoi(address));
#endif
        if (m_cap.isOpened()) {
            // Request MJPEG format to bypass USB bandwidth limitations for 4K streams
            if (requestedWidth > 1920) {
                m_cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
            }
            m_cap.set(cv::CAP_PROP_FRAME_WIDTH, requestedWidth);
            m_cap.set(cv::CAP_PROP_FRAME_HEIGHT, requestedHeight);
            
            // Adjust framerate for 4K since most webcams only support 4K at 30 FPS.
            // Requesting 60 FPS in 4K forces the driver to fallback to a lower resolution.
            int requestedFps = (requestedWidth > 1920) ? 30 : 60;
            m_cap.set(cv::CAP_PROP_FPS, requestedFps);
            m_cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
        }
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
