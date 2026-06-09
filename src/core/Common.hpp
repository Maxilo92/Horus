#ifndef COMMON_HPP
#define COMMON_HPP

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <set>
#include <cstdint>
#include <chrono>
#include <algorithm>

// Utility: Clamps a rectangle to stay within defined image boundaries [0, width] and [0, height].
inline cv::Rect clampRect(const cv::Rect& rect, int width, int height) {
    if (width <= 0 || height <= 0) return rect;
    int x1 = std::max(0, std::min(rect.x, width - 1));
    int y1 = std::max(0, std::min(rect.y, height - 1));
    int x2 = std::max(0, std::min(rect.x + rect.width,  width));
    int y2 = std::max(0, std::min(rect.y + rect.height, height));
    
    // Ensure width and height are at least 1 if the original rect was not empty
    int nw = std::max(rect.width > 0 ? 1 : 0, x2 - x1);
    int nh = std::max(rect.height > 0 ? 1 : 0, y2 - y1);
    
    return cv::Rect(x1, y1, nw, nh);
}

// Kein imgui.h hier – ImU32 = uint32_t, kompatibel ohne imgui-Abhängigkeit

enum class TrackingState {
    SEARCHING,
    LOCKED,
    LOST
};

struct ViewportInfo {
    float target_w, target_h;
    float pos_x, pos_y;
    float scale;
};

struct Detection {
    int class_id;
    float confidence;
    cv::Rect box;
    std::string className;
    bool is_recovery = false; // Plan 11: Recovery detections don't spawn new tracks
};

// Multi-Object-Tracking: Persistente Objektrepräsentation mit stabiler ID
struct TrackedObject {
    int   track_id;           // Stabile, eindeutige ID (monoton steigend)
    int   class_id;
    std::string className;
    cv::Rect box;             // Aktuelle Bounding Box (Kalman-korrigiert)
    float confidence;
    int   lost_frames;        // Frames seit letztem Match
    bool  is_active;          // False = Dead Reckoning / verlorener Track
    bool  is_confirmed;       // True wenn Track verlässlich bestätigt wurde

    float vx = 0.0f;          // Pixel velocity X (Kalman-estimated)
    float vy = 0.0f;          // Pixel velocity Y (Kalman-estimated)

    // Face Recognition
    int         face_id   = -1;
    std::string face_name = "";
    cv::Rect    face_box;          // Face bounding box in frame-space (empty = no face detected)

    // Centroid-History für Trail-Rendering
    std::vector<cv::Point> trail;
};

// Legacy: Für Single-Target-Lock (Mausklick-Lock Feature)
struct TrackedTarget {
    TrackingState state     = TrackingState::SEARCHING;
    int   track_id          = -1;
    int   class_id          = -1;
    std::string className;
    cv::Rect box;
    float confidence        = 0.0f;
    int   lost_frames       = 0;
    std::vector<cv::Point> trail;
};

struct TargetSnapshot {
    cv::Mat image;
    std::string timestamp;
    cv::Rect box;
    float confidence = 0.0f;
};

// Target History: Eindeutige Targets zur Aufzeichnung und Analyse
struct UniqueTargetRecord {
    int track_id = -1;
    int class_id = -1;
    std::string className;
    float max_confidence = 0.0f;
    std::string first_seen_timestamp;
    std::string last_seen_timestamp;
    cv::Rect first_box;
    cv::Rect last_box;
    std::vector<cv::Point> trail;

    // Face Recognition (Plan 11)
    int         face_id   = -1;
    std::string face_name = "";
    
    // Key milestones snapshots (containing image, timestamp, box, confidence)
    TargetSnapshot snapshot_first;
    TargetSnapshot snapshot_mid;
    TargetSnapshot snapshot_last;

    cv::Mat cropped_image_first;
    cv::Mat cropped_image_mid;
    cv::Mat cropped_image_last;
    
    int cropped_image_first_version = 0;
    int cropped_image_mid_version = 0;
    int cropped_image_last_version = 0;
    
    bool is_currently_active = false;

