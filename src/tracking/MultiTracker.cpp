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
    cv::setIdentity(kf.processNoiseCov, cv::Scalar(1e-4));
    kf.processNoiseCov.at<double>(0, 0) = 0.01;
    kf.processNoiseCov.at<double>(1, 1) = 0.01;
    kf.processNoiseCov.at<double>(2, 2) = 0.01;
    kf.processNoiseCov.at<double>(3, 3) = 0.01;
    kf.processNoiseCov.at<double>(4, 4) = 1.0;  // vx – reduziert von 5.0 für mehr Stabilität
    kf.processNoiseCov.at<double>(5, 5) = 1.0;  // vy

    // Mess-Rauschen: Wie viel Ungenauigkeit hat der Detektor?
    cv::setIdentity(kf.measurementNoiseCov, cv::Scalar(2.0)); // Reduziert von 4.0

    // Initiale Fehler-Kovarianz: hohe Unsicherheit für Geschwindigkeit
    cv::setIdentity(kf.errorCovPost, cv::Scalar(1.0));
    kf.errorCovPost.at<double>(4, 4) = 10.0; // Reduziert von 100.0
    kf.errorCovPost.at<double>(5, 5) = 10.0;

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
                          int lagFrames,
                          cv::Point2d cameraMotion,
                          bool detectorRan)
{
    const int nDets = static_cast<int>(detections.size());
    const int safeLag = std::clamp(lagFrames, 0, 30); // 30 frames ≈ 1 s at 30 fps, caps extreme network jitter

    // --- Phase 1: Kalman-Predict für alle aktiven Tracks ---
    std::vector<int>     trackIds;
    std::vector<cv::Rect> predictedBoxes;
    trackIds.reserve(m_tracks.size());
    predictedBoxes.reserve(m_tracks.size());

    for (auto& [id, track] : m_tracks) {
        track.kf.predict();
        trackIds.push_back(id);
        predictedBoxes.push_back(track.getPredictedBoundingBox());
    }

    const int nTracks = static_cast<int>(trackIds.size());

    // --- Kamerabewegungskompensation (Ego-Motion) ---
    // Alle Track-Positionen werden um die inverse Kamerabewegung verschoben,
    // damit stationäre Objekte bei bewegter Kamera korrekt gematcht werden.
    // statePre  → für Matching (Phase 2)
    // statePost → für Coast-Track-Anzeige und Trail
    if ((cameraMotion.x != 0.0 || cameraMotion.y != 0.0) && nTracks > 0) {
        const int dx = static_cast<int>(std::round(cameraMotion.x));
        const int dy = static_cast<int>(std::round(cameraMotion.y));
        for (int i = 0; i < nTracks; ++i) {
            auto& track = m_tracks.at(trackIds[i]);
            track.kf.statePre.at<double>(0)  -= cameraMotion.x;
            track.kf.statePre.at<double>(1)  -= cameraMotion.y;
            track.kf.statePost.at<double>(0) -= cameraMotion.x;
            track.kf.statePost.at<double>(1) -= cameraMotion.y;
            predictedBoxes[i].x -= dx;
            predictedBoxes[i].y -= dy;
        }
    }

    // --- Phase 2 & 3: Matching — only when detector produced a result this frame.
    // When detectorRan==false and there are no detections from reacquisition either,
    // we skip matching entirely. Tracks coast via Kalman prediction and lost_frames
    // is NOT incremented, preventing boxes from disappearing between detector frames.
    const bool shouldMatch = detectorRan || (nDets > 0);

    const float maxCenterDist = settings.trackerMaxCenterDistPx;

    std::vector<int>  matchedTracks(nTracks, -1);
    std::vector<bool> matchedDets(nDets, false);

    if (shouldMatch && nTracks > 0 && nDets > 0) {
        auto costMatrix = computeCostMatrix(trackIds, predictedBoxes, detections,
                                            maxCenterDist, safeLag,
                                            settings.trackerReacquisitionMaxDist);
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
            // Plan 11 Safeguard: Recovery-Detektionen sind bereits Echtzeit
            if (safeLag > 0 && !det.is_recovery) {
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
            // Confirmed only after trackerConfirmFrames consecutive matches (prevents ephemeral flicker).
            if (!track.is_confirmed)
                track.is_confirmed = (track.matched_frames >= settings.trackerConfirmFrames);

            // Trail-Punkt (gemessene, extrapolierte Position)
            track.trail.emplace_back(extBox.x + extBox.width  / 2,
                                     extBox.y + extBox.height / 2);
        } else {
            // No match — either detector didn't run this frame (!shouldMatch) or
            // it ran but this track wasn't matched (lost).
            // Forward statePost to the Kalman prediction so the displayed box
            // continues to follow the predicted trajectory instead of freezing.
            // kf.predict() updates statePre and errorCovPre but does NOT update
            // statePost/errorCovPost. Without this, getBoundingBox() returns the
            // stale last-corrected position forever between detections.
            track.kf.statePost    = track.kf.statePre;
            track.kf.errorCovPost = track.kf.errorCovPre;

            if (shouldMatch) {
                // Detector ran but missed this track → count it as lost
                track.lost_frames++;
            }

            // Trail zeigt vorhergesagte Position (gedimmt im HUD)
            const cv::Rect pred = track.getBoundingBox(); // now reads updated statePost
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

            // Plan 11 Safeguard: Vermeidung von "Ghost"-Tracks durch Duplikate (z.B. bei Lag)
            // Prüfe, ob die Detektion bereits von einem aktiven Track abgedeckt wird.
            bool isDuplicate = false;
            for (const auto& [id, track] : m_tracks) {
                if (track.lost_frames == 0 && track.is_confirmed) {
                    float iou = calculateIoU(detections[di].box, track.getBoundingBox());
                    if (iou > 0.35f) { // suppress new track when overlap with confirmed track exceeds 35%
                        isDuplicate = true;
                        break;
                    }
                }
            }
            if (isDuplicate) continue;

            m_tracks.emplace(m_nextId, TrackState(m_nextId, detections[di]));
            // Creation itself counts as the first match.
            auto& newTrack = m_tracks.at(m_nextId);
            newTrack.matched_frames = 1;
            newTrack.is_confirmed   = (newTrack.matched_frames >= settings.trackerConfirmFrames);
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
        // Only show confirmed tracks; dead-reckoning is fine for confirmed+lost tracks.
        if (!track.is_confirmed) continue;

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
        if (!track.is_confirmed) continue;
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
    const std::vector<Detection>& detections,
    float                        maxCenterDistPx,
    int                          lagFrames,
    float                        reacquisitionMaxDist) const
{
    const int nT = static_cast<int>(predictedBoxes.size());
    const int nD = static_cast<int>(detections.size());

    std::vector<std::vector<MatchCost>> matrix(nT, std::vector<MatchCost>(nD, {0,1,0}));

    for (int ti = 0; ti < nT; ++ti) {
        const cv::Rect& pb = predictedBoxes[ti];
        const float pcx = static_cast<float>(pb.x + pb.width  / 2);
        const float pcy = static_cast<float>(pb.y + pb.height / 2);
        // Typische Objektgröße für adaptive Distanz-Schwelle
        const float objDiag = std::hypot(static_cast<float>(pb.width),
                                         static_cast<float>(pb.height));
        // Adaptiver maxDist: mindestens 80px (verhindert Fehlmatches bei kleinen Objekten),
        // maximal maxCenterDistPx; 1.5× Diagonale deckt typische Bewegung in einem Frame ab.
        float adaptiveDist = std::min(maxCenterDistPx,
                                      std::max(80.0f, objDiag * 1.5f));

        // Plan 11: Erhöhe Suchradius für verlorene Tracks
        const auto& trackRef = m_tracks.at(trackIds[ti]);
        if (trackRef.lost_frames > 0) {
            adaptiveDist *= reacquisitionMaxDist;
        }

        for (int di = 0; di < nD; ++di) {
            // Klassen-Konsistenz: andere Klasse = kein Match
            if (trackRef.class_id != detections[di].class_id) {
                matrix[ti][di] = {0.0f, 1.0f, 0.0f};
                continue;
            }

            cv::Rect db = detections[di].box;
            // Plan 11 Safeguard: Recovery-Detektionen sind bereits Echtzeit, kein Lag-Ausgleich nötig
            if (lagFrames > 0 && !detections[di].is_recovery) {
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

            // Gesamt-Score: IoU bevorzugt (geometrisch zuverlässiger);
            // Distanzanteil (Gewicht 0.65) greift als Fallback wenn IoU≈0
            // (z.B. schnelle Bewegung mit großem Sprung zwischen Frames).
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
