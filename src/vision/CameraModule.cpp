#include "CameraModule.hpp"
#include <iostream>
#include <algorithm>
#include <cctype>

namespace {
void configureCaptureProperties(cv::VideoCapture& cap, int requestedWidth, int requestedHeight) {
    if (!cap.isOpened()) return;

    if (requestedWidth > 1920) {
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    }

    cap.set(cv::CAP_PROP_FRAME_WIDTH, requestedWidth);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, requestedHeight);

    // 4K streams are usually negotiated at 30 FPS; requesting 60 can force fallback.
    int requestedFps = (requestedWidth > 1920) ? 30 : 60;
    cap.set(cv::CAP_PROP_FPS, requestedFps);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
}
}

CameraModule::CameraModule() {}

CameraModule::~CameraModule() {
    if (m_cap.isOpened()) {
        m_cap.release();
    }
}

bool CameraModule::open(const std::string& address, int requestedWidth, int requestedHeight) {
    bool isNumber = !address.empty() && std::all_of(address.begin(), address.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
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
        configureCaptureProperties(m_cap, requestedWidth, requestedHeight);
    } else {
        // Prefer FFmpeg for network streams when available, then fallback to default backend.
        m_cap.open(address, cv::CAP_FFMPEG);
        if (!m_cap.isOpened()) {
            m_cap.open(address);
        }
        configureCaptureProperties(m_cap, requestedWidth, requestedHeight);
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

std::string CameraModule::getBackendName() const {
    if (!m_cap.isOpened()) return "N/A";
    return m_cap.getBackendName();
}

namespace ImageUtils {
    void enhanceLowLight(cv::Mat& frame, cv::Mat& labMat, std::vector<cv::Mat>& channelsVec, cv::Ptr<cv::CLAHE>& claheObj, float clipLimit, int denoiseKernel) {
        if (frame.empty()) return;

        // Convert to Lab colorspace and reuse provided buffers to avoid allocations
        cv::cvtColor(frame, labMat, cv::COLOR_BGR2Lab);
        cv::split(labMat, channelsVec);
        if (channelsVec.size() < 3) return;

        if (claheObj.empty()) {
            claheObj = cv::createCLAHE(clipLimit, cv::Size(8, 8));
        } else {
            claheObj->setClipLimit(clipLimit);
        }

        // Apply CLAHE to the L channel
        claheObj->apply(channelsVec[0], channelsVec[0]);

        // Optional denoise via Gaussian blur on L channel
        if (denoiseKernel > 0) {
            int k = denoiseKernel;
            if ((k % 2) == 0) ++k; // ensure odd kernel
            cv::GaussianBlur(channelsVec[0], channelsVec[0], cv::Size(k, k), 0);
        }

        cv::merge(channelsVec, labMat);
        cv::cvtColor(labMat, frame, cv::COLOR_Lab2BGR);
    }
}