    // Buffer for periodic captures during active tracking
    std::vector<TargetSnapshot> periodic_snapshots;
    std::chrono::steady_clock::time_point last_snapshot_time;

    // Manual selection sync from UI to worker (-1 = auto)
    int snapshot_mid_manual_idx = -1;

    // Adaptive decimation state
    int current_snapshot_interval_ms = 500;
    int gallery_version = 0;
};

struct DossierEntry {
    std::string uuid;
    std::string type;
    std::vector<float> embedding;
    std::string first_seen;
    std::string last_seen;
    std::string dossier_text;
    int sightings_count = 0;
};

struct SystemSettings {
    // ----------------------------------------------------------------
    // Detector Settings
    // ----------------------------------------------------------------
    int   detectorModel          = 0;    // 0=yolov8s (precision), 1=yolov8n (speed)
    float detectorConfThreshold  = 0.30f; // Increased from 0.15 for fewer false positives
    float detectorScoreThreshold = 0.30f; // Increased from 0.15
    float detectorNmsThreshold   = 0.45f;

    // Priority-Class-Filter
    // COCO IDs: 0=person, 1=bicycle, 2=car, 3=motorcycle, 5=bus, 7=truck
    bool filterByPriorityClasses = true;
    std::set<int> priorityClasses = {0, 1, 2, 3, 5, 7}; // Reduced to core classes by default

    // ----------------------------------------------------------------
    // Multi-Tracker Settings
    // ----------------------------------------------------------------
    int   trackerMaxLostFrames    = 30;       // ~1s tolerance: survives brief misses/occlusion (coast track stays visible)
    float trackerMinMatchIOU      = 0.20f;    // Slightly lower IOU requirement for 4K jitter
    int   trackerMaxTrailLength   = 30;
    bool  showTrails              = true;
    float trackerMinMatchScore    = 0.35f;    // Raised from 0.25 to reduce spurious matches
    float trackerMaxCenterDistPx  = 300.0f;   // Reduced from 500 to prevent cross-track matching
    int   trackerConfirmFrames    = 3;        // Box visible only after 3 consecutive matches

    // ----------------------------------------------------------------
    // Tracking Re-acquisition Settings (Plan 11)
    // ----------------------------------------------------------------
    bool  trackerReacquisitionEnabled = true;   
    float trackerReacquisitionZoom    = 1.8f;   // Slightly tighter ROI
    bool  trackerUseMotionFallback    = true;   
    float trackerReacquisitionMaxDist = 1.6f;   // Larger search area for lost tracks

    // Single-Tracker Settings (Target-Lock Feature)
    float trackerVelocitySmoothing    = 0.4f;    // Smoother velocity estimation
    float trackerDeadReckoningDamping = 0.85f;   // More aggressive damping

    // ----------------------------------------------------------------
    // HUD Settings
    // ----------------------------------------------------------------
    bool showTacticalOverlay = true;
    bool showCrosshair       = true;
    bool showCornerBrackets  = true;
    bool showStatusWindows   = true;
    bool showDetections      = true;
    bool showTrackIDs        = true;
    bool showConfidence      = true;
    bool showTrailFade       = true;    // Trail alpha fades over time

    float hudBrightness      = 1.0f;    // Global HUD brightness multiplier [0.2 – 1.0]
    float crosshairScale     = 1.0f;    // Scale multiplier for crosshair size
    float boxLineWidth       = 1.5f;    // Bounding box line width
    float trailLineWidth     = 1.5f;    // Trail line width

    // RGBA color channels stored separately for ImGui::ColorEdit4 compatibility
    // uint32_t is binary-compatible with ImU32 (= unsigned int in imgui)
    uint32_t hudColor    = 0;  // Wird in HUD initialisiert
    uint32_t targetColor = 0;

    // ----------------------------------------------------------------
    // Performance / Pipeline Settings
    // ----------------------------------------------------------------
    bool  enableDetection    = true;   // Toggle the ONNX detector entirely
    bool  enableTracking     = true;   // Toggle tracking pipeline
    int   detectionSkipFrames = 0;     // Run detector every N+1 frames (0 = every frame)
    bool  grayscaleInput     = false;  // Convert to grayscale before detection (speed)
    bool  enableCameraMotionComp = true; // Ego-Motion: Sparse Optical Flow + RANSAC zur Kamerabewegungskompensation

