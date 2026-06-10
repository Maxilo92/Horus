#include "TrackingSystem.hpp"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <ctime>
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

// Returns a quality score in [0, 1] for a snapshot crop.
// Combines sharpness (Laplacian variance), detection confidence, and
// whether the bounding box is fully inside the frame.
static float computeSnapQuality(const cv::Mat& crop, float confidence,
                                 const cv::Rect& box, cv::Size frameSize) {
    if (crop.empty()) return 0.0f;

    // Sharpness: variance of the Laplacian on a grayscale crop.
    // 200 = "clearly sharp" reference point; capped at 1.0.
    cv::Mat gray;
    if (crop.channels() == 3) cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
    else gray = crop;
    cv::Mat lap;
    cv::Laplacian(gray, lap, CV_64F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(lap, mean, stddev);
    float sharpness = std::min(1.0f, static_cast<float>(stddev[0] * stddev[0]) / 200.0f);

    // Completeness: full score if box doesn't touch any frame edge.
    constexpr int kEdgeMargin = 3;
    bool clipped = (box.x <= kEdgeMargin || box.y <= kEdgeMargin ||
                    box.x + box.width  >= frameSize.width  - kEdgeMargin ||
                    box.y + box.height >= frameSize.height - kEdgeMargin);
    float completeness = clipped ? 0.75f : 1.0f;

    return sharpness * (0.7f + 0.3f * confidence) * completeness;
}

TrackingSystem::TrackingSystem(Blackboard& blackboard, ROIManager& roiManager,
                               DataLogger& dataLogger, AudioEngine& audioEngine,
                               LogFn logFn)
    : m_blackboard(blackboard),
      m_roiManager(roiManager),
      m_dataLogger(dataLogger),
      m_audioEngine(audioEngine),
      m_log(std::move(logFn)) {
    m_tracker = std::make_unique<MultiTracker>();
}

TrackingSystem::~TrackingSystem() {
    stop();
}

bool TrackingSystem::init(const std::string& faceDetPath, const std::string& faceRecPath,
                          DossierDatabase* dossierDb) {
    m_dossierDb = dossierDb;

    SystemSettings settings = m_blackboard.getSettings();
    if (settings.faceRecognitionEnabled && !faceDetPath.empty() && !faceRecPath.empty()) {
        m_log(LogLevel::INFO, "[FaceRecognizer] Loading detector: " + faceDetPath);
        m_log(LogLevel::INFO, "[FaceRecognizer] Loading recognizer: " + faceRecPath);
        try {
            m_faceRecognizer = std::make_unique<FaceRecognizer>(faceDetPath, faceRecPath);
            m_log(LogLevel::INFO, "[FaceRecognizer] Init OK");
        } catch (const std::exception& e) {
            m_log(LogLevel::ERR, std::string("[TrackingSystem] Face recognizer init failed: ") + e.what());
            return false;
        }

        // Load persisted face identities and wire up persistence.
        int seededCount = 0;
        if (m_dossierDb) {
            std::vector<FaceIdentity> seeded;
            for (const auto& rec : m_dossierDb->getAllFaces()) {
                if (rec.embedding.empty()) continue;
                FaceIdentity fi;
                fi.id = rec.id;
                fi.name = rec.name;
                fi.embedding = cv::Mat(1, static_cast<int>(rec.embedding.size()), CV_32F,
                                       const_cast<float*>(rec.embedding.data())).clone();
                seeded.push_back(std::move(fi));
            }
            seededCount = static_cast<int>(seeded.size());
            m_faceRecognizer->seedIdentities(seeded, m_dossierDb->getMaxFaceId() + 1);
            m_log(LogLevel::INFO, "[TrackingSystem] Loaded " + std::to_string(seededCount) +
                                  " face identities from database");

            DossierDatabase* db = m_dossierDb;
            m_faceRecognizer->setPersistCallback([db](const FaceIdentity& id) {
                std::vector<float> emb;
                if (!id.embedding.empty()) {
                    cv::Mat flat = id.embedding.reshape(1, 1);
                    flat.convertTo(flat, CV_32F);
                    emb.assign(flat.ptr<float>(0), flat.ptr<float>(0) + flat.cols);
                }
                db->upsertFace(id.id, id.name, emb);
            });
        }

        // Publish init state for the debug console.
        FaceDebugState dbg;
        dbg.initOk        = true;
        dbg.detPath       = faceDetPath;
        dbg.recPath       = faceRecPath;
        dbg.identityCount = seededCount;
        m_blackboard.setFaceDebugState(dbg);
    }
    return true;
}

void TrackingSystem::start() {
    if (m_running.load()) return;
    m_running = true;
    m_trackingThread = std::thread(&TrackingSystem::trackingWorkerLoop, this);
}

void TrackingSystem::stop() {
    m_running = false;
    m_trackingCv.notify_all();
    if (m_trackingThread.joinable()) m_trackingThread.join();
}

void TrackingSystem::update() {
    // No-op: all logic is in trackingWorkerLoop
}

void TrackingSystem::submitFrame(const cv::Mat& frame, const SystemSettings& settings,
                                  const std::vector<Detection>& detections, int detectorLag,
                                  uint64_t sessionMs, const std::vector<MotionTrack>& motionTracks,
                                  bool hasNewDetections) {
    if (m_trackingBusy.load()) return;
    std::lock_guard<std::mutex> lock(m_trackingMutex);
    // Shallow safe: jeder Frame lebt in einem eigenen, nach Publish unveränderlichen
    // Puffer (Capture released seinen Header nach dem Publish).
    m_trackingFrameCopy = frame;
    m_trackingSettingsCopy         = settings;
    m_trackingDetectionsCopy       = detections;
    m_trackingDetectorLag          = detectorLag;
    m_trackingSessionMs            = sessionMs;
    m_trackingMotionTracksCopy     = motionTracks;
    m_trackingHasNewDetections     = hasNewDetections;
    m_trackingBusy                 = true;
    m_trackingCv.notify_one();
}

void TrackingSystem::trackingWorkerLoop() {
    m_log(LogLevel::INFO, "Tracking thread started");
#ifdef __APPLE__
    // Tracking darf UI und Kamera-Feed nie verdrängen — bei CPU-Sättigung
    // bekommen die interaktiven Threads die Kerne zuerst.
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
    while (m_running) {
        cv::Mat trackingFrame;
        SystemSettings settings;
        std::vector<Detection> detections;
        int detectorLag = 0;
        uint64_t sessionMs = 0;
        std::vector<MotionTrack> motionTracks;
        bool hasNewDetections = true;

        {
            std::unique_lock<std::mutex> lock(m_trackingMutex);
            m_trackingCv.wait(lock, [this]() {
                return !m_running || m_trackingBusy.load();
            });
            if (!m_running) break;

            trackingFrame     = m_trackingFrameCopy;
            settings          = m_trackingSettingsCopy;
            detections        = m_trackingDetectionsCopy;
            detectorLag       = m_trackingDetectorLag;
            sessionMs         = m_trackingSessionMs;
            motionTracks      = m_trackingMotionTracksCopy;
            hasNewDetections  = m_trackingHasNewDetections;
        }

        if (trackingFrame.empty()) {
            m_trackingBusy = false;
            continue;
        }

        UICommandState cmd = m_blackboard.consumeTrackingCommands();
        int manualCaptureId = cmd.manualCaptureTargetId;

        // Handle face rename requests from the UI (Akte/Analyzer).
        if (cmd.renameFaceId != -1 && m_faceRecognizer) {
            if (m_faceRecognizer->renameIdentity(cmd.renameFaceId, cmd.renameFaceName)) {
                std::lock_guard<std::mutex> lock(m_faceMutex);
                for (auto& kv : m_trackIdToFace) {
                    if (kv.second.face_id == cmd.renameFaceId)
                        kv.second.face_name = cmd.renameFaceName;
                }
            }
        }

        std::vector<TrackedObject> tracked;
        auto trackStart = std::chrono::high_resolution_clock::now();
        try {
            handleLockCommands(cmd);
            handlePixelLockCommands(cmd, trackingFrame);

            if (settings.enableTracking) {
                runReacquisition(detections, trackingFrame, settings, motionTracks);
                
                // Plan 04: Filter detections through ROI zones (Inclusion/Exclusion)
                m_roiManager.filterDetections(detections);
                
                deduplicateDetections(detections);

                cv::Point2d cameraMotion = estimateCameraMotion(trackingFrame, settings);
                updateTrackerAndFaces(tracked, trackingFrame, detections, settings,
                                      detectorLag, cameraMotion, hasNewDetections);

                computeTargetPriorities(tracked, settings, trackingFrame.size());

                updateTargetHistory(tracked, trackingFrame, manualCaptureId);
                runPixelTemplateMatch(tracked, trackingFrame, cmd, settings);
                updateLockedTargetYolo(tracked);
                updateAudioFeedback(settings);
                checkAlarmZones(tracked);
                logTrackingData(tracked, motionTracks, settings, sessionMs);
            }
        } catch (const std::exception& e) {
            m_log(LogLevel::ERR, std::string("[TrackingSystem] Tracking failed: ") + e.what());
        }

        auto trackEnd = std::chrono::high_resolution_clock::now();
        float trackMs = std::chrono::duration<float, std::milli>(trackEnd - trackStart).count();

        TrackingStateData tState;
        tState.activeTracks    = tracked;
        tState.lockedTarget    = m_sharedLockedTarget;
        tState.targetHistory   = m_internalTargetHistory;
        tState.pixelLockActive = m_pixelLockActive;
        m_blackboard.setTrackingState(tState);

        m_blackboard.updateStatusCounts(-1, static_cast<int>(tracked.size()), 0);

        PerformanceMetrics perf = m_blackboard.getPerformanceMetrics();
        perf.trackingTimeMs  = trackMs;
        perf.dataLoggerQueue = 0;
        m_blackboard.setPerformanceMetrics(perf);

        m_trackingBusy = false;
    }
    m_log(LogLevel::INFO, "Tracking thread stopping");
}

// -----------------------------------------------------------------------
// Section 1 – external lock / unlock commands
// -----------------------------------------------------------------------

void TrackingSystem::handleLockCommands(const UICommandState& cmd) {
    if (cmd.lockRequested) {
        m_sharedLockedTarget.track_id = cmd.requestedLockId;
        m_sharedLockedTarget.state    = TrackingState::LOCKED;
        m_pixelLockActive = false;
    }
    if (cmd.releaseLockRequested) {
        m_sharedLockedTarget.state    = TrackingState::SEARCHING;
        m_sharedLockedTarget.track_id = -1;
        m_pixelLockActive = false;
        m_audioEngine.stopLockPulse();
        m_lockStableFrames = 0;
    }
}

// -----------------------------------------------------------------------
// Sections 2 + 3 – pixel lock acquisition and rect update
// -----------------------------------------------------------------------

void TrackingSystem::handlePixelLockCommands(const UICommandState& cmd, const cv::Mat& frame) {
    if (cmd.pixelLockRequested) {
        cv::Rect tr = cmd.pixelLockRect;
        int x1 = std::max(0, std::min(tr.x, frame.cols - 1));
        int y1 = std::max(0, std::min(tr.y, frame.rows - 1));
        int x2 = std::max(0, std::min(tr.x + tr.width,  frame.cols));
        int y2 = std::max(0, std::min(tr.y + tr.height, frame.rows));
        tr = cv::Rect(x1, y1, x2 - x1, y2 - y1);

        if (tr.width > 4 && tr.height > 4) {
            m_pixelTemplate  = frame(tr).clone();
            m_pixelLockActive = true;
            m_pixelVx = m_pixelVy = 0.0f;
            m_pixelCenterX = static_cast<float>(tr.x + tr.width  / 2.0f);
            m_pixelCenterY = static_cast<float>(tr.y + tr.height / 2.0f);
            m_pixelLockRect = tr;
            m_sharedLockedTarget.state      = TrackingState::LOCKED;
            m_sharedLockedTarget.track_id   = m_tracker->getNextIdAndIncrement();
            m_sharedLockedTarget.class_id   = -1;
            m_sharedLockedTarget.className  = "Pixel Target";
            m_sharedLockedTarget.box        = tr;
            m_sharedLockedTarget.confidence = 1.0f;
            m_sharedLockedTarget.lost_frames = 0;
            m_sharedLockedTarget.trail.clear();
            m_sharedLockedTarget.trail.push_back(
                cv::Point(static_cast<int>(m_pixelCenterX), static_cast<int>(m_pixelCenterY)));
            m_audioEngine.playLockAcquired();
            m_log(LogLevel::INFO, "Pixel target locked via selection: (" +
                  std::to_string(tr.x) + ", " + std::to_string(tr.y) + ", " +
                  std::to_string(tr.width) + "x" + std::to_string(tr.height) + ")");
        }
    }

    if (cmd.pixelLockRectUpdateRequested) {
        cv::Rect rect = cmd.pixelLockRect;
        int x1 = std::max(0, std::min(rect.x, frame.cols - 1));
        int y1 = std::max(0, std::min(rect.y, frame.rows - 1));
        int x2 = std::max(0, std::min(rect.x + rect.width,  frame.cols));
        int y2 = std::max(0, std::min(rect.y + rect.height, frame.rows));
        rect = cv::Rect(x1, y1, x2 - x1, y2 - y1);

        if (rect.width > 4 && rect.height > 4) {
            m_pixelTemplate   = frame(rect).clone();
            m_pixelLockActive = true;
            m_pixelVx = m_pixelVy = 0.0f;
            m_pixelCenterX = static_cast<float>(rect.x + rect.width  / 2.0f);
            m_pixelCenterY = static_cast<float>(rect.y + rect.height / 2.0f);
            m_pixelLockRect = rect;
            m_sharedLockedTarget.state = TrackingState::LOCKED;
            if (m_sharedLockedTarget.track_id < 0)
                m_sharedLockedTarget.track_id = m_tracker->getNextIdAndIncrement();
            m_sharedLockedTarget.box         = rect;
            m_sharedLockedTarget.confidence  = 1.0f;
            m_sharedLockedTarget.lost_frames = 0;
            m_sharedLockedTarget.trail.clear();
            m_sharedLockedTarget.trail.push_back(
                cv::Point(static_cast<int>(m_pixelCenterX), static_cast<int>(m_pixelCenterY)));
        }
    }
}

// -----------------------------------------------------------------------
// Section 4 – high-sensitivity re-acquisition
// -----------------------------------------------------------------------

void TrackingSystem::runReacquisition(std::vector<Detection>& detections,
                                       const cv::Mat& frame,
                                       const SystemSettings& settings,
                                       const std::vector<MotionTrack>& motionTracks) {
    if (!settings.trackerReacquisitionEnabled && !settings.trackerUseMotionFallback) return;

    std::vector<TrackedObject> currentTracks = m_tracker->getTrackedObjects(1);
    for (const auto& track : currentTracks) {
        if (track.lost_frames <= 0 || track.lost_frames >= settings.trackerMaxLostFrames) continue;

        bool foundInROI = false;

        if (settings.trackerReacquisitionEnabled && m_detector) {
            float zoom = std::max(1.1f, settings.trackerReacquisitionZoom);
            int roiW = static_cast<int>(track.box.width  * zoom);
            int roiH = static_cast<int>(track.box.height * zoom);
            int roiX = track.box.x + track.box.width  / 2 - roiW / 2;
            int roiY = track.box.y + track.box.height / 2 - roiH / 2;

            int x1 = std::max(0, roiX);
            int y1 = std::max(0, roiY);
            int x2 = std::min(frame.cols, roiX + roiW);
            int y2 = std::min(frame.rows, roiY + roiH);

            if (x2 - x1 > 10 && y2 - y1 > 10) {
                cv::Rect roi(x1, y1, x2 - x1, y2 - y1);
                cv::Mat roiFrame = frame(roi).clone();
                std::vector<Detection> roiDets = m_detector->detect(roiFrame, settings);

                float bestConf = 0.0f;
                Detection bestDet;
                bool bestFound = false;
                for (auto& d : roiDets) {
                    if (d.class_id == track.class_id && d.confidence > bestConf) {
                        bestConf  = d.confidence;
                        bestDet   = d;
                        bestFound = true;
                    }
                }
                if (bestFound) {
                    bestDet.box.x += x1;
                    bestDet.box.y += y1;
                    bestDet.is_recovery = true;
                    detections.push_back(bestDet);
                    foundInROI = true;
                }
            }
        }

        if (!foundInROI && settings.trackerUseMotionFallback) {
            for (const auto& mt : motionTracks) {
                cv::Rect intersect = track.box & mt.box;
                if (intersect.area() > 0) {
                    float iou = static_cast<float>(intersect.area()) /
                                (track.box.area() + mt.box.area() - intersect.area());
                    if (iou > 0.15f || intersect.area() > track.box.area() * 0.5f) {
                        Detection d;
                        d.class_id    = track.class_id;
                        d.className   = track.className + " (Motion)";
                        d.confidence  = 0.5f;
                        d.box         = mt.box;
                        d.is_recovery = true;
                        detections.push_back(d);
                        break;
                    }
                }
            }
        }
    }
}

// -----------------------------------------------------------------------
// Section 5 – global deduplication NMS
// -----------------------------------------------------------------------

void TrackingSystem::deduplicateDetections(std::vector<Detection>& detections) {
    if (detections.empty()) return;

    std::vector<cv::Rect> boxes;
    std::vector<float>    confs;
    boxes.reserve(detections.size());
    confs.reserve(detections.size());
    for (const auto& d : detections) {
        boxes.push_back(d.box);
        confs.push_back(d.confidence);
    }
    std::vector<int> idx;
    cv::dnn::NMSBoxes(boxes, confs, 0.05f, 0.3f, idx);
    std::vector<Detection> unique;
    unique.reserve(idx.size());
    for (int i : idx) unique.push_back(detections[i]);
    detections = std::move(unique);
}

// -----------------------------------------------------------------------
// Section 6 – camera motion estimation (Ego-Motion via Sparse Optical Flow)
// -----------------------------------------------------------------------

cv::Point2d TrackingSystem::estimateCameraMotion(const cv::Mat& frame,
                                                  const SystemSettings& settings) {
    cv::Point2d motion(0.0, 0.0);
    if (!settings.enableCameraMotionComp || frame.empty()) return motion;

    cv::Mat grayFrame;
    if (frame.channels() == 1)
        grayFrame = frame;
    else
        cv::cvtColor(frame, grayFrame, cv::COLOR_BGR2GRAY);

    if (!m_prevFrameGray.empty() && m_prevFrameGray.size() == grayFrame.size()) {
        // Exclude tracked-object bounding boxes so only static scene features are used
        cv::Mat mask(grayFrame.size(), CV_8U, cv::Scalar(255));
        for (const auto& obj : m_tracker->getTrackedObjects(1)) {
            cv::Rect safe(
                std::max(0, obj.box.x - 4),
                std::max(0, obj.box.y - 4),
                std::min(obj.box.width  + 8, grayFrame.cols - std::max(0, obj.box.x - 4)),
                std::min(obj.box.height + 8, grayFrame.rows - std::max(0, obj.box.y - 4))
            );
            if (safe.width > 0 && safe.height > 0)
                cv::rectangle(mask, safe, cv::Scalar(0), -1);
        }

        std::vector<cv::Point2f> prevPts;
        cv::goodFeaturesToTrack(m_prevFrameGray, prevPts, 300, 0.01, 7, mask);

        if (prevPts.size() >= 6) {
            std::vector<cv::Point2f> currPts;
            std::vector<uchar> status;
            std::vector<float> err;
            cv::calcOpticalFlowPyrLK(m_prevFrameGray, grayFrame,
                                      prevPts, currPts, status, err,
                                      cv::Size(21, 21), 3);

            std::vector<cv::Point2f> goodPrev, goodCurr;
            for (size_t i = 0; i < status.size(); ++i) {
                if (status[i]) {
                    goodPrev.push_back(prevPts[i]);
                    goodCurr.push_back(currPts[i]);
                }
            }

            if (goodPrev.size() >= 6) {
                // Partial affine (Translation + Rotation + uniform Scale), RANSAC filters outliers
                cv::Mat inliers;
                cv::Mat affine = cv::estimateAffinePartial2D(
                    goodPrev, goodCurr, inliers, cv::RANSAC, 3.0);

                // Inlier gating: a trustworthy global motion needs a clear consensus of
                // static features. Otherwise (foreground motion, low texture) we self-disable
                // the compensation rather than shoving every track by a bad estimate.
                const int inlierCount = cv::countNonZero(inliers);
                const double inlierRatio = goodPrev.empty() ? 0.0
                    : static_cast<double>(inlierCount) / static_cast<double>(goodPrev.size());

                if (!affine.empty() && inlierCount >= 12 && inlierRatio >= 0.40) {
                    const double dx = affine.at<double>(0, 2);
                    const double dy = affine.at<double>(1, 2);
                    // Plausibility guard: a real pan rarely moves the scene more than
                    // ~8 % of frame width per frame at 30 fps.
                    const double maxShift = 0.08 * frame.cols;
                    if (std::abs(dx) <= maxShift && std::abs(dy) <= maxShift) {
                        motion.x = dx;
                        motion.y = dy;
                    }
                }
            }
        }
    }
    m_prevFrameGray = grayFrame.clone();

    // Temporal smoothing dampens single-frame outliers before the shift hits every track.
    motion.x = 0.6 * motion.x + 0.4 * m_prevCameraMotion.x;
    motion.y = 0.6 * motion.y + 0.4 * m_prevCameraMotion.y;
    m_prevCameraMotion = motion;
    return motion;
}

// -----------------------------------------------------------------------
// Sections 7a + 7b – tracker update and face recognition
// -----------------------------------------------------------------------

void TrackingSystem::updateTrackerAndFaces(std::vector<TrackedObject>& tracked,
                                            const cv::Mat& frame,
                                            const std::vector<Detection>& detections,
                                            const SystemSettings& settings,
                                            int detectorLag,
                                            const cv::Point2d& cameraMotion,
                                            bool hasNewDetections) {
    m_tracker->update(detections, settings, frame.size(), detectorLag, cameraMotion, hasNewDetections);
    tracked = m_tracker->getTrackedObjects(settings.trackerMaxTrailLength);
    
    // Safety clamp for all tracked objects displayed in HUD
    for (auto& obj : tracked) {
        obj.box = clampRect(obj.box, frame.cols, frame.rows);
    }

    if (!settings.faceRecognitionEnabled) {
        m_log(LogLevel::VERBOSE, "[FaceRecognizer] Skipped: faceRecognitionEnabled=false");
        return;
    }
    if (!m_faceRecognizer) {
        m_log(LogLevel::ERR, "[FaceRecognizer] Skipped: m_faceRecognizer is null (init failed?)");
        return;
    }

    for (auto& obj : tracked) {
        if (obj.class_id != 0 || !obj.is_confirmed) continue;

        bool runRecognition = false;
        {
            std::lock_guard<std::mutex> lock(m_faceMutex);
            auto it = m_trackIdToFace.find(obj.track_id);
            if (it == m_trackIdToFace.end()) {
                runRecognition = true;
            } else {
                // Retry every 15 frames to update tracking and check identity
                it->second.retryCountdown--;
                if (it->second.retryCountdown <= 0) {
                    runRecognition = true;
                    it->second.retryCountdown = 15;
                }
            }
        }
        if (runRecognition) {
            auto faceResults = m_faceRecognizer->process(
                frame, obj.box, settings.faceRecognitionThreshold,
                settings.faceDetectionMinConfidence);
            m_log(LogLevel::VERBOSE, "[FaceRecognizer] track=" + std::to_string(obj.track_id) +
                  " box=" + std::to_string(obj.box.width) + "x" + std::to_string(obj.box.height) +
                  " faces_found=" + std::to_string(faceResults.size()));

            // Update debug counters (atomic-ish — single tracking thread writes these).
            m_faceDbg.callCount++;
            m_faceDbg.lastFacesFound  = static_cast<int>(faceResults.size());
            m_faceDbg.totalFacesFound += static_cast<int>(faceResults.size());

            {
                std::lock_guard<std::mutex> lock(m_faceMutex);
                auto& info = m_trackIdToFace[obj.track_id];
                if (!faceResults.empty()) {
                    // Update identity if recognized, otherwise keep current identity
                    if (faceResults[0].identityId != -1) {
                        info.face_id   = faceResults[0].identityId;
                        info.face_name = faceResults[0].name;
                    }
                    info.face_box  = faceResults[0].box;
                    info.last_person_box = obj.box;
                    m_log(LogLevel::INFO, "[FaceRecognizer] Face found for track=" +
                          std::to_string(obj.track_id) + " id=" + std::to_string(info.face_id) +
                          " name=" + info.face_name);
                } else {
                    // No face detected this time; we keep the existing identity but don't update face_box
                    if (info.last_person_box.width == 0) info.face_id = -1;
                }
                info.retryCountdown = 15;
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_faceMutex);
            auto it = m_trackIdToFace.find(obj.track_id);
            if (it != m_trackIdToFace.end()) {
                obj.face_id   = it->second.face_id;
                obj.face_name = it->second.face_name;
                
                // Interpolate face_box relative to current person box for smooth tracking
                if (it->second.last_person_box.width > 0 && it->second.last_person_box.height > 0) {
                    float rel_x = (it->second.face_box.x - it->second.last_person_box.x) / (float)it->second.last_person_box.width;
                    float rel_y = (it->second.face_box.y - it->second.last_person_box.y) / (float)it->second.last_person_box.height;
                    float rel_w = it->second.face_box.width / (float)it->second.last_person_box.width;
                    float rel_h = it->second.face_box.height / (float)it->second.last_person_box.height;
                    
                    obj.face_box.x = obj.box.x + static_cast<int>(rel_x * obj.box.width);
                    obj.face_box.y = obj.box.y + static_cast<int>(rel_y * obj.box.height);
                    obj.face_box.width = static_cast<int>(rel_w * obj.box.width);
                    obj.face_box.height = static_cast<int>(rel_h * obj.box.height);
                } else {
                    obj.face_box = it->second.face_box;
                }
            }
        }
    }

    // Prune face cache for tracks that are no longer alive
    {
        std::lock_guard<std::mutex> lock(m_faceMutex);
        for (auto it = m_trackIdToFace.begin(); it != m_trackIdToFace.end(); ) {
            bool alive = false;
            for (const auto& obj : tracked)
                if (obj.track_id == it->first) { alive = true; break; }
            it = alive ? std::next(it) : m_trackIdToFace.erase(it);
        }
    }

    // Push updated debug state (written only by this thread, cheap copy).
    if (m_faceRecognizer) {
        m_faceDbg.identityCount = m_faceRecognizer->identityCount();
        m_blackboard.setFaceDebugState(m_faceDbg);
    }
}

// -----------------------------------------------------------------------
// Section 9 – pixel template matching
// (also exposes the pixel target as a TrackedObject for downstream stages)
// -----------------------------------------------------------------------

void TrackingSystem::runPixelTemplateMatch(std::vector<TrackedObject>& tracked,
                                            const cv::Mat& frame,
                                            const UICommandState& cmd,
                                            const SystemSettings& settings) {
    if (m_pixelLockActive && m_sharedLockedTarget.state != TrackingState::SEARCHING) {
        if (!cmd.pixelLockDragging) {
            cv::Rect lastBox = m_sharedLockedTarget.box;
            float prevCx = m_pixelCenterX;
            float prevCy = m_pixelCenterY;

            m_pixelCenterX += m_pixelVx;
            m_pixelCenterY += m_pixelVy;

            cv::Rect predicted(
                static_cast<int>(std::round(m_pixelCenterX - lastBox.width  / 2.0f)),
                static_cast<int>(std::round(m_pixelCenterY - lastBox.height / 2.0f)),
                lastBox.width, lastBox.height);
            
            // Safety clamp for predicted box
            predicted = clampRect(predicted, frame.cols, frame.rows);

            const int pad = 80;
            int sx1 = std::max(0, std::min(predicted.x - pad, frame.cols - 1));
            int sy1 = std::max(0, std::min(predicted.y - pad, frame.rows - 1));
            int sx2 = std::max(0, std::min(predicted.x + predicted.width  + pad, frame.cols));
            int sy2 = std::max(0, std::min(predicted.y + predicted.height + pad, frame.rows));
            cv::Rect searchRect(sx1, sy1, sx2 - sx1, sy2 - sy1);

            if (searchRect.width >= m_pixelTemplate.cols * 0.95f &&
                searchRect.height >= m_pixelTemplate.rows * 0.95f) {

                double bestMaxVal = -1.0;
                cv::Point bestMaxLoc;
                cv::Mat bestResult;
                float bestScale = 1.0f;

                for (float scale : {0.95f, 1.0f, 1.05f}) {
                    cv::Mat scaledTemplate;
                    if (std::abs(scale - 1.0f) > 1e-3f)
                        cv::resize(m_pixelTemplate, scaledTemplate, cv::Size(), scale, scale);
                    else
                        scaledTemplate = m_pixelTemplate;

                    if (searchRect.width < scaledTemplate.cols ||
                        searchRect.height < scaledTemplate.rows) continue;

                    cv::Mat result;
                    cv::matchTemplate(frame(searchRect), scaledTemplate, result, cv::TM_CCOEFF_NORMED);
                    double minVal, maxVal;
                    cv::Point minLoc, maxLoc;
                    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
                    if (maxVal > bestMaxVal) {
                        bestMaxVal = maxVal;
                        bestMaxLoc = maxLoc;
                        bestResult = result;
                        bestScale  = scale;
                    }
                }

                if (bestMaxVal > 0.5) {
                    // Sub-pixel refinement via parabolic interpolation
                    double dx = 0.0, dy = 0.0;
                    if (bestMaxLoc.x > 0 && bestMaxLoc.x < bestResult.cols - 1) {
                        float vL = bestResult.at<float>(bestMaxLoc.y, bestMaxLoc.x - 1);
                        float vR = bestResult.at<float>(bestMaxLoc.y, bestMaxLoc.x + 1);
                        float vC = static_cast<float>(bestMaxVal);
                        float denom = vL - 2.0f * vC + vR;
                        if (std::abs(denom) > 1e-5f) dx = (vL - vR) / (2.0f * denom);
                    }
                    if (bestMaxLoc.y > 0 && bestMaxLoc.y < bestResult.rows - 1) {
                        float vT = bestResult.at<float>(bestMaxLoc.y - 1, bestMaxLoc.x);
                        float vB = bestResult.at<float>(bestMaxLoc.y + 1, bestMaxLoc.x);
                        float vC = static_cast<float>(bestMaxVal);
                        float denom = vT - 2.0f * vC + vB;
                        if (std::abs(denom) > 1e-5f) dy = (vT - vB) / (2.0f * denom);
                    }

                    float tw = m_pixelTemplate.cols * bestScale;
                    float th = m_pixelTemplate.rows * bestScale;
                    double newCx = searchRect.x + bestMaxLoc.x + dx + tw / 2.0;
                    double newCy = searchRect.y + bestMaxLoc.y + dy + th / 2.0;

                    float alpha = settings.trackerVelocitySmoothing;
                    float pixDx = static_cast<float>(newCx - prevCx);
                    float pixDy = static_cast<float>(newCy - prevCy);

                    if (pixDx * pixDx + pixDy * pixDy < 1.0f) {
                        m_pixelVx *= 0.5f; m_pixelVy *= 0.5f;
                        if (std::abs(m_pixelVx) < 0.05f) m_pixelVx = 0.0f;
                        if (std::abs(m_pixelVy) < 0.05f) m_pixelVy = 0.0f;
                    } else {
                        m_pixelVx = alpha * pixDx + (1.0f - alpha) * m_pixelVx;
                        m_pixelVy = alpha * pixDy + (1.0f - alpha) * m_pixelVy;
                    }

                    // Hard Velocity Cap for Pixel Target (max 120 px/frame)
                    m_pixelVx = std::clamp(m_pixelVx, -120.0f, 120.0f);
                    m_pixelVy = std::clamp(m_pixelVy, -120.0f, 120.0f);

                    m_pixelCenterX = static_cast<float>(newCx);
                    m_pixelCenterY = static_cast<float>(newCy);

                    cv::Rect newBox(
                        static_cast<int>(std::round(m_pixelCenterX - tw / 2.0f)),
                        static_cast<int>(std::round(m_pixelCenterY - th / 2.0f)),
                        static_cast<int>(std::round(tw)),
                        static_cast<int>(std::round(th)));

                    m_sharedLockedTarget.box         = clampRect(newBox, frame.cols, frame.rows);
                    m_sharedLockedTarget.confidence  = static_cast<float>(bestMaxVal);
                    m_sharedLockedTarget.lost_frames = 0;
                    m_sharedLockedTarget.state       = TrackingState::LOCKED;
                    m_sharedLockedTarget.trail.push_back(
                        cv::Point(newBox.x + newBox.width / 2, newBox.y + newBox.height / 2));
                    if (m_sharedLockedTarget.trail.size() >
                        static_cast<size_t>(settings.trackerMaxTrailLength))
                        m_sharedLockedTarget.trail.erase(m_sharedLockedTarget.trail.begin());

                    // Adaptive template update: blend in current patch when match is very strong
                    if (bestMaxVal > 0.85) {
                        cv::Mat patch;
                        cv::getRectSubPix(frame, m_pixelTemplate.size(),
                                          cv::Point2f(m_pixelCenterX, m_pixelCenterY), patch);
                        if (patch.size() == m_pixelTemplate.size() &&
                            patch.type() == m_pixelTemplate.type())
                            cv::addWeighted(m_pixelTemplate, 0.95, patch, 0.05, 0.0, m_pixelTemplate);
                    }
                } else {
                    m_sharedLockedTarget.lost_frames++;
                    m_pixelVx *= settings.trackerDeadReckoningDamping;
                    m_pixelVy *= settings.trackerDeadReckoningDamping;
                    m_sharedLockedTarget.box   = predicted;
                    m_sharedLockedTarget.state = TrackingState::LOST;
                    if (m_sharedLockedTarget.lost_frames > settings.trackerMaxLostFrames)
                        m_pixelLockActive = false;
                }
            }
        }
    }

    // Expose the pixel target as a TrackedObject so rendering and alarm-zone
    // checks see it alongside YOLO-tracked objects
    if (m_pixelLockActive && m_sharedLockedTarget.state != TrackingState::SEARCHING) {
        TrackedObject po;
        po.track_id     = m_sharedLockedTarget.track_id;
        po.class_id     = -1;
        po.className    = m_sharedLockedTarget.className;
        po.box          = m_sharedLockedTarget.box;
        po.confidence   = m_sharedLockedTarget.confidence;
        po.lost_frames  = m_sharedLockedTarget.lost_frames;
        po.is_active    = (m_sharedLockedTarget.state == TrackingState::LOCKED);
        po.is_confirmed = true;
        po.trail        = m_sharedLockedTarget.trail;
        tracked.push_back(po);
    }
}

// -----------------------------------------------------------------------
// Section 10 – YOLO-based locked target state update
// -----------------------------------------------------------------------

void TrackingSystem::updateLockedTargetYolo(const std::vector<TrackedObject>& tracked) {
    if (m_pixelLockActive || m_sharedLockedTarget.state == TrackingState::SEARCHING) return;

    bool found = false;
    for (const auto& t : tracked) {
        if (t.track_id == m_sharedLockedTarget.track_id) {
            m_sharedLockedTarget.box        = t.box;
            m_sharedLockedTarget.className  = t.className;
            m_sharedLockedTarget.confidence = t.confidence;
            m_sharedLockedTarget.trail      = t.trail;
            found = true;
            break;
        }
    }
    m_sharedLockedTarget.state = found ? TrackingState::LOCKED : TrackingState::LOST;
}

// -----------------------------------------------------------------------
// Section 11 – audio feedback for lock state transitions and pulse
// -----------------------------------------------------------------------

void TrackingSystem::updateAudioFeedback(const SystemSettings& settings) {
    const TrackingState cur = m_sharedLockedTarget.state;

    if (m_prevLockState != TrackingState::LOCKED && cur == TrackingState::LOCKED) {
        m_audioEngine.playLockAcquired();
        m_lockStableFrames = 0;
        if (settings.audioLockPulseEnabled && settings.audioEnabled)
            m_audioEngine.startLockPulse(m_sharedLockedTarget.confidence);
    } else if (m_prevLockState == TrackingState::LOCKED && cur != TrackingState::LOCKED) {
        m_audioEngine.stopLockPulse();
        m_lockStableFrames = 0;
        if (cur == TrackingState::LOST)
            m_audioEngine.playLockLost();
    } else if (cur == TrackingState::LOCKED) {
        // Stability counter: grows on clean frames, shrinks on lost frames
        if (m_sharedLockedTarget.lost_frames == 0)
            m_lockStableFrames = std::min(m_lockStableFrames + 1, 150);
        else
            m_lockStableFrames = std::max(0, m_lockStableFrames - 4);

        // Stability bonus: up to +0.15 after ~5 s of clean lock at 30 fps
        const float stabilityBonus = std::min(0.15f, m_lockStableFrames / 150.0f * 0.15f);
        const float lockStrength   = std::min(1.0f, m_sharedLockedTarget.confidence + stabilityBonus);
        m_audioEngine.updateLockStrength(lockStrength);
    }

    m_prevLockState = cur;
}

// -----------------------------------------------------------------------
// Section 11b – automatic target prioritization
// Bewertet pro Frame, welches Ziel für den Operator am wichtigsten ist.
// Score-Komponenten: Klassengewicht, Alarmzone, Tempo, Nähe, Annäherung,
// Neuheit. Rang 0 = wichtigstes Ziel (HUD hebt es hervor).
// -----------------------------------------------------------------------

void TrackingSystem::computeTargetPriorities(std::vector<TrackedObject>& tracked,
                                             const SystemSettings& settings,
                                             cv::Size frameSize) {
    if (!settings.targetPriorityEnabled || tracked.empty()) {
        for (auto& obj : tracked) { obj.priority = 0.0f; obj.priorityRank = -1; obj.priorityReason.clear(); }
        m_prioMem.clear();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float frameArea = std::max(1.0f, static_cast<float>(frameSize.width) *
                                            static_cast<float>(frameSize.height));

    // Aktive Alarmzonen einsammeln
    std::vector<cv::Rect> alarmZones;
    for (const auto& z : m_roiManager.getROIs())
        if (z.active && z.function == ROIFunction::ALARM)
            alarmZones.push_back(z.rect);

    // Klassengewicht: Wie operator-relevant ist diese Objektklasse grundsätzlich?
    auto classWeight = [](const std::string& cls) -> float {
        if (cls == "person") return 1.0f;
        if (cls == "car" || cls == "truck" || cls == "bus" ||
            cls == "motorcycle" || cls == "bicycle") return 0.85f;
        if (cls == "airplane" || cls == "boat" || cls == "train") return 0.8f;
        if (cls == "dog" || cls == "cat" || cls == "horse") return 0.5f;
        return 0.35f;
    };

    // Verlaufsspeicher für verschwundene Tracks aufräumen
    {
        std::unordered_set<int> alive;
        for (const auto& o : tracked) alive.insert(o.track_id);
        for (auto it = m_prioMem.begin(); it != m_prioMem.end(); )
            it = alive.count(it->first) ? std::next(it) : m_prioMem.erase(it);
    }

    for (auto& obj : tracked) {
        auto& mem = m_prioMem[obj.track_id];
        if (mem.firstSeen == std::chrono::steady_clock::time_point()) {
            mem.firstSeen = now;
            mem.smoothedArea = static_cast<float>(obj.box.area());
        }

        std::string reason;
        auto addReason = [&reason](const char* tag) {
            if (!reason.empty()) reason += "+";
            reason += tag;
        };

        float score = classWeight(obj.className);

        // Alarmzone: dominanter Faktor — ein Ziel in einer Alarmzone hat Vorrang.
        const cv::Point center(obj.box.x + obj.box.width / 2, obj.box.y + obj.box.height / 2);
        for (const auto& z : alarmZones) {
            if (z.contains(center)) { score += 1.5f; addReason("ALARM"); break; }
        }

        // Tempo relativ zur Objektgröße (px/Frame pro Objektdiagonale)
        const float objDiag = std::hypot(static_cast<float>(obj.box.width),
                                          static_cast<float>(obj.box.height));
        const float relSpeed = std::hypot(obj.vx, obj.vy) / std::max(1.0f, objDiag);
        if (relSpeed > 0.02f) {
            score += std::min(0.6f, relSpeed * 6.0f);
            if (relSpeed > 0.06f) addReason("FAST");
        }

        // Nähe: Bildflächenanteil (große Box = nah an der Kamera)
        const float areaFrac = static_cast<float>(obj.box.area()) / frameArea;
        score += std::min(0.5f, areaFrac * 5.0f);
        if (areaFrac > 0.08f) addReason("CLOSE");

        // Annäherung: wächst die Box nachhaltig, kommt das Objekt auf die Kamera zu.
        const float area = static_cast<float>(obj.box.area());
        if (mem.smoothedArea > 1.0f) {
            const float growth = (area - mem.smoothedArea) / mem.smoothedArea;
            mem.areaGrowth = 0.9f * mem.areaGrowth + 0.1f * growth;
            if (mem.areaGrowth > 0.004f) { // ~+12 %/s Flächenwachstum bei 30 fps
                score += 0.4f;
                addReason("APPROACHING");
            }
        }
        mem.smoothedArea = 0.9f * mem.smoothedArea + 0.1f * area;

        // Neuheit: frisch aufgetauchte Ziele brauchen Operator-Aufmerksamkeit.
        const float ageSec = std::chrono::duration<float>(now - mem.firstSeen).count();
        if (ageSec < 3.0f) { score += 0.4f * (1.0f - ageSec / 3.0f); if (ageSec < 1.5f) addReason("NEW"); }

        // Verlorene Tracks sind weniger handlungsrelevant.
        if (obj.lost_frames > 0) score *= 0.4f;

        obj.priority = score;
        obj.priorityReason = std::move(reason);
    }

    // Rang vergeben (0 = wichtigstes Ziel)
    std::vector<int> order(tracked.size());
    for (size_t i = 0; i < order.size(); ++i) order[static_cast<int>(i)] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&tracked](int a, int b) {
        return tracked[a].priority > tracked[b].priority;
    });
    for (size_t rank = 0; rank < order.size(); ++rank)
        tracked[order[rank]].priorityRank = static_cast<int>(rank);
}

