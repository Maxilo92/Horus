#ifndef CAMERA_MODULE_HPP
#define CAMERA_MODULE_HPP

#include <opencv2/opencv.hpp>
#include <string>

class CameraModule {
public:
    CameraModule();
    ~CameraModule();

    bool open(const std::string& address);
    bool read(cv::Mat& frame);
    bool isOpened() const;

    int getWidth() const;
    int getHeight() const;

private:
    cv::VideoCapture m_cap;
};

#endif // CAMERA_MODULE_HPP
