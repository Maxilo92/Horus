#ifndef COMMON_HPP
#define COMMON_HPP

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <set>
#include <cstdint>

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

struct SystemSettings {
    // ----------------------------------------------------------------
    // Detector Settings
    // ----------------------------------------------------------------
    float detectorConfThreshold  = 0.15f;
    float detectorScoreThreshold = 0.15f;
    float detectorNmsThreshold   = 0.45f;

    // Priority-Class-Filter
    // COCO IDs: 0=person, 1=bicycle, 2=car, 3=motorcycle, 5=bus, 7=truck
    bool filterByPriorityClasses = true;
    std::set<int> priorityClasses = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 15, 16, 24, 25, 26, 28, 39, 41, 56, 62, 63, 67, 74};

    // ----------------------------------------------------------------
    // Multi-Tracker Settings
    // ----------------------------------------------------------------
    int   trackerMaxLostFrames    = 30;
    float trackerMinMatchIOU      = 0.25f;
    int   trackerMaxTrailLength   = 30;
    bool  showTrails              = true;
    float trackerMinMatchScore    = 0.15f;   // Greedy-Matching minimum combined score
    float trackerMaxCenterDistPx  = 200.0f;  // Max center distance for matching
    int   trackerConfirmFrames    = 2;        // Frames until a track is confirmed

    // Single-Tracker Settings (Target-Lock Feature)
    float trackerVelocitySmoothing    = 0.6f;
    float trackerDeadReckoningDamping = 0.9f;

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
    float    motionSensitivity       = 30.0f;  // MOG2 varThreshold: lower = more sensitive [5–100]
    int      motionMinArea           = 50;     // Minimum contour area in pixels to report [1–5000]
    int      motionBlurKernel        = 5;      // Gaussian pre-blur kernel size (1 = disabled, must be odd) [1–21]
    float    motionOverlayAlpha      = 0.35f;  // Fill transparency of motion overlay [0.0–1.0]
    uint32_t motionOverlayColor      = 0;      // RGBA color of motion overlay (initialised in HUD)
    bool     motionDetectShadows     = false;  // MOG2 shadow detection (reduces phantoms, costs ~15% perf)
    int      motionLearningRate      = -1;     // MOG2 learning rate: -1 = auto, 0–100 maps to [0.0–1.0]

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
};

#endif // COMMON_HPP

