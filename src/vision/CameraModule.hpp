#ifndef CAMERA_MODULE_HPP
#define CAMERA_MODULE_HPP

#include <opencv2/opencv.hpp>
#include <string>

struct CameraDevice {
    int id;
    std::string name;
};

class CameraModule {
public:
    CameraModule();
    ~CameraModule();

    static std::vector<CameraDevice> listDevices();

    bool open(const std::string& address, int requestedWidth = 1280, int requestedHeight = 720);
    void close();
    bool read(cv::Mat& frame);
    bool isOpened() const;

    // Playback Controls (Debug/Replay Mode)
    bool isVideoFile() const { return m_isFile; }
    int  getTotalFrames() const;
    int  getCurrentFrame() const;
    double getFps() const;
    void seekToFrame(int frame);

    int getWidth() const;
    int getHeight() const;
    std::string getBackendName() const;

private:
    cv::VideoCapture m_cap;
    bool m_isFile = false;
};

namespace ImageUtils {
    void enhanceLowLight(cv::Mat& frame, cv::Mat& labMat, std::vector<cv::Mat>& channelsVec, cv::Ptr<cv::CLAHE>& claheObj, float clipLimit, int denoiseKernel);
}

#endif // CAMERA_MODULE_HPP
