#ifndef BLACKBOARD_HPP
#define BLACKBOARD_HPP

#include <mutex>
#include <vector>
#include <opencv2/opencv.hpp>
#include "Common.hpp"
#include "MotionDetector.hpp" // For WorkerMotionTrack equivalent, but we might want a simpler struct here

// Struct to represent motion tracking from worker
struct MotionTrack {
    int id;
    cv::Rect box;
    bool active;
    std::chrono::steady_clock::time_point lastSeen;
};

// Struct to represent sub-zooms
struct SubZoomData {
    bool active = false;
    int motion_id = -1;
    cv::Rect box;
    cv::Mat frame;
    bool isLost = false;
    std::string precisionLabel = "";
    float precisionConfidence = 0.0f;
};

// --- Isolated State Structs ---

struct VisionState {
    cv::Mat zoomCrop;
    cv::Mat heatmapFrame;
    
    int cameraWidth = 0;
    int cameraHeight = 0;
    int trackingWidth = 0;
    int trackingHeight = 0;
    int zoomWidth = 0;
    int zoomHeight = 0;
    
    float cameraFps = 0.0f;
    
    SubZoomData subZooms[4];
};

struct DetectionState {
    std::vector<Detection> detections;
    std::vector<cv::Rect> motionRegions;
    std::vector<MotionTrack> motionTracks;
};

struct TrackingStateData {
    std::vector<TrackedObject> activeTracks;
    TrackedTarget lockedTarget;
    std::vector<UniqueTargetRecord> targetHistory;
    bool pixelLockActive = false;
};

struct UICommandState {
    bool lockRequested = false;
    int requestedLockId = -1;
    bool releaseLockRequested = false;

    bool pixelLockRequested = false;
    cv::Rect pixelLockRect;
    bool pixelLockRectUpdateRequested = false;
    bool pixelLockDragging = false;

    bool cameraChangeRequested = false;
    std::string pendingCameraAddress;

    int manualCaptureTargetId = -1;
    bool screenshotRequested = false;

    bool motionDetectorResetRequested = false;

    int snapshotMidManualTrackId = -1;
    int snapshotMidManualIdx = -1;

    bool modelSwitchRequested = false;
    int  requestedModelIdx    = 0;  // 0=yolov8s, 1=yolov8n
};

struct AppStatusState {
    std::string cameraStatus = "Initializing...";
    bool cameraStatusOk = true;
    std::string cameraAddress;

    std::vector<std::string> cameraDeviceNames;
    std::vector<int> cameraDeviceIDs;

    std::vector<std::string> audioDeviceNames;
    std::vector<uint32_t> audioDeviceIDs;

    int workerDetectionCount = 0;
    int workerTrackCount = 0;
    int totalFramesProcessed = 0;

    std::vector<std::string> classNames;

    float remoteInferenceRttMs = -1.0f;  // -1 = not used / no data yet
    std::string activeModelName;          // e.g. "yolov8s" or "yolov8n"
};

struct ReplayState {
    bool isFile = false;
    bool isPlaying = true;
    int currentFrame = 0;
    int totalFrames = 0;
    double fps = 0.0;
};

struct ReplayCommand {
    bool pauseRequested = false;
    bool playRequested = false;
    bool stepRequested = false;
    int stepDirection = 1; // 1 = forward, -1 = backward
    bool seekRequested = false;
    int seekFrame = 0;
};

struct PerformanceMetrics {
    float captureTimeMs    = 0.0f;
    float inferenceTimeMs  = 0.0f;
    float trackingTimeMs   = 0.0f;
    float uiRenderTimeMs   = 0.0f;
    int   dataLoggerQueue  = 0;
};

struct AudioState {
    float intensity = 0.0f; // 0.0 to 1.0, decayed over time
    bool  pulseActive = false;
    float pulseStrength = 0.0f;
    std::chrono::steady_clock::time_point lastActive;
};


