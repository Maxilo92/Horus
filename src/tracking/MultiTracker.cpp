#include "MultiTracker.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

// ============================================================
// TrackState – Initialisierung & Kalman-Filter
// ============================================================

TrackState::TrackState(int id, const Detection& det)
    : track_id(id), class_id(det.class_id), className(det.className),
      confidence(det.confidence), lost_frames(0), matched_frames(0),
      is_confirmed(false)
{
    initKalmanFilter(det.box);

    // Erster Centroid im Trail
    trail.emplace_back(det.box.x + det.box.width  / 2,
                       det.box.y + det.box.height / 2);
}

void TrackState::initKalmanFilter(const cv::Rect& box) {
    // State  [6]: cx, cy, w, h, vx, vy
    // Measure[4]: cx, cy, w, h
    kf.init(6, 4, 0, CV_64F);

    // Transitions-Matrix: konstante Geschwindigkeit (pos += vel pro Frame)
    cv::setIdentity(kf.transitionMatrix);
    kf.transitionMatrix.at<double>(0, 4) = 1.0; // cx = cx + vx
    kf.transitionMatrix.at<double>(1, 5) = 1.0; // cy = cy + vy

    // Messung: nur Position/Größe (keine Geschwindigkeit direkt messbar)
    kf.measurementMatrix = cv::Mat::zeros(4, 6, CV_64F);
    kf.measurementMatrix.at<double>(0, 0) = 1.0;
    kf.measurementMatrix.at<double>(1, 1) = 1.0;
    kf.measurementMatrix.at<double>(2, 2) = 1.0;
    kf.measurementMatrix.at<double>(3, 3) = 1.0;

    // Prozess-Rauschen: höherer Wert = Kalman folgt Messung schneller
    // Für w and h: kleine Varianz (0.01) – modelliert langsame Größenänderung
    cv::setIdentity(kf.processNoiseCov, cv::Scalar(1e-3));
    kf.processNoiseCov.at<double>(0, 0) = 0.01;
    kf.processNoiseCov.at<double>(1, 1) = 0.01;
    kf.processNoiseCov.at<double>(2, 2) = 0.01;
    kf.processNoiseCov.at<double>(3, 3) = 0.01;
    kf.processNoiseCov.at<double>(4, 4) = 5.0;  // vx – hohe Unsicherheit
    kf.processNoiseCov.at<double>(5, 5) = 5.0;  // vy

    // Mess-Rauschen: Wie viel Ungenauigkeit hat der Detektor?
    cv::setIdentity(kf.measurementNoiseCov, cv::Scalar(4.0));

    // Initiale Fehler-Kovarianz: hohe Unsicherheit für Geschwindigkeit
    cv::setIdentity(kf.errorCovPost, cv::Scalar(1.0));
    kf.errorCovPost.at<double>(4, 4) = 100.0;
    kf.errorCovPost.at<double>(5, 5) = 100.0;

    // Initialzustand
    const double cx = box.x + box.width  / 2.0;
    const double cy = box.y + box.height / 2.0;
    kf.statePost.at<double>(0) = cx;
    kf.statePost.at<double>(1) = cy;
    kf.statePost.at<double>(2) = static_cast<double>(box.width);
    kf.statePost.at<double>(3) = static_cast<double>(box.height);
    kf.statePost.at<double>(4) = 0.0;
    kf.statePost.at<double>(5) = 0.0;
}

cv::Rect TrackState::getBoundingBox() const {
    const cv::Mat& s = kf.statePost;
    if (s.empty()) return cv::Rect();

    const double cx = s.at<double>(0);
    const double cy = s.at<double>(1);
    const double w  = std::max(1.0, s.at<double>(2));
    const double h  = std::max(1.0, s.at<double>(3));

    return cv::Rect(
        static_cast<int>(std::round(cx - w / 2.0)),
        static_cast<int>(std::round(cy - h / 2.0)),
        static_cast<int>(std::round(w)),
        static_cast<int>(std::round(h))
    );
}

cv::Rect TrackState::getPredictedBoundingBox() const {
    const cv::Mat& s = kf.statePre;
    if (s.empty()) return cv::Rect();

    const double cx = s.at<double>(0);
    const double cy = s.at<double>(1);
    const double w  = std::max(1.0, s.at<double>(2));
    const double h  = std::max(1.0, s.at<double>(3));

    return cv::Rect(
        static_cast<int>(std::round(cx - w / 2.0)),
        static_cast<int>(std::round(cy - h / 2.0)),
        static_cast<int>(std::round(w)),
        static_cast<int>(std::round(h))
    );
}

