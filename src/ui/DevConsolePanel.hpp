#pragma once
#include "Common.hpp"
#include "Blackboard.hpp"
#include <deque>
#include <mutex>
#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include <imgui.h>

struct GLFWwindow;

class DevConsolePanel {
public:
    DevConsolePanel();
    ~DevConsolePanel() = default;

    void appendLog(LogLevel level, const std::string& msg, float appSeconds);
    void clearLog();

    void render(bool& show,
                float renderFps, float frameTimeMs, float cameraFps,
                float inferTimeMs, float trackTimeMs, int loggerQueueSize,
                int camW, int camH, int trackW, int trackH, int zoomW, int zoomH,
                int activeTracks, int detections, uint64_t totalFrames,
                const std::vector<std::string>& classNames,
                std::string& cameraAddress,
                const AppStatusState& status,
                bool isCameraChangePending,
                std::function<void(const std::string&)> requestCameraChange,
                std::function<void()> requestMotionReset,
                std::function<void()> applyStandardPreset,
                SystemSettings& settings,
                bool& settingsChanged,
                std::function<void(LogLevel, const std::string&)> logFn,
                GLFWwindow* window,
                float remoteRttMs = -1.0f,
                const std::string& activeModelName = "",
                const TrackingStateData* trackingState = nullptr,
                const FaceDebugState* faceDbg = nullptr);

    // Timing history
    void addFpsSample(float fps);
    void addPerformanceSamples(float inferMs, float trackTimeMs);

private:
    static constexpr size_t kFpsHistorySize = 128;
    std::vector<float>      m_fpsHistory;
    int                     m_fpsHistoryIdx = 0;

    std::vector<float>      m_inferHistory;
    std::vector<float>      m_trackHistory;
    int                     m_perfHistoryIdx = 0;

    static constexpr size_t kMaxLogEntries = 512;
    std::deque<ConsoleEntry> m_consoleLog;
    std::mutex               m_consoleMutex;
    bool                     m_autoScrollLog = true;
    char                     m_logFilter[128] = {0};
    
    char m_cameraInputBuf[256] = {0};
    float m_motionOverlayColorF[4] = {1.0f, 0.35f, 0.0f, 0.65f};
};
