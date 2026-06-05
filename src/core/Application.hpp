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
#include <deque>
#include <chrono>

#include "CameraModule.hpp"
#include "VideoRenderer.hpp"
#include "HUD.hpp"
#include "ObjectDetector.hpp"
#include "MultiTracker.hpp"
#include "Common.hpp"

// ---------------------------------------------------------------
// Console log entry
// ---------------------------------------------------------------
enum class LogLevel { VERBOSE = 0, INFO = 1, WARN = 2, ERR = 3 };

struct ConsoleEntry {
    LogLevel    level;
    std::string message;
    // timestamp in seconds since app start
    float       timestamp;
};

class Application {
public:
    Application();
    ~Application();

    bool init(int argc, char** argv);
    void run();

    // Thread-safe log appender (usable from worker thread)
    void log(LogLevel level, const std::string& msg);

private:
    bool initGLFW();
    bool initImGui();
    void cleanup();
    void workerLoop();
    void handleTargetLocking(const ViewportInfo& view);

    // UI render helpers
    void renderCameraView();
    void renderDataPanel();
    void renderDevConsole();
    void renderSettingsWindow();

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
    std::atomic<int>         m_requestedLockId;
    std::atomic<bool>        m_releaseLockRequested;
    bool                     m_newDataAvailable;
    float                    m_cameraFps = 0.0f;

    // ---------------------------------------------------------------
    // Dev Console state
    // ---------------------------------------------------------------
    static constexpr size_t  kMaxLogEntries  = 512;
    static constexpr size_t  kFpsHistorySize = 128;

    std::deque<ConsoleEntry> m_consoleLog;
    std::mutex               m_consoleMutex;

    // FPS ring-buffer for the performance graph
    std::vector<float>       m_fpsHistory;
    int                      m_fpsHistoryIdx = 0;

    // Per-render frame timing
    std::chrono::steady_clock::time_point m_lastRenderTime;
    float                    m_renderFps  = 0.0f;
    float                    m_frameTimeMs = 0.0f;

    // App start time (for log timestamps)
    std::chrono::steady_clock::time_point m_appStart;

    // UI state
    bool  m_showSettingsWindow = false;
    bool  m_autoScrollLog      = true;
    char  m_logFilter[128]     = {0};
    int   m_devConsoleTab      = 0;   // 0=System, 1=Detector, 2=Tracker, 3=HUD, 4=Console

    // Color arrays for ImGui::ColorEdit4 (RGBA [0,1])
    float m_hudColorF[4]    = {0.0f, 0.784f, 0.392f, 0.863f};
    float m_targetColorF[4] = {1.0f, 0.706f, 0.0f,  1.0f};

    // Worker-thread perf counters (set from worker, read in render)
    std::atomic<int>         m_workerDetectionCount{0};
    std::atomic<int>         m_workerTrackCount{0};
    std::atomic<int>         m_totalFramesProcessed{0};
};

#endif // APPLICATION_HPP