class Blackboard {
public:
    Blackboard() = default;
    ~Blackboard() = default;

    // --- Settings ---
    void setSettings(const SystemSettings& settings) {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        m_settings = settings;
    }
    
    SystemSettings getSettings() const {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        return m_settings;
    }

    // --- Audio State ---
    void setAudioState(const AudioState& state) {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        m_audioState = state;
    }

    AudioState getAudioState() const {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        return m_audioState;
    }

    void updateAudioIntensity(float intensity) {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        if (intensity > m_audioState.intensity) {
            m_audioState.intensity = intensity;
        }
        m_audioState.lastActive = std::chrono::steady_clock::now();
    }

    // --- Vision Data (Frames) ---
    void setVisionState(const VisionState& state) {
        std::lock_guard<std::mutex> lock(m_visionMutex);
        m_visionState.zoomCrop      = state.zoomCrop;
        m_visionState.heatmapFrame = state.heatmapFrame;
        
        m_visionState.cameraWidth = state.cameraWidth;
        m_visionState.cameraHeight = state.cameraHeight;
        m_visionState.trackingWidth = state.trackingWidth;
        m_visionState.trackingHeight = state.trackingHeight;
        m_visionState.zoomWidth = state.zoomWidth;
        m_visionState.zoomHeight = state.zoomHeight;
        m_visionState.cameraFps = state.cameraFps;
        
        for (int i = 0; i < 4; ++i) {
            m_visionState.subZooms[i].active = state.subZooms[i].active;
            m_visionState.subZooms[i].motion_id = state.subZooms[i].motion_id;
            m_visionState.subZooms[i].box = state.subZooms[i].box;
            m_visionState.subZooms[i].isLost = state.subZooms[i].isLost;
            m_visionState.subZooms[i].precisionLabel = state.subZooms[i].precisionLabel;
            m_visionState.subZooms[i].precisionConfidence = state.subZooms[i].precisionConfidence;
            m_visionState.subZooms[i].frame = state.subZooms[i].frame;
        }
    }

    VisionState getVisionState() const {
        std::lock_guard<std::mutex> lock(m_visionMutex);
        return m_visionState;
    }

    // --- Detection Data ---
    void setDetectionState(const DetectionState& state) {
        std::lock_guard<std::mutex> lock(m_detectionMutex);
        m_detectionState = state;
    }

    DetectionState getDetectionState() const {
        std::lock_guard<std::mutex> lock(m_detectionMutex);
        return m_detectionState;
    }

    // --- Tracking Data ---
    void setTrackingState(const TrackingStateData& state) {
        std::lock_guard<std::mutex> lock(m_trackingMutex);
        m_trackingState = state;
    }

    TrackingStateData getTrackingState() const {
        std::lock_guard<std::mutex> lock(m_trackingMutex);
        return m_trackingState;
    }

    // --- UI Commands ---
    void setUICommand(const UICommandState& cmd) {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand = cmd;
    }

    // --- System-Specific Command Consumption ---

    // TrackingSystem consumes tracking-related commands
    UICommandState consumeTrackingCommands() {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        UICommandState copy = m_uiCommand;
        // Reset only tracking-related volatile commands
        m_uiCommand.lockRequested = false;
        m_uiCommand.releaseLockRequested = false;
        m_uiCommand.pixelLockRequested = false;
        m_uiCommand.pixelLockRectUpdateRequested = false;
        m_uiCommand.motionDetectorResetRequested = false;
        m_uiCommand.snapshotMidManualTrackId = -1;
        m_uiCommand.snapshotMidManualIdx = -1;
        m_uiCommand.manualCaptureTargetId = -1; // Also reset manual capture to avoid repeated triggers
        return copy;
    }

    // VisionSystem consumes vision-related commands (currently just model switching)
    UICommandState consumeVisionCommands() {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        UICommandState copy = m_uiCommand;
        // Reset only vision-related volatile commands
        m_uiCommand.modelSwitchRequested = false;
        return copy;
    }

