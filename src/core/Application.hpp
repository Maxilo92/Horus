#ifndef APPLICATION_HPP
#define APPLICATION_HPP

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

#include "CameraModule.hpp"
#include "VideoRenderer.hpp"
#include "HUD.hpp"
#include "ObjectDetector.hpp"
#include "MultiTracker.hpp"
#include "Common.hpp"

class Application {
public:
    Application();
    ~Application();

    bool init(int argc, char** argv);
    void run();

private:
    bool initGLFW();
    bool initImGui();
    void cleanup();
    void workerLoop();
    void handleTargetLocking(const ViewportInfo& view);

    GLFWwindow* m_window;
    int         m_width;
    int         m_height;
    std::string m_title;

    std::unique_ptr<CameraModule>    m_camera;
    std::unique_ptr<VideoRenderer>   m_renderer;
    std::unique_ptr<HUD>             m_hud;
    std::unique_ptr<ObjectDetector>  m_detector;
    std::unique_ptr<MultiTracker>    m_tracker;

    std::string m_cameraAddress;

    std::vector<Detection>     m_detections;
    std::vector<TrackedObject> m_trackedObjects;
    TrackedTarget              m_lockedTarget;
    SystemSettings             m_settings;

    std::thread              m_workerThread;
    std::atomic<bool>        m_running;
    std::mutex               m_dataMutex;
    cv::Mat                  m_sharedFrame;
    std::vector<Detection>     m_sharedDetections;
    std::vector<TrackedObject> m_sharedTrackedObjects;
    TrackedTarget            m_sharedLockedTarget;
    SystemSettings           m_sharedSettings;
    float                    m_sharedCameraFps = 0.0f;
    std::atomic<bool>        m_lockRequested;
    Detection                m_requestedLockTarget;
    bool                     m_newDataAvailable;
    float                    m_cameraFps = 0.0f;
};

#endif // APPLICATION_HPP