    // ----------------------------------------------------------------
    // Console / Logging Settings
    // ----------------------------------------------------------------
    int   logLevel = 1;    // 0=VERBOSE, 1=INFO, 2=WARN, 3=ERROR
    bool  logToFile = false;

    // ----------------------------------------------------------------
    // Data-Logging (Plan 03)
    // ----------------------------------------------------------------
    bool        dataLoggingEnabled     = false;
    int         dataLoggingFormat      = 0;    // 0=CSV, 1=JSON-Lines
    int         dataLoggingFreqFrames  = 1;    // Log every N frames (1 = every frame)
    std::string dataLoggingOutputDir   = "";   // "" = current working directory
    std::string exportOutputDir        = "exports"; // Dedicated directory for manual exports

    // ----------------------------------------------------------------
    // ROI Settings (Plan 04)
    // ----------------------------------------------------------------
    bool showROIOverlay = true;   // Draw ROI zones in HUD

    // ----------------------------------------------------------------
    // Camera Resolution & Zoom Settings
    // ----------------------------------------------------------------
    bool request4KCamera = true;  // Request 4K (3840x2160) from camera
    bool enable4KZoom    = true;  // Crop target zoom from high-resolution frame
    float targetZoomMagnification = 1.0f; // 1.0 = neutral, >1.0 = stronger magnification in Target Zoom window

    // ----------------------------------------------------------------
    // Low-Light Enhancement Settings
    // ----------------------------------------------------------------
    bool  lowLightEnhancement   = false;  // Enable CLAHE contrast enhancement
    float lowLightClipLimit     = 3.0f;   // CLAHE contrast limit
    int   lowLightDenoiseKernel = 3;      // Gaussian blur kernel size for L channel (0 = disabled)

    // ----------------------------------------------------------------
    // Motion Detection Settings (Plan 10)
    // ----------------------------------------------------------------
    bool     motionDetectionEnabled  = false;  // Toggle motion detection pipeline
    bool     motionShowOverlay       = true;   // Draw motion regions in HUD
    bool     motionHeatmapOverlay    = false;  // Enable dense optical flow heatmap
    float    motionHeatmapDecay      = 0.90f;  // Decay rate for heatmap intensity [0.0–1.0]
    float    motionHeatmapSensitivity = 10.0f; // Max velocity for intensity normalization [1.0–50.0]
    float    motionHeatmapAlpha       = 0.60f; // Global transparency multiplier for heatmap [0.0–1.0]
    float    motionSensitivity       = 30.0f;  // MOG2 varThreshold: lower = more sensitive [5–100]
    int      motionMinArea           = 50;     // Minimum contour area in pixels to report [1–5000]
    int      motionBlurKernel        = 5;      // Gaussian pre-blur kernel size (1 = disabled, must be odd) [1–21]
    float    motionOverlayAlpha      = 0.35f;  // Fill transparency of motion overlay [0.0–1.0]
    uint32_t motionOverlayColor      = 0;      // RGBA color of motion overlay (initialised in HUD)
    bool     motionDetectShadows     = false;  // MOG2 shadow detection (reduces phantoms, costs ~15% perf)
    int      motionLearningRate      = -1;     // MOG2 learning rate: -1 = auto, 0–100 maps to [0.0–1.0]

    // Duration in seconds to hold motion tracks after the movement stops
    float    motionTrackHoldDuration = 2.0f; 

    // ----------------------------------------------------------------
    // Audio Feedback Settings
    // ----------------------------------------------------------------
    bool  audioEnabled             = true;
    float audioMasterVolume        = 0.7f;   // [0.0 – 1.0]

    // Motion alert
    bool  audioMotionEnabled       = true;
    float audioMotionFreqHz        = 880.0f;
    float audioMotionDurationMs    = 80.0f;
    float audioMotionCooldownSec   = 1.0f;   // Min. seconds between two motion beeps