    // --- Replay Control ---
    void setReplayState(const ReplayState& state) {
        std::lock_guard<std::mutex> lock(m_replayMutex);
        m_replayState = state;
    }

    ReplayState getReplayState() const {
        std::lock_guard<std::mutex> lock(m_replayMutex);
        return m_replayState;
    }

    void setReplayCommand(const ReplayCommand& cmd) {
        std::lock_guard<std::mutex> lock(m_replayMutex);
        if (cmd.pauseRequested) m_replayCommand.pauseRequested = true;
        if (cmd.playRequested)  m_replayCommand.playRequested = true;
        if (cmd.stepRequested) {
            m_replayCommand.stepRequested = true;
            m_replayCommand.stepDirection = cmd.stepDirection;
        }
        if (cmd.seekRequested) {
            m_replayCommand.seekRequested = true;
            m_replayCommand.seekFrame = cmd.seekFrame;
        }
    }

    ReplayCommand consumeReplayCommands() {
        std::lock_guard<std::mutex> lock(m_replayMutex);
        ReplayCommand copy = m_replayCommand;
        m_replayCommand.pauseRequested = false;
        m_replayCommand.playRequested = false;
        m_replayCommand.stepRequested = false;
        m_replayCommand.seekRequested = false;
        return copy;
    }
    
    void requestTargetLock(int trackId) {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.lockRequested = true;
        m_uiCommand.requestedLockId = trackId;
    }
    
    void requestTargetRelease() {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.releaseLockRequested = true;
    }
    
    void requestPixelLock(const cv::Rect& rect) {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.pixelLockRequested = true;
        m_uiCommand.pixelLockRect = rect;
    }
    
    void requestCameraChange(const std::string& address) {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.cameraChangeRequested = true;
        m_uiCommand.pendingCameraAddress = address;
    }

    // VisionSystem polls only the camera-change flag without resetting other commands
    bool consumeCameraChangeRequest(std::string& outAddress) {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        if (!m_uiCommand.cameraChangeRequested) return false;
        outAddress = m_uiCommand.pendingCameraAddress;
        m_uiCommand.cameraChangeRequested = false;
        return true;
    }

    void setPixelLockDragging(bool dragging) {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.pixelLockDragging = dragging;
    }

    void requestManualCapture(int trackId) {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.manualCaptureTargetId = trackId;
    }

    void requestScreenshot() {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.screenshotRequested = true;
    }

    void requestMotionDetectorReset() {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.motionDetectorResetRequested = true;
    }

    void requestPixelLockRectUpdate(const cv::Rect& rect) {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.pixelLockRect = rect;
        m_uiCommand.pixelLockRectUpdateRequested = true;
    }

    void requestModelSwitch(int modelIdx) {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.modelSwitchRequested = true;
        m_uiCommand.requestedModelIdx    = modelIdx;
    }

    void requestSnapshotMidManual(int trackId, int snapshotIdx) {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        m_uiCommand.snapshotMidManualTrackId = trackId;
        m_uiCommand.snapshotMidManualIdx = snapshotIdx;
    }

