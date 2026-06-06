#ifndef CAMERA_MODULE_HPP
#define CAMERA_MODULE_HPP

#include <opencv2/opencv.hpp>
#include <string>

class CameraModule {
public:
    CameraModule();
    ~CameraModule();

    bool open(const std::string& address, int requestedWidth = 1280, int requestedHeight = 720);
    void close();
    bool read(cv::Mat& frame);
    bool isOpened() const;

    int getWidth() const;
    int getHeight() const;
    std::string getBackendName() const;

private:
    cv::VideoCapture m_cap;
};

namespace ImageUtils {
    void enhanceLowLight(cv::Mat& frame, cv::Mat& labMat, std::vector<cv::Mat>& channelsVec, cv::Ptr<cv::CLAHE>& claheObj, float clipLimit, int denoiseKernel);
}

#endif // CAMERA_MODULE_HPP
