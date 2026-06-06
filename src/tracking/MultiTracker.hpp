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
    void update(const std::vector<Detection>& detections,
                const SystemSettings& settings,
                int lagFrames = 0);

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
        const std::vector<cv::Rect>& detBoxes,
        const std::vector<int>&      trackClassIds,
        const std::vector<int>&      detClassIds,
        float                        maxCenterDistPx,
        int                          lagFrames) const;

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