// -----------------------------------------------------------------------
// Section 12 – alarm zone checks
// -----------------------------------------------------------------------

void TrackingSystem::checkAlarmZones(const std::vector<TrackedObject>& tracked) {
    auto zones = m_roiManager.getROIs();
    std::unordered_set<int> activeZoneIds;

    for (const auto& z : zones) {
        if (!z.active || z.function != ROIFunction::ALARM) continue;
        activeZoneIds.insert(z.id);

        std::unordered_set<int> inZoneNow;
        for (const auto& obj : tracked) {
            cv::Point center(obj.box.x + obj.box.width / 2, obj.box.y + obj.box.height / 2);
            if (z.rect.contains(center)) inZoneNow.insert(obj.track_id);
        }

        for (int tid : inZoneNow) {
            if (m_activeAlarms[z.id].find(tid) == m_activeAlarms[z.id].end()) {
                m_log(LogLevel::WARN, "ALARM: Object #" + std::to_string(tid) +
                      " entered Alarm Zone '" + z.label + "'");
                m_audioEngine.playAlarmEntry();
            }
        }
        for (int tid : m_activeAlarms[z.id]) {
            if (inZoneNow.find(tid) == inZoneNow.end()) {
                m_log(LogLevel::INFO, "Object #" + std::to_string(tid) +
                      " left Alarm Zone '" + z.label + "'");
                m_audioEngine.playAlarmExit();
            }
        }
        m_activeAlarms[z.id] = inZoneNow;
    }

    // Remove entries for zones that are no longer active
    for (auto it = m_activeAlarms.begin(); it != m_activeAlarms.end(); )
        it = activeZoneIds.count(it->first) ? std::next(it) : m_activeAlarms.erase(it);
}

