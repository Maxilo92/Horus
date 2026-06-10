#ifndef TRACKING_SYSTEM_HPP
#define TRACKING_SYSTEM_HPP

#include "IModule.hpp"
#include "Blackboard.hpp"
#include "MultiTracker.hpp"
#include "ROIManager.hpp"
#include "DataLogger.hpp"
#include "AudioEngine.hpp"
#include "FaceRecognizer.hpp"
#include "ObjectDetector.hpp"
#include "DossierDatabase.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <unordered_set>

class TrackingSystem : public IModule {
public:
    using LogFn = std::function<void(LogLevel, const std::string&)>;

    TrackingSystem(Blackboard& blackboard, ROIManager& roiManager,
                   DataLogger& dataLogger, AudioEngine& audioEngine,
                   LogFn logFn);
    ~TrackingSystem() override;

    bool init(const std::string& faceDetPath, const std::string& faceRecPath,
              DossierDatabase* dossierDb);
void start() override;
void stop() override;
void update() override;

    // Called by VisionSystem::workerLoop to hand off a frame for tracking.
    // hasNewDetections: false when YOLO is still running and detections is stale/empty.
    void submitFrame(const cv::Mat& frame, const SystemSettings& settings,
                     const std::vector<Detection>& detections, int detectorLag,
                     uint64_t sessionMs, const std::vector<MotionTrack>& motionTracks,
                     bool hasNewDetections = true);

private:
    void trackingWorkerLoop();
    void updateTargetHistory(const std::vector<TrackedObject>& activeTracks,
                              const cv::Mat& currentFrame, int manualCaptureId);

    // --- trackingWorkerLoop helpers (one per numbered section) ---
    void handleLockCommands(const UICommandState& cmd);
    void handlePixelLockCommands(const UICommandState& cmd, const cv::Mat& frame);
    void runReacquisition(std::vector<Detection>& detections,
                          const cv::Mat& frame,
                          const SystemSettings& settings,
                          const std::vector<MotionTrack>& motionTracks);
    void deduplicateDetections(std::vector<Detection>& detections);
    cv::Point2d estimateCameraMotion(const cv::Mat& frame, const SystemSettings& settings);
    void updateTrackerAndFaces(std::vector<TrackedObject>& tracked,
                               const cv::Mat& frame,
                               const std::vector<Detection>& detections,
                               const SystemSettings& settings,
                               int detectorLag,
                               const cv::Point2d& cameraMotion,
                               bool hasNewDetections);
    void runPixelTemplateMatch(std::vector<TrackedObject>& tracked,
                               const cv::Mat& frame,
                               const UICommandState& cmd,
                               const SystemSettings& settings);
    void updateLockedTargetYolo(const std::vector<TrackedObject>& tracked);
    void updateAudioFeedback(const SystemSettings& settings);
    void checkAlarmZones(const std::vector<TrackedObject>& tracked);
    void computeTargetPriorities(std::vector<TrackedObject>& tracked,
                                 const SystemSettings& settings,
                                 cv::Size frameSize);
    void logTrackingData(const std::vector<TrackedObject>& tracked,
                         const std::vector<MotionTrack>& motionTracks,
                         const SystemSettings& settings,
                         uint64_t sessionMs);

    Blackboard&  m_blackboard;
    ROIManager&  m_roiManager;
    DataLogger&  m_dataLogger;
    AudioEngine& m_audioEngine;
    LogFn        m_log;

    std::unique_ptr<MultiTracker>    m_tracker;
    std::unique_ptr<FaceRecognizer>  m_faceRecognizer;
    ObjectDetector*                  m_detector = nullptr;
    DossierDatabase*                 m_dossierDb = nullptr;

    std::thread          m_trackingThread;
    std::atomic<bool>    m_running{false};

    // Tracking pipeline synchronization (mirrors Application's original design)
    std::mutex               m_trackingMutex;
    std::condition_variable  m_trackingCv;
    std::atomic<bool>        m_trackingBusy{false};
    cv::Mat                  m_trackingFrameCopy;
    SystemSettings           m_trackingSettingsCopy;
    std::vector<Detection>   m_trackingDetectionsCopy;
    int                      m_trackingDetectorLag = 0;
    uint64_t                 m_trackingSessionMs = 0;
    std::vector<MotionTrack> m_trackingMotionTracksCopy;
    bool                     m_trackingHasNewDetections = true;

    // Kamerabewegungsschätzung (Ego-Motion via Sparse Optical Flow)
    cv::Mat      m_prevFrameGray;
    cv::Point2d  m_prevCameraMotion{0.0, 0.0}; // for temporal smoothing of the ego-motion estimate

    // Pixel template tracking state
    bool     m_pixelLockActive = false;
    cv::Mat  m_pixelTemplate;
    cv::Rect m_pixelLockRect;
    float    m_pixelVx = 0.0f;
    float    m_pixelVy = 0.0f;
    float    m_pixelCenterX = 0.0f;
    float    m_pixelCenterY = 0.0f;

    struct FaceTrackInfo {
        int         face_id = -1;
        std::string face_name;
        cv::Rect    face_box;
        cv::Rect    last_person_box; // person box when face was last detected
        int         retryCountdown = 0; // frames until next recognition attempt
    };
    // Face recognition identity cache: track_id -> {face_id, face_name, face_box}
    std::unordered_map<int, FaceTrackInfo> m_trackIdToFace;
    std::mutex m_faceMutex;

    // Target-Priorität: Verlaufsdaten pro Track (Annäherung, Neuheit)
    struct PriorityMemory {
        std::chrono::steady_clock::time_point firstSeen{};
        float smoothedArea = 0.0f; // EMA der Boxfläche zur Annäherungserkennung
        float areaGrowth   = 0.0f; // geglättete relative Flächenänderung pro Update
    };
    std::unordered_map<int, PriorityMemory> m_prioMem;

    // Accumulated face recognition stats — updated by tracking thread, pushed to Blackboard.
    FaceDebugState m_faceDbg;

    // Alarm zone state (zone_id -> set of track_ids currently inside)
    std::unordered_map<int, std::unordered_set<int>> m_activeAlarms;

    // Audio transition state
    TrackingState m_prevLockState    = TrackingState::SEARCHING;
    int           m_lockStableFrames = 0;  // Consecutive frames with a clean lock (no lost_frames)

    // Internal target history (written to Blackboard after each tracking iteration)
    std::vector<UniqueTargetRecord> m_internalTargetHistory;

    // Shared locked target (owned by TrackingSystem, pushed to Blackboard)
    TrackedTarget m_sharedLockedTarget;
};

#endif // TRACKING_SYSTEM_HPP