    // Alarm Zone entry
    bool  audioAlarmEntryEnabled   = true;
    float audioAlarmEntryFreqHz    = 1200.0f;
    float audioAlarmEntryDurMs     = 120.0f;

    // Alarm Zone exit
    bool  audioAlarmExitEnabled    = true;
    float audioAlarmExitFreqHz     = 440.0f;
    float audioAlarmExitDurMs      = 80.0f;

    // Target Lock acquired
    bool  audioLockAcquiredEnabled = true;
    float audioLockAcquiredFreqHz  = 1000.0f;
    float audioLockAcquiredDurMs   = 150.0f;

    // Target Lock lost
    bool  audioLockLostEnabled     = true;
    float audioLockLostFreqHz      = 300.0f;
    float audioLockLostDurMs       = 200.0f;

    // Lock Pulse — continuous targeting beep, rate driven by lock confidence
    bool  audioLockPulseEnabled        = true;
    float audioLockPulseFreqHz         = 920.0f;
    float audioLockPulseDurMs          = 45.0f;
    float audioLockPulseMinIntervalMs  = 110.0f;
    float audioLockPulseMaxIntervalMs  = 750.0f;
    float audioLockPulseSolutionThresh = 0.82f;
    float audioLockPulseSolutionFreqHz = 1400.0f;
    float audioLockPulseSolutionDurMs  = 450.0f;

    // ----------------------------------------------------------------
    // Audio Capture Settings
    // ----------------------------------------------------------------
    bool     audioCaptureEnabled      = false;
    uint32_t audioCaptureDeviceId     = 0; // 0 = default

    // ----------------------------------------------------------------
    // Sub Zooms Settings
    // ----------------------------------------------------------------
    bool  subZoomsEnabled             = true;
    bool  subZoomsUseSeparateWindows  = false;
    int   subZoomPaddingPx            = 20;
    float subZoomMagnification        = 1.0f;

    // ----------------------------------------------------------------
    // Remote Inference Settings (network GPU offloading)
    // ----------------------------------------------------------------
    bool        remoteInferenceEnabled = false;
    std::string remoteInferenceIp      = "127.0.0.1";
    int         remoteInferencePort    = 8000;

    // ----------------------------------------------------------------
    // Face Detection / Recognition Settings
    // ----------------------------------------------------------------
    bool        faceRecognitionEnabled      = true;
    float       faceRecognitionThreshold    = 0.36f;  // Cosine similarity for SFace identity match
    float       faceDetectionMinConfidence  = 0.75f;  // Min YuNet confidence to auto-register new face
    bool        showFaceBoxes               = true;   // Draw separate box around each detected face
    uint32_t    faceBoxColor                = 0;      // 0 = default cyan (IM_COL32(0,220,255,220))

    // ----------------------------------------------------------------
    // AI Dossier Settings (Plan 13)
    // ----------------------------------------------------------------
    bool        aiDossierEnabled      = true;
    std::string aiOpenRouterKey       = "";
    std::string aiVlmModel            = "google/gemini-flash-1.5";
    float       aiReidThreshold       = 0.75f;
    int         aiRequestLimitPerMin  = 5;
    float       aiStabilitySec        = 3.0f;

    // ----------------------------------------------------------------
    // Debug & Performance Settings
    // ----------------------------------------------------------------
    bool debugShowRawDetections  = false; // Draw YOLO boxes before tracker filtering
    bool debugShowKalmanVectors   = false; // Draw predicted velocity vectors
    bool debugFreezeVision        = false; // Pause vision processing for inspection
    bool debugPerformanceGraphs   = true;  // Show micro-benchmarking graphs in DevConsole
};

// ─────────────────────────────────────────────────────────────────────────────
// Logging
// ─────────────────────────────────────────────────────────────────────────────

enum class LogLevel { VERBOSE = 0, INFO = 1, WARN = 2, ERR = 3 };

struct ConsoleEntry {
    LogLevel    level;
    std::string message;
    float       timestamp = 0.0f;  // seconds since app start
};

using LogFn = std::function<void(LogLevel, const std::string&)>;

#endif // COMMON_HPP

