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
};

struct SystemSettings {
    // ----------------------------------------------------------------
    // Detector Settings
    // ----------------------------------------------------------------
    float detectorConfThreshold  = 0.25f;
    float detectorScoreThreshold = 0.25f;
    float detectorNmsThreshold   = 0.45f;

    // Priority-Class-Filter
    // COCO IDs: 0=person, 1=bicycle, 2=car, 3=motorcycle, 5=bus, 7=truck
    bool filterByPriorityClasses = true;
    std::set<int> priorityClasses = {0, 1, 2, 3, 5, 7};

    // ----------------------------------------------------------------
    // Multi-Tracker Settings
    // ----------------------------------------------------------------
    int   trackerMaxLostFrames    = 30;
    float trackerMinMatchIOU      = 0.25f;
    int   trackerMaxTrailLength   = 30;
    bool  showTrails              = true;
    float trackerMinMatchScore    = 0.30f;   // Greedy-Matching minimum combined score
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
};

#endif // COMMON_HPP