// -----------------------------------------------------------------------
// Section 13 – data logging
// -----------------------------------------------------------------------

void TrackingSystem::logTrackingData(const std::vector<TrackedObject>& tracked,
                                      const std::vector<MotionTrack>& motionTracks,
                                      const SystemSettings& settings,
                                      uint64_t sessionMs) {
    if (!settings.dataLoggingEnabled || !m_dataLogger.isOpen()) return;

    std::vector<TrackedObject> logObjs = tracked;
    for (const auto& mt : motionTracks) {
        bool overlaps = false;
        for (const auto& obj : tracked) {
            if (!obj.is_active || !obj.is_confirmed) continue;
            cv::Rect inter = mt.box & obj.box;
            if (inter.area() > 0 && static_cast<double>(inter.area()) / mt.box.area() > 0.2) {
                overlaps = true; break;
            }
        }
        if (!overlaps) {
            TrackedObject mo;
            mo.track_id     = mt.id + 10000;
            mo.class_id     = -99;
            mo.className    = "Motion";
            mo.box          = mt.box;
            mo.confidence   = 1.0f;
            mo.is_active    = true;
            mo.is_confirmed = true;
            logObjs.push_back(mo);
        }
    }
    m_dataLogger.logFrame(static_cast<double>(sessionMs), logObjs, 0.0);
}