// ============================================================
// MultiTracker – Haupt-Update
// ============================================================

MultiTracker::MultiTracker() : m_nextId(0) {}

void MultiTracker::update(const std::vector<Detection>& detections,
                          const SystemSettings& settings,
                          int lagFrames)
{
    const int nDets = static_cast<int>(detections.size());
    const int safeLag = std::clamp(lagFrames, 0, 30);

    // --- Phase 1: Kalman-Predict für alle aktiven Tracks ---
    std::vector<int>     trackIds;
    std::vector<cv::Rect> predictedBoxes;
    std::vector<int>     trackClassIds;
    trackIds.reserve(m_tracks.size());
    predictedBoxes.reserve(m_tracks.size());
    trackClassIds.reserve(m_tracks.size());

    for (auto& [id, track] : m_tracks) {
        track.kf.predict();
        trackIds.push_back(id);
        predictedBoxes.push_back(track.getPredictedBoundingBox());
        trackClassIds.push_back(track.class_id);
    }

    const int nTracks = static_cast<int>(trackIds.size());

    // Detektions-Klassen-IDs
    std::vector<int> detClassIds;
    detClassIds.reserve(nDets);
    std::vector<cv::Rect> detBoxes;
    detBoxes.reserve(nDets);
    for (const auto& det : detections) {
        detClassIds.push_back(det.class_id);
        detBoxes.push_back(det.box);
    }

    // --- Phase 2: Hybride Cost-Matrix berechnen ---
    // Max. erlaubte Zentrum-Distanz für ein Match:
    // Skaliert mit der typischen Objektgröße, mind. 150px.
    // Erlaubt schnell-bewegten Objekten dennoch gematcht zu werden.
    const float maxCenterDist = settings.trackerMaxCenterDistPx; // Frame-Pixel

    std::vector<int>  matchedTracks(nTracks, -1);
    std::vector<bool> matchedDets(nDets, false);

    if (nTracks > 0 && nDets > 0) {
        auto costMatrix = computeCostMatrix(trackIds, predictedBoxes, detBoxes,
                                            trackClassIds, detClassIds,
                                            maxCenterDist, safeLag,
                                            settings.trackerReacquisitionMaxDist);
        // Mindest-Score für ein gültiges Match:
        // IoU-only wäre 0.25, aber durch hybrides Scoring auch bei IoU=0 möglich
        const float minScore = settings.trackerMinMatchScore;
        greedyMatch(costMatrix, minScore, matchedTracks, matchedDets);
    }

    // --- Phase 3: Matched Tracks updaten ---
    for (int ti = 0; ti < nTracks; ++ti) {
        auto& track = m_tracks.at(trackIds[ti]);
        const int did = matchedTracks[ti];

        if (did >= 0) {
            // Kalman correct()
            const Detection& det = detections[did];
            
            // Extrapolate the detection coordinates using the track's estimated velocity
            cv::Rect extBox = det.box;
            if (safeLag > 0) {
                double vx = track.kf.statePost.at<double>(4);
                double vy = track.kf.statePost.at<double>(5);
                extBox.x += static_cast<int>(std::round(safeLag * vx));
                extBox.y += static_cast<int>(std::round(safeLag * vy));
            }

            cv::Mat measurement(4, 1, CV_64F);
            measurement.at<double>(0) = extBox.x + extBox.width  / 2.0;
            measurement.at<double>(1) = extBox.y + extBox.height / 2.0;
            measurement.at<double>(2) = static_cast<double>(extBox.width);
            measurement.at<double>(3) = static_cast<double>(extBox.height);
            track.kf.correct(measurement);

            track.confidence    = det.confidence;
            track.lost_frames   = 0;
            track.matched_frames++;
            // Bestätigt nach 1 erfolgreichem Match (sofortige Anzeige)
            track.is_confirmed  = true;

            // Trail-Punkt (gemessene, extrapolierte Position)
            track.trail.emplace_back(extBox.x + extBox.width  / 2,
                                     extBox.y + extBox.height / 2);
        } else {
            // Kein Match → Dead Reckoning via Kalman-Prediction
            track.lost_frames++;

            // Trail zeigt vorhergesagte Position (gedimmt im HUD)
            const cv::Rect pred = track.getBoundingBox();
            track.trail.emplace_back(pred.x + pred.width  / 2,
                                     pred.y + pred.height / 2);
        }

        // Trail-Länge begrenzen
        const int maxLen = settings.trackerMaxTrailLength;
        if (maxLen > 0) {
            while (static_cast<int>(track.trail.size()) > maxLen)
                track.trail.erase(track.trail.begin());
        }
    }

    // --- Phase 4: Neue Tracks für ungematchte Detections ---
    for (int di = 0; di < nDets; ++di) {
        if (!matchedDets[di]) {
            // Plan 11 Safeguard: Recovery-Detektionen (Zoom/Motion) dürfen KEINE neuen Tracks erzeugen
            if (detections[di].is_recovery) continue;

            m_tracks.emplace(m_nextId, TrackState(m_nextId, detections[di]));
            ++m_nextId;
        }
    }

    // --- Phase 5: Veraltete Tracks entfernen ---
    int maxLost = settings.trackerMaxLostFrames;
    if (settings.remoteInferenceEnabled) {
        maxLost = std::max(maxLost, 90); // Tolerates up to 3 seconds of network/asynchronous lag
    }

    for (auto it = m_tracks.begin(); it != m_tracks.end(); ) {
        if (it->second.lost_frames > maxLost)
            it = m_tracks.erase(it);
        else
            ++it;
    }
}

