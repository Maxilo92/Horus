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
              ObjectDetector* sharedDetector);

    void start() override;
    void stop() override;
    void update() override;

    // Called by VisionSystem::workerLoop to hand off a frame for tracking
    void submitFrame(const cv::Mat& frame, const SystemSettings& settings,
                     const std::vector<Detection>& detections, int detectorLag,
                     uint64_t sessionMs, const std::vector<MotionTrack>& motionTracks);

private:
    void trackingWorkerLoop();
    void updateTargetHistory(const std::vector<TrackedObject>& activeTracks,
                              const cv::Mat& currentFrame, int manualCaptureId);

    Blackboard&  m_blackboard;
    ROIManager&  m_roiManager;
    DataLogger&  m_dataLogger;
    AudioEngine& m_audioEngine;
    LogFn        m_log;

    std::unique_ptr<MultiTracker>    m_tracker;
    std::unique_ptr<FaceRecognizer>  m_faceRecognizer;
    ObjectDetector*                  m_detector = nullptr;

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

    // Pixel template tracking state
    bool     m_pixelLockActive = false;
    cv::Mat  m_pixelTemplate;
    cv::Rect m_pixelLockRect;
    float    m_pixelVx = 0.0f;
    float    m_pixelVy = 0.0f;
    float    m_pixelCenterX = 0.0f;
    float    m_pixelCenterY = 0.0f;

    // Face recognition identity cache (track_id -> {face_id, face_name})
    std::unordered_map<int, std::pair<int, std::string>> m_trackIdToFace;
    std::mutex m_faceMutex;

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
