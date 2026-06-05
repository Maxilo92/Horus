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
    int   class_id          = -1;
    std::string className;
    cv::Rect box;
    float confidence        = 0.0f;
    int   lost_frames       = 0;
};

struct SystemSettings {
    // Detector Settings
    float detectorConfThreshold  = 0.25f;
    float detectorScoreThreshold = 0.25f;
    float detectorNmsThreshold   = 0.45f;

    // Priority-Class-Filter
    // COCO IDs: 0=person, 1=bicycle, 2=car, 3=motorcycle, 5=bus, 7=truck
    bool filterByPriorityClasses = true;
    std::set<int> priorityClasses = {0, 1, 2, 3, 5, 7};

    // Multi-Tracker Settings
    int   trackerMaxLostFrames    = 30;
    float trackerMinMatchIOU      = 0.25f;   // Nur noch für SingleTracker; MOT nutzt hybriden Score
    int   trackerMaxTrailLength   = 30;
    bool  showTrails              = true;

    // Single-Tracker Settings (Target-Lock Feature)
    float trackerVelocitySmoothing    = 0.6f;
    float trackerDeadReckoningDamping = 0.9f;

    // HUD Settings
    bool showTacticalOverlay = true;
    bool showCrosshair       = true;
    bool showCornerBrackets  = true;
    bool showStatusWindows   = true;
    bool showDetections      = true;

    // uint32_t ist binärkompatibel mit ImU32 (= unsigned int in imgui)
    uint32_t hudColor    = 0;  // Wird in HUD initialisiert
    uint32_t targetColor = 0;
};

#endif // COMMON_HPP