// ============================================================
// Ergebnisse für HUD
// ============================================================

std::vector<TrackedObject> MultiTracker::getTrackedObjects(int maxTrailLength) const {
    std::vector<TrackedObject> result;
    result.reserve(m_tracks.size());

    for (const auto& [id, track] : m_tracks) {
        // Optimierung: Zeige Tracks sofort, wenn sie frisch sind (lost_frames == 0),
        // auch wenn sie noch nicht "bestätigt" (is_confirmed) sind.
        if (!track.is_confirmed && track.lost_frames > 0) continue;

        TrackedObject obj;
        obj.track_id    = track.track_id;
        obj.class_id    = track.class_id;
        obj.className   = track.className;
        obj.box         = track.getBoundingBox();
        obj.confidence  = track.confidence;
        obj.lost_frames = track.lost_frames;
        obj.is_active   = (track.lost_frames == 0);
        obj.is_confirmed = track.is_confirmed;

        // Trail kopieren
        const int trailSize = static_cast<int>(track.trail.size());
        const int startIdx  = std::max(0, trailSize - maxTrailLength);
        obj.trail.assign(track.trail.begin() + startIdx, track.trail.end());

        result.push_back(obj);
    }
    return result;
}

const TrackState* MultiTracker::findNearestTrack(cv::Point2f point, float maxDistPx) const {
    const TrackState* nearest = nullptr;
    float minDist = maxDistPx;

    for (const auto& [id, track] : m_tracks) {
        if (!track.is_confirmed && track.lost_frames > 0) continue;
        const cv::Rect box = track.getBoundingBox();
        const float cx   = static_cast<float>(box.x + box.width  / 2);
        const float cy   = static_cast<float>(box.y + box.height / 2);
        const float dist = std::hypot(point.x - cx, point.y - cy);
        if (dist < minDist) {
            minDist = dist;
            nearest = &track;
        }
    }
    return nearest;
}

const TrackState* MultiTracker::getTrackById(int id) const {
    auto it = m_tracks.find(id);
    if (it != m_tracks.end()) return &(it->second);
    return nullptr;
}

int MultiTracker::getActiveTrackCount() const {
    int count = 0;
    for (const auto& [id, track] : m_tracks)
        if (track.lost_frames == 0) ++count;
    return count;
}

