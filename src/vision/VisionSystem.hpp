#ifndef VISION_SYSTEM_HPP
#define VISION_SYSTEM_HPP

#include "IModule.hpp"
#include "Blackboard.hpp"
#include "CameraModule.hpp"
#include "ObjectDetector.hpp"
#include "MotionDetector.hpp"
#include "AudioEngine.hpp"
#include "DossierDatabase.hpp"
#include "AIAgent.hpp"
#include "ReIDManager.hpp"
#include <opencv2/video/tracking.hpp>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>

class TrackingSystem;

class VisionSystem : public IModule {
public:
    using LogFn = std::function<void(LogLevel, const std::string&)>;

    VisionSystem(Blackboard& blackboard, AudioEngine& audioEngine, 
                 DossierDatabase* dossierDb, AIAgent* aiAgent, LogFn logFn);
    ~VisionSystem() override;

    bool init(const std::string& cameraAddress, const std::string& modelPath,
              const std::string& labelsPath, const std::string& initialAddress);

    void setTrackingSystem(TrackingSystem* ts) { m_trackingSystem = ts; }

    // Expose the shared detector for TrackingSystem re-acquisition
    ObjectDetector* getDetector() const { return m_detector.get(); }

    void start() override;
    void stop() override;
    void update() override;

private:
    void captureWorkerLoop();
    void detectorWorkerLoop();
    void workerLoop();

    Blackboard&   m_blackboard;
    AudioEngine&  m_audioEngine;
    DossierDatabase* m_dossierDb = nullptr;
    AIAgent*      m_aiAgent      = nullptr;
    LogFn         m_log;
    TrackingSystem* m_trackingSystem = nullptr;

    std::string m_cameraAddress;
    std::string m_modelPathS;   // yolov8s (precision)
    std::string m_modelPathN;   // yolov8n (speed)
    std::string m_labelsPath;
    std::string m_reidModelPath;

    std::unique_ptr<CameraModule>   m_camera;
    std::unique_ptr<ObjectDetector> m_detector;
    std::unique_ptr<ReIDManager>    m_reidManager;
    MotionDetector                  m_motionDetector;

    std::thread          m_captureThread;
    std::thread          m_detectorThread;
    std::thread          m_workerThread;
    std::atomic<bool>    m_running{false};

    // Capture pipeline synchronization
    std::mutex           m_captureMutex;
    cv::Mat              m_captureFrame;
    std::atomic<bool>    m_captureNewFrameVision{false};

    // Detector pipeline synchronization
    std::mutex               m_detectorMutex;
    std::condition_variable  m_detectorCv;
    std::atomic<bool>        m_detectorBusy{false};
    std::atomic<bool>        m_detectorNewResults{false};
    std::atomic<int>         m_detectorTriggerFrame{0};   // worker-iteration counter value at detector dispatch
    // Monotonic worker-loop iteration counter. Detector lag is measured in these units so it
    // matches the Kalman velocity time base (px per tracker iteration). Worker-thread only.
    uint64_t                 m_workerFrameCounter{0};
    cv::Mat                  m_detectorFrameCopy;
    SystemSettings           m_detectorSettingsCopy;
    std::vector<Detection>   m_detectorResults;

    // Cached detections for tracking (latest available)
    std::vector<Detection>   m_detections;

    // Motion track assignments for sub-zooms
    struct WorkerMotionTrack {
        int id;
        cv::Rect box;
        std::chrono::steady_clock::time_point lastSeen;
        bool active = true;

        std::unique_ptr<cv::KalmanFilter> kalman;
        cv::Point2f smoothedCenter;
        cv::Size2f smoothedSize;
    };
    std::vector<WorkerMotionTrack> m_workerMotionTracks;
    int m_nextMotionId = 1;

    // Logging state
    uint64_t m_logSessionStartMs = 0;

    // AI Dossier Stability Tracking
    struct StabilityInfo {
        std::chrono::steady_clock::time_point firstSeen;
        bool reidDone = false;
        bool aiRequested = false;
        std::string lastUuid = "";
    };
    std::map<int, StabilityInfo> m_trackStability;
};

#endif // VISION_SYSTEM_HPP
