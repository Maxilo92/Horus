#ifndef APPLICATION_HPP
#define APPLICATION_HPP

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <cstring>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

#include "CameraModule.hpp"
#include "VideoRenderer.hpp"
#include "HUD.hpp"
#include "ObjectDetector.hpp"
#include "MultiTracker.hpp"
#include "DataLogger.hpp"
#include "ROIManager.hpp"
#include "MotionDetector.hpp"
#include "AudioEngine.hpp"
#include "FaceRecognizer.hpp"
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
    struct WorkerMotionTrack {
        int id;
        cv::Rect box;
        std::chrono::steady_clock::time_point lastSeen;
        bool active = true;
    };

    bool initGLFW();
    bool initImGui();
    void cleanup();
    void workerLoop();
    void captureWorkerLoop();
    void detectorWorkerLoop();
    void trackingWorkerLoop();
    void loadPersistedSettings();
    void savePersistedSettings() const;
    void applyStandardPreset();
    void applyPresetPerformance();
    void applyPresetBalanced();
    void applyPresetPrecision();
    void applyPresetLowLight();
    void syncSettingsToSharedState();
    void syncColorEditorsFromSettings();

    // UI render helpers
    void renderCameraView();
    void renderDataPanel();
    void renderZoomWindow(const cv::Mat& zoomFrame);
    void renderDevConsole();
    void renderSettingsWindow();
    void renderTargetAnalyzer();
    void updateTargetHistory(const std::vector<TrackedObject>& activeTracks, const cv::Mat& currentFrame);
    bool exportTarget(const UniqueTargetRecord& record);
    bool exportTargetHistory();
    void takeScreenshot();

    GLFWwindow* m_window;
    int         m_width;
    int         m_height;
    std::string m_title;
    std::string m_settingsPath;

    std::unique_ptr<CameraModule>    m_camera;
    std::unique_ptr<VideoRenderer>   m_renderer;
    std::unique_ptr<VideoRenderer>   m_heatmapRenderer;
    std::unique_ptr<VideoRenderer>   m_zoomRenderer;
    std::unique_ptr<HUD>             m_hud;
    std::unique_ptr<ObjectDetector>  m_detector;
    std::unique_ptr<FaceRecognizer>  m_faceRecognizer;
    std::unique_ptr<MultiTracker>    m_tracker;
    std::unique_ptr<DataLogger>      m_dataLogger;
    std::unique_ptr<ROIManager>      m_roiManager;
    MotionDetector                   m_motionDetector;
    AudioEngine                      m_audioEngine;

    std::string m_cameraAddress;

    std::vector<Detection>     m_detections;
    std::vector<TrackedObject> m_trackedObjects;
    TrackedTarget              m_lockedTarget;
    SystemSettings             m_settings;

    std::thread              m_captureThread;
    std::thread              m_workerThread;
    std::thread              m_detectorThread;
    std::thread              m_trackingThread;
    std::atomic<bool>        m_running;

    // Asynchronous capture pipeline synchronization
    std::mutex               m_captureMutex;
    cv::Mat                  m_captureFrame;
    std::atomic<bool>        m_captureNewFrameVision{false};
    std::atomic<bool>        m_captureNewFrameDisplay{false};
    
    // Asynchronous detector pipeline synchronization
    std::mutex               m_detectorMutex;
    std::condition_variable  m_detectorCv;
    std::atomic<bool>        m_detectorBusy{false};
    std::atomic<bool>        m_detectorNewResults{false};
    std::atomic<int>         m_detectorTriggerFrame{0};
    cv::Mat                  m_detectorFrameCopy;
    SystemSettings           m_detectorSettingsCopy;
    std::vector<Detection>   m_detectorResults;

    // Asynchronous tracking pipeline synchronization
    std::mutex               m_trackingMutex;
    std::condition_variable  m_trackingCv;
    std::atomic<bool>        m_trackingBusy{false};
    cv::Mat                  m_trackingFrameCopy;
    SystemSettings           m_trackingSettingsCopy;
    std::vector<Detection>   m_trackingDetectionsCopy;
    int                      m_trackingDetectorLag = 0;
    uint64_t                 m_trackingSessionMs = 0;
    std::vector<WorkerMotionTrack> m_trackingMotionTracksCopy;

    std::mutex               m_dataMutex;
    cv::Mat                  m_sharedFrame;
    cv::Mat                  m_sharedZoomFrame;
    cv::Mat                  m_sharedTrackingFrame;
    cv::Mat                  m_sharedHeatmapFrame;
    std::vector<Detection>   m_sharedDetections;
    std::vector<TrackedObject> m_sharedTrackedObjects;
    TrackedTarget            m_sharedLockedTarget;
    SystemSettings           m_sharedSettings;
    std::vector<cv::Rect>    m_sharedMotionRegions;   // Motion regions (worker → render)
    float                    m_sharedCameraFps = 0.0f;
    std::atomic<int>         m_sharedCameraWidth{0};
    std::atomic<int>         m_sharedCameraHeight{0};
    std::atomic<int>         m_sharedTrackingWidth{0};
    std::atomic<int>         m_sharedTrackingHeight{0};
    std::atomic<int>         m_sharedZoomWidth{0};
    std::atomic<int>         m_sharedZoomHeight{0};
    std::atomic<bool>        m_lockRequested;
    std::atomic<int>         m_requestedLockId;
    std::atomic<bool>        m_releaseLockRequested;
    std::atomic<int>         m_manualCaptureTargetId{-1};
    std::atomic<bool>        m_screenshotRequested{false};
    std::atomic<bool>        m_pixelLockRequested{false};
    cv::Point                m_pixelLockPoint;
    bool                     m_pixelLockActive = false;
    cv::Mat                  m_pixelTemplate;
    std::atomic<bool>        m_pixelLockDragging{false};
    std::atomic<bool>        m_pixelLockRectUpdateRequested{false};
    cv::Rect                 m_pixelLockRect;
    float                    m_pixelVx = 0.0f;
    float                    m_pixelVy = 0.0f;
    float                    m_pixelCenterX = 0.0f;
    float                    m_pixelCenterY = 0.0f;
    bool                     m_newDataAvailable;
    float                    m_cameraFps = 0.0f;
    int                      m_cameraWidth = 0;
    int                      m_cameraHeight = 0;
    int                      m_trackingWidth = 0;
    int                      m_trackingHeight = 0;
    int                      m_zoomWidth = 0;
    int                      m_zoomHeight = 0;

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
    bool  m_showDataPanel      = true;
    bool  m_showZoomWindow     = true;
    bool  m_showDevConsole     = false;
    bool  m_showTargetAnalyzer = true;
    bool  m_autoScrollLog      = true;
    char  m_logFilter[128]     = {0};
    char  m_dataPanelFilter[128] = {0};
    int   m_devConsoleTab      = 0;   // 0=System, 1=Detector, 2=Tracker, 3=HUD, 4=Console

    // ---------------------------------------------------------------
    // Camera hot-swap state
    // ---------------------------------------------------------------
    char               m_cameraInputBuf[256] = {0};  // InputText buffer
    std::string        m_pendingCameraAddress;         // Protected by m_cameraChangeMutex
    std::mutex         m_cameraChangeMutex;
    std::atomic<bool>  m_cameraChangeRequested{false};
    std::string        m_cameraStatus;                 // e.g. "OK" / "FAILED" / "..."
    bool               m_cameraStatusOk = true;

    // Color arrays for ImGui::ColorEdit4 (RGBA [0,1])
    float m_hudColorF[4]           = {0.0f, 0.784f, 0.392f, 0.863f};
    float m_targetColorF[4]        = {1.0f, 0.706f, 0.0f,  1.0f};
    float m_motionOverlayColorF[4] = {1.0f, 0.35f,  0.0f,  0.65f}; // Default: orange-red

    // Worker-thread perf counters (set from worker, read in render)
    std::atomic<int>         m_workerDetectionCount{0};
    std::atomic<int>         m_workerTrackCount{0};
    std::atomic<int>         m_totalFramesProcessed{0};

    // ---------------------------------------------------------------
    // Logging state (Plan 03)
    // ---------------------------------------------------------------
    int      m_logFrameCounter    = 0;  // counts frames for loggingFreqFrames
    uint64_t m_logSessionStartMs  = 0;  // epoch ms when logging started
    bool     m_dataLoggingWasOn   = false; // tracks prev state for start/stop

    enum class ROIEditState {
        NONE,
        DRAWING,
        MOVING,
        RESIZING_TL,
        RESIZING_TR,
        RESIZING_BL,
        RESIZING_BR,
        RESIZING_L,
        RESIZING_R,
        RESIZING_T,
        RESIZING_B
    };

    bool          m_roiEditMode    = false;
    ROIEditState  m_editState      = ROIEditState::NONE;
    int           m_editZoneId     = -1;
    cv::Point     m_editDragStartMouse;
    cv::Rect      m_editDragStartRect;
    // label input buffer per ROI (max kMaxZones)
    char          m_roiLabelBuf[ROIManager::kMaxZones][64] = {};

    // Active track-zone alarm states for transition warnings
    std::unordered_map<int, std::unordered_set<int>> m_activeAlarms;

    // ---------------------------------------------------------------
    // Audio feedback state
    // ---------------------------------------------------------------
    // Previous lock state for detecting LOCKED→LOST transitions
    TrackingState m_prevLockState = TrackingState::SEARCHING;
    // Previous confirmed tracked-object count for lock-acquired detection
    bool          m_prevLockWasActive = false;

    // Feedback UI state
    bool m_showFeedbackWindow = false;
    char m_feedbackBuf[1024] = {0};
    std::string m_feedbackStatus;
    bool saveFeedback(const std::string& feedback);

    // Target History & Analyzer State
    std::vector<UniqueTargetRecord> m_internalTargetHistory;
    std::vector<UniqueTargetRecord> m_targetHistory;
    std::vector<UniqueTargetRecord> m_sharedTargetHistory;
    int m_selectedAnalyzerTargetId = -1;
    struct TextureInfo {
        uint32_t texture_id_first = 0;
        int texture_version_first = -1;
        uint32_t texture_id_mid = 0;
        int texture_version_mid = -1;
        uint32_t texture_id_last = 0;
        int texture_version_last = -1;
        
        std::vector<uint32_t> texture_ids_periodic;
        std::vector<int> texture_versions_periodic;
        int gallery_version = -1;

        float max_confidence = 0.0f;
    };
    std::unordered_map<int, TextureInfo> m_targetTextures;

    struct AnalyzerUiState {
        bool show_full_gallery = false;
        int  selected_snapshot_idx = -1;
        bool show_large_view_modal = false;
        int  large_view_snapshot_idx = -1;
        uint32_t large_view_tex_id = 0;
    };
    std::unordered_map<int, AnalyzerUiState> m_analyzerUiStates;

    // ---------------------------------------------------------------
    // Sub Zooms and Motion Tracking State
    // ---------------------------------------------------------------
    struct SharedSubZoom {
        bool active = false;
        int motion_id = -1;
        cv::Rect box;
        cv::Mat frame;
        bool isLost = false;
        std::string precisionLabel = "";
        float precisionConfidence = 0.0f;
    };
    SharedSubZoom m_sharedSubZooms[4];
    SharedSubZoom m_subZooms[4]; // render-thread local copy
    std::unique_ptr<VideoRenderer> m_subZoomRenderers[4];

    std::vector<WorkerMotionTrack> m_workerMotionTracks;
    int m_nextMotionId = 1;

    // Face Recognition identity cache (Plan 11)
    // track_id -> {face_id, face_name}
    std::unordered_map<int, std::pair<int, std::string>> m_trackIdToFace;
    std::mutex m_faceMutex;

    // Precision Search for SubZooms cache (motion_id -> {className, confidence})
    std::unordered_map<int, std::pair<std::string, float>> m_motionPrecisionCache;
    std::mutex m_motionPrecisionMutex;
};

#endif // APPLICATION_HPP
