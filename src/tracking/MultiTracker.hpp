#ifndef MULTITRACKER_HPP
#define MULTITRACKER_HPP

#include "Common.hpp"
#include <opencv2/video/tracking.hpp>
#include <vector>
#include <map>

// Interner Zustand eines einzelnen Tracks
struct TrackState {
    int   track_id;
    int   class_id;
    std::string className;
    float confidence;
    int   lost_frames;
    int   matched_frames;   // Anzahl konsekutiver Matches (für Bestätigung)
    bool  is_confirmed;     // True nach mindestens 1 erfolgreichem Match

    cv::KalmanFilter kf;

    // Messbasierte Geschwindigkeit (px pro update()-Iteration), unabhängig vom
    // Kalman-Zustand. Wird ausschließlich für den Lag-Ausgleich verwendet:
    // Würde stattdessen die Kalman-Geschwindigkeit in die Messung extrapoliert,
    // entstünde eine Rückkopplungsschleife (Schätzung bestätigt sich selbst),
    // durch die Boxen bei stehenden Objekten wegdriften.
    cv::Point2d measVelocity{0.0, 0.0};
    cv::Point2d lastMeasCenter{0.0, 0.0};
    int  updatesSinceMeas = 0;   // update()-Iterationen seit letzter akzeptierter Messung
    bool hasMeasVelocity  = false;

    // Bewegungspfad (Centroid-History für Trail-Rendering)
    std::vector<cv::Point> trail;

    // Gibt die aktuelle Bounding Box aus dem Kalman-Zustand zurück
    cv::Rect getBoundingBox() const;
    // Gibt die vorhergesagte Bounding Box (statePre) zurück
    cv::Rect getPredictedBoundingBox() const;

    TrackState() = default;
    TrackState(int id, const Detection& det);

private:
    void initKalmanFilter(const cv::Rect& box);
};

// ============================================================
// Matching-Kosten-Typen
// ============================================================

// Hybrides Matching: IoU + normalisierte Zentrum-Distanz
// Erlaubt stabiles Tracking auch bei schneller Bewegung (IoU ~ 0)
struct MatchCost {
    float iou;            // [0, 1], höher = besser
    float normDist;       // [0, 1], 0=gleiche Position, 1=maxDist entfernt
    float score;          // Gesamt-Score: höher = besserer Match
};

// ============================================================
// Multi-Object-Tracker
// ============================================================

class MultiTracker {
public:
    MultiTracker();

    // Aktualisiert alle Tracks mit neuen Detektionen. Thread-safe nicht
    // (muss im Worker-Thread ausgeführt werden).
    // detectorRan: true when the YOLO detector produced these results this frame.
    // false when detections come from reacquisition only (detector still running).
    // When false and detections are empty, lost_frames is NOT incremented so tracks
    // don't disappear while the async detector is between frames.
    void update(const std::vector<Detection>& detections,
                const SystemSettings& settings,
                cv::Size frameSize,
                int lagFrames = 0,
                cv::Point2d cameraMotion = {0.0, 0.0},
                bool detectorRan = true);

    // Gibt alle aktiven Tracks als TrackedObject für das HUD zurück
    std::vector<TrackedObject> getTrackedObjects(int maxTrailLength) const;

    // Findet den nächstgelegenen Track zu einem Frame-Punkt (für Mausklick-Lock)
    const TrackState* findNearestTrack(cv::Point2f framePoint, float maxDistPx) const;

    // Findet einen spezifischen Track nach ID
    const TrackState* getTrackById(int id) const;

    int getActiveTrackCount() const;

    // Retrieves the next unique chronological ID and increments the counter
    int getNextIdAndIncrement() { return m_nextId++; }

private:
    // Berechnet für jedes Track-Detection-Paar einen Match-Score.
    // Gibt Matrix[nTracks][nDets] zurück.
    std::vector<std::vector<MatchCost>> computeCostMatrix(
        const std::vector<int>&      trackIds,
        const std::vector<cv::Rect>& predictedBoxes,
        const std::vector<Detection>& detections,
        float                        maxCenterDistPx,
        int                          lagFrames,
        float                        reacquisitionMaxDist) const;

    float calculateIoU(const cv::Rect& a, const cv::Rect& b) const;

    // Greedy Matching nach absteigendem Score
    void greedyMatch(
        const std::vector<std::vector<MatchCost>>& costMatrix,
        float minScore,
        std::vector<int>&  matchedTracks,  // [trackIdx] = detIdx, -1 = kein Match
        std::vector<bool>& matchedDets     // [detIdx] = true wenn zugewiesen
    ) const;

    std::map<int, TrackState> m_tracks;
    int                        m_nextId;
};

#endif // MULTITRACKER_HPP