    void setClassNames(const std::vector<std::string>& names) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_appStatus.classNames = names;
    }

    void setCameraAddress(const std::string& addr) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_appStatus.cameraAddress = addr;
    }

    void setAudioDevices(const std::vector<std::string>& names, const std::vector<uint32_t>& ids) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_appStatus.audioDeviceNames = names;
        m_appStatus.audioDeviceIDs = ids;
    }

    void setCameraDevices(const std::vector<std::string>& names, const std::vector<int>& ids) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_appStatus.cameraDeviceNames = names;
        m_appStatus.cameraDeviceIDs = ids;
    }

    bool isCameraChangePending() const {
        std::lock_guard<std::recursive_mutex> lock(m_uiCmdMutex);
        return m_uiCommand.cameraChangeRequested;
    }

    // --- Display Frame Fast Path (Triple-Buffered) ---
    // Triple-buffering ensures that the capture thread never blocks on the UI
    // thread and vice-versa, providing jitter-free real-time frame delivery.
    void updateDisplayFrame(const cv::Mat& frame) {
        if (frame.empty()) return;

        // 1. Determine next write slot (the one that is NOT the current ready or read slot)
        int ready = m_displayReadyIdx.load(std::memory_order_relaxed);
        int read  = m_displayReadIdx.load(std::memory_order_relaxed);
        int write = 0;
        if (ready != 0 && read != 0) write = 0;
        else if (ready != 1 && read != 1) write = 1;
        else write = 2;

        // 2. Perform copy into private write slot
        frame.copyTo(m_displaySlots[write]);

        // 3. Atomically publish as the new 'ready' slot
        m_displayReadyIdx.store(write, std::memory_order_release);
        m_displayFrameNew.store(true, std::memory_order_release);
    }

    bool consumeDisplayFrame(cv::Mat& out) {
        if (!m_displayFrameNew.load(std::memory_order_acquire)) return false;

        // 1. Get the latest ready slot
        int ready = m_displayReadyIdx.exchange(-1, std::memory_order_acquire);
        if (ready == -1) return false;

        // 2. Perform shallow copy from ready slot into output
        // Reference counting ensures the data remains valid even if we point to it.
        // Triple buffering prevents the producer from overwriting this slot while we read it.
        out = m_displaySlots[ready];
        
        // 3. Keep track of current read index to assist producer
        m_displayReadIdx.store(ready, std::memory_order_release);
        m_displayFrameNew.store(false, std::memory_order_release);

        return true;
    }

    // --- App Status ---
    void setAppStatus(const AppStatusState& status) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_appStatus = status;
    }
    
    void updateStatusCounts(int detCount, int trackCount, int framesIncrement) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        if (detCount >= 0) m_appStatus.workerDetectionCount = detCount;
        if (trackCount >= 0) m_appStatus.workerTrackCount = trackCount;
        m_appStatus.totalFramesProcessed += framesIncrement;
    }

    AppStatusState getAppStatus() const {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        return m_appStatus;
    }

    // --- Performance Metrics ---
    void setPerformanceMetrics(const PerformanceMetrics& metrics) {
        std::lock_guard<std::mutex> lock(m_performanceMutex);
        m_performanceMetrics = metrics;
    }

    PerformanceMetrics getPerformanceMetrics() const {
        std::lock_guard<std::mutex> lock(m_performanceMutex);
        return m_performanceMetrics;
    }

private:
    mutable std::mutex m_settingsMutex;
    SystemSettings m_settings;

    mutable std::mutex m_visionMutex;
    VisionState m_visionState;

    mutable std::mutex m_detectionMutex;
    DetectionState m_detectionState;

    mutable std::mutex m_trackingMutex;
    TrackingStateData m_trackingState;

    mutable std::recursive_mutex m_uiCmdMutex;
    UICommandState m_uiCommand;

    mutable std::mutex m_statusMutex;
    AppStatusState m_appStatus;

    mutable std::mutex m_performanceMutex;
    PerformanceMetrics m_performanceMetrics;

    mutable std::mutex m_audioMutex;
    AudioState m_audioState;

    mutable std::mutex m_replayMutex;
    ReplayState m_replayState;
    ReplayCommand m_replayCommand;

    // Triple-Buffered Display Frame
    cv::Mat m_displaySlots[3];
    std::atomic<int> m_displayReadyIdx{0}; // Index of the latest published frame
    std::atomic<int> m_displayReadIdx{1};  // Index currently/last used by reader
    std::atomic<bool> m_displayFrameNew{false};
};

#endif // BLACKBOARD_HPP