void TrackingSystem::updateTargetHistory(const std::vector<TrackedObject>& activeTracks,
                                          const cv::Mat& currentFrame, int manualCaptureId) {
    if (currentFrame.empty()) return;

    auto getTimestamp = []() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        struct tm buf;
#ifdef _WIN32
        localtime_s(&buf, &t);
#else
        localtime_r(&t, &buf);
#endif
        char ts[64];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &buf);
        return std::string(ts);
    };

    std::string timestamp = getTimestamp();

    std::vector<int> previouslyActiveIds;
    for (auto& rec : m_internalTargetHistory) {
        if (rec.is_currently_active) previouslyActiveIds.push_back(rec.track_id);
        rec.is_currently_active = false;
    }

    for (const auto& obj : activeTracks) {
        auto it = std::find_if(m_internalTargetHistory.begin(), m_internalTargetHistory.end(),
                               [&](const UniqueTargetRecord& r) { return r.track_id == obj.track_id; });

        if (it == m_internalTargetHistory.end()) {
            UniqueTargetRecord rec;
            rec.track_id              = obj.track_id;
            rec.class_id              = obj.class_id;
            rec.className             = obj.className;
            rec.max_confidence        = obj.confidence;
            rec.first_seen_timestamp  = timestamp;
            rec.last_seen_timestamp   = timestamp;
            rec.first_box             = obj.box;
            rec.last_box              = obj.box;
            rec.trail                 = obj.trail;
            rec.is_currently_active   = true;
            rec.face_id               = obj.face_id;
            rec.face_name             = obj.face_name;

            float pf  = 0.5f;
            cv::Rect roi = obj.box;
            int padW = static_cast<int>(roi.width  * pf);
            int padH = static_cast<int>(roi.height * pf);
            roi.x -= padW; roi.y -= padH;
            roi.width  += 2 * padW; roi.height += 2 * padH;

            // Safe Clamping: ensure x/y are within [0, cols/rows] and width/height don't go negative
            int x1 = std::max(0, std::min(roi.x, currentFrame.cols - 1));
            int y1 = std::max(0, std::min(roi.y, currentFrame.rows - 1));
            int x2 = std::max(0, std::min(roi.x + roi.width,  currentFrame.cols));
            int y2 = std::max(0, std::min(roi.y + roi.height, currentFrame.rows));
            roi = cv::Rect(x1, y1, x2 - x1, y2 - y1);

            if (roi.width > 0 && roi.height > 0) {
                TargetSnapshot snap;
                snap.image        = currentFrame(roi).clone();
                snap.timestamp    = timestamp;
                snap.box          = obj.box;
                snap.confidence   = obj.confidence;
                snap.qualityScore = computeSnapQuality(snap.image, snap.confidence,
                                                       snap.box, currentFrame.size());
                rec.snapshot_first = rec.snapshot_mid = rec.snapshot_last = snap;
                rec.snapshot_best  = snap;
                rec.periodic_snapshots.push_back(snap);
            }
            rec.last_snapshot_time       = std::chrono::steady_clock::now();
            rec.cropped_image_first_version = 1;
            rec.cropped_image_mid_version   = 1;
            rec.cropped_image_last_version  = 1;
            m_internalTargetHistory.push_back(rec);
        } else {
            it->last_seen_timestamp = timestamp;
            it->last_box            = obj.box;
            it->trail               = obj.trail;
            it->is_currently_active = true;
            if (obj.face_id != -1) { it->face_id = obj.face_id; it->face_name = obj.face_name; }
            if (obj.confidence > it->max_confidence) it->max_confidence = obj.confidence;

            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->last_snapshot_time).count();

            auto captureSnap = [&]() {
                float pf = 0.5f;
                cv::Rect roi = obj.box;
                int padW = static_cast<int>(roi.width  * pf);
                int padH = static_cast<int>(roi.height * pf);
                roi.x -= padW; roi.y -= padH;
                roi.width  += 2 * padW; roi.height += 2 * padH;

                int cx1 = std::max(0, std::min(roi.x, currentFrame.cols - 1));
                int cy1 = std::max(0, std::min(roi.y, currentFrame.rows - 1));
                int cx2 = std::max(0, std::min(roi.x + roi.width,  currentFrame.cols));
                int cy2 = std::max(0, std::min(roi.y + roi.height, currentFrame.rows));
                roi = cv::Rect(cx1, cy1, cx2 - cx1, cy2 - cy1);
                
                if (roi.width <= 0 || roi.height <= 0) return;

                TargetSnapshot snap;
                snap.image        = currentFrame(roi).clone();
                snap.timestamp    = timestamp;
                snap.box          = obj.box;
                snap.confidence   = obj.confidence;
                snap.qualityScore = computeSnapQuality(snap.image, snap.confidence,
                                                        snap.box, currentFrame.size());
                if (snap.qualityScore > it->snapshot_best.qualityScore)
                    it->snapshot_best = snap;
                it->periodic_snapshots.push_back(snap);
                it->last_snapshot_time = now;

                const size_t MAX_GALLERY = 24;
                if (it->periodic_snapshots.size() > MAX_GALLERY) {
                    std::vector<TargetSnapshot> dec;
                    dec.reserve(MAX_GALLERY);
                    dec.push_back(it->periodic_snapshots[0]);
                    for (size_t i = 2; i < it->periodic_snapshots.size(); i += 2)
                        dec.push_back(it->periodic_snapshots[i]);
                    it->periodic_snapshots = std::move(dec);
                    it->current_snapshot_interval_ms *= 2;
                    it->gallery_version++;
                    if (it->snapshot_mid_manual_idx > 0) {
                        if (it->snapshot_mid_manual_idx % 2 == 0)
                            it->snapshot_mid_manual_idx /= 2;
                        else
                            it->snapshot_mid_manual_idx = -1;
                    }
                    m_log(LogLevel::INFO, "Decimated gallery for Target ID " +
                          std::to_string(it->track_id) + ". New interval: " +
                          std::to_string(it->current_snapshot_interval_ms) + "ms");
                }

                it->snapshot_last = snap;
                it->cropped_image_last_version++;
                if (it->snapshot_mid_manual_idx == -1) {
                    int mid = static_cast<int>(it->periodic_snapshots.size()) / 2;
                    it->snapshot_mid = it->periodic_snapshots[mid];
                    it->cropped_image_mid_version++;
                }
            };

            if (elapsedMs >= it->current_snapshot_interval_ms) captureSnap();
            if (manualCaptureId == obj.track_id) {
                captureSnap();
                m_log(LogLevel::INFO, "Manual snapshot captured for Target ID " +
                      std::to_string(obj.track_id));
            }
        }
    }

    // Finalize visual chronology for newly-lost targets
    for (auto& rec : m_internalTargetHistory) {
        bool wasActive = std::find(previouslyActiveIds.begin(), previouslyActiveIds.end(),
                                   rec.track_id) != previouslyActiveIds.end();
        if (!rec.is_currently_active && wasActive && !rec.periodic_snapshots.empty()) {
            rec.snapshot_first = rec.periodic_snapshots.front();
            rec.cropped_image_first_version++;
            if (rec.snapshot_mid_manual_idx >= 0 &&
                rec.snapshot_mid_manual_idx < static_cast<int>(rec.periodic_snapshots.size()))
                rec.snapshot_mid = rec.periodic_snapshots[rec.snapshot_mid_manual_idx];
            else {
                int mid = static_cast<int>(rec.periodic_snapshots.size()) / 2;
                rec.snapshot_mid = rec.periodic_snapshots[mid];
            }
            rec.cropped_image_mid_version++;
            rec.snapshot_last = rec.periodic_snapshots.back();
            rec.cropped_image_last_version++;
            m_log(LogLevel::INFO, "Finalized visual chronology for Target ID " +
                  std::to_string(rec.track_id) + " (" +
                  std::to_string(rec.periodic_snapshots.size()) + " images).");
        }
    }
}