// ============================================================
// Hybride Cost-Matrix
// ============================================================
//
// Score für jedes (Track, Detection)-Paar:
//
//   score = max(iou_score, dist_score)   falls gleiche Klasse
//         = 0                             falls verschiedene Klasse
//
// iou_score  = IoU ∈ [0, 1]
// dist_score = max(0, 1 - dist / maxCenterDist)^2   ∈ [0, 1]
//
// Vorteil: Bei schneller Bewegung (IoU ≈ 0, dist < maxCenterDist)
//          gibt dist_score noch einen ausreichend hohen Score für ein Match.
//
std::vector<std::vector<MatchCost>> MultiTracker::computeCostMatrix(
    const std::vector<int>&      trackIds,
    const std::vector<cv::Rect>& predictedBoxes,
    const std::vector<cv::Rect>& detBoxes,
    const std::vector<int>&      trackClassIds,
    const std::vector<int>&      detClassIds,
    float                        maxCenterDistPx,
    int                          lagFrames,
    float                        reacquisitionMaxDist) const
{
    const int nT = static_cast<int>(predictedBoxes.size());
    const int nD = static_cast<int>(detBoxes.size());

    std::vector<std::vector<MatchCost>> matrix(nT, std::vector<MatchCost>(nD, {0,1,0}));

    for (int ti = 0; ti < nT; ++ti) {
        const cv::Rect& pb = predictedBoxes[ti];
        const float pcx = static_cast<float>(pb.x + pb.width  / 2);
        const float pcy = static_cast<float>(pb.y + pb.height / 2);
        // Typische Objektgröße für adaptive Distanz-Schwelle
        const float objDiag = std::hypot(static_cast<float>(pb.width),
                                         static_cast<float>(pb.height));
        // Adaptiver maxDist: mindestens 80px, maximal maxCenterDistPx
        float adaptiveDist = std::min(maxCenterDistPx,
                                      std::max(80.0f, objDiag * 1.5f));

        // Plan 11: Erhöhe Suchradius für verlorene Tracks
        const auto& trackRef = m_tracks.at(trackIds[ti]);
        if (trackRef.lost_frames > 0) {
            adaptiveDist *= reacquisitionMaxDist;
        }

        for (int di = 0; di < nD; ++di) {
            // Klassen-Konsistenz: andere Klasse = kein Match
            if (trackClassIds[ti] != detClassIds[di]) {
                matrix[ti][di] = {0.0f, 1.0f, 0.0f};
                continue;
            }

            cv::Rect db = detBoxes[di];
            if (lagFrames > 0) {
                const auto& track = m_tracks.at(trackIds[ti]);
                double vx = track.kf.statePost.at<double>(4);
                double vy = track.kf.statePost.at<double>(5);
                db.x += static_cast<int>(std::round(lagFrames * vx));
                db.y += static_cast<int>(std::round(lagFrames * vy));
            }
            const float dcx = static_cast<float>(db.x + db.width  / 2);
            const float dcy = static_cast<float>(db.y + db.height / 2);

            // IoU-Score
            const float iou = calculateIoU(pb, db);

            // Normalisierte Zentrum-Distanz [0, 1]
            const float dist = std::hypot(pcx - dcx, pcy - dcy);
            const float normDist = std::min(1.0f, dist / adaptiveDist);

            // Distanz-Score: quadratischer Abfall mit Distanz
            // Gibt bei dist=0 → 1.0, bei dist=adaptiveDist → 0.0
            const float distScore = (1.0f - normDist) * (1.0f - normDist);

            // Gesamt-Score: gewichtete Kombination
            // IoU wird bevorzugt (zuverlässiger), Distanz als Fallback
            // Formel: max(iou, 0.6 * distScore) – bei IoU=0 greift Distanz
            const float score = std::max(iou, 0.65f * distScore);

            matrix[ti][di] = {iou, normDist, score};
        }
    }
    return matrix;
}

float MultiTracker::calculateIoU(const cv::Rect& a, const cv::Rect& b) const {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width,  b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);

    if (x1 >= x2 || y1 >= y2) return 0.0f;

    const float inter = static_cast<float>((x2 - x1) * (y2 - y1));
    const float aArea = static_cast<float>(a.width * a.height);
    const float bArea = static_cast<float>(b.width * b.height);
    return inter / (aArea + bArea - inter);
}

// ============================================================
// Greedy Matching (absteigend nach Score)
// ============================================================

void MultiTracker::greedyMatch(
    const std::vector<std::vector<MatchCost>>& costMatrix,
    float minScore,
    std::vector<int>&  matchedTracks,
    std::vector<bool>& matchedDets) const
{
    const int nT = static_cast<int>(costMatrix.size());
    if (nT == 0) return;
    const int nD = static_cast<int>(costMatrix[0].size());

    struct Candidate { float score; int ti, di; };
    std::vector<Candidate> candidates;
    candidates.reserve(nT * nD);

    for (int ti = 0; ti < nT; ++ti)
        for (int di = 0; di < nD; ++di)
            if (costMatrix[ti][di].score >= minScore)
                candidates.push_back({costMatrix[ti][di].score, ti, di});

    // Bestes Paar zuerst
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    for (const auto& c : candidates) {
        if (matchedTracks[c.ti] == -1 && !matchedDets[c.di]) {
            matchedTracks[c.ti] = c.di;
            matchedDets[c.di]   = true;
        }
    }
}
