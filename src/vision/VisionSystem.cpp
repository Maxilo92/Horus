#include "VisionSystem.hpp"
#include "TrackingSystem.hpp"
#include <iostream>
#include <chrono>

VisionSystem::VisionSystem(Blackboard& blackboard, AudioEngine& audioEngine, LogFn logFn)
    : m_blackboard(blackboard),
      m_audioEngine(audioEngine),
      m_log(std::move(logFn)) {
    m_camera = std::make_unique<CameraModule>();
}

VisionSystem::~VisionSystem() {
    stop();
}

bool VisionSystem::init(const std::string& cameraAddress, const std::string& modelPath,
                         const std::string& labelsPath, const std::string& initialAddress) {
    m_cameraAddress = initialAddress.empty() ? cameraAddress : initialAddress;

    SystemSettings settings = m_blackboard.getSettings();
    int requestedW = settings.request4KCamera ? 3840 : 1280;
    int requestedH = settings.request4KCamera ? 2160 : 720;

    if (!m_camera->open(m_cameraAddress, requestedW, requestedH)) {
        m_log(LogLevel::ERR, "[VisionSystem] Failed to open camera: " + m_cameraAddress);
        return false;
    }

    m_log(LogLevel::INFO, "[VisionSystem] Camera opened: " + m_cameraAddress +
          " (backend=" + m_camera->getBackendName() + ")");
    m_log(LogLevel::INFO, "[VisionSystem] Camera resolution requested=" +
          std::to_string(requestedW) + "x" + std::to_string(requestedH) +
          ", actual=" + std::to_string(m_camera->getWidth()) + "x" +
          std::to_string(m_camera->getHeight()));

    // Derive sibling model paths from the resolved modelPath
    {
        std::string dir;
        auto sep = modelPath.find_last_of("/\\");
        if (sep != std::string::npos) dir = modelPath.substr(0, sep + 1);

        m_labelsPath = labelsPath;
        m_modelPathS = dir + "yolov8s.onnx";
        m_modelPathN = dir + "yolov8n.onnx";

        // Verify which paths actually exist
        auto exists = [](const std::string& p) {
            FILE* f = fopen(p.c_str(), "r");
            if (f) { fclose(f); return true; }
            return false;
        };
        if (!exists(m_modelPathS)) m_modelPathS.clear();
        if (!exists(m_modelPathN)) m_modelPathN.clear();
    }

    try {
        m_detector = std::make_unique<ObjectDetector>(modelPath, labelsPath);
    } catch (const std::exception& e) {
        m_log(LogLevel::ERR, std::string("[VisionSystem] Detector init failed: ") + e.what());
        return false;
    }

    // Determine active model name from the resolved path
    std::string activeModel = "unknown";
    if (modelPath.find("yolov8s") != std::string::npos) activeModel = "yolov8s";
    else if (modelPath.find("yolov8n") != std::string::npos) activeModel = "yolov8n";

    AppStatusState status = m_blackboard.getAppStatus();
    status.cameraStatus = "OK — " + m_cameraAddress;
    status.cameraStatusOk = true;
    status.classNames = m_detector->getClasses();
    status.activeModelName = activeModel;
    m_blackboard.setAppStatus(status);

    return true;
}

void VisionSystem::start() {
    if (m_running.load()) return;
    m_running = true;
    m_captureThread  = std::thread(&VisionSystem::captureWorkerLoop, this);
    m_detectorThread = std::thread(&VisionSystem::detectorWorkerLoop, this);
    m_workerThread   = std::thread(&VisionSystem::workerLoop, this);
}

void VisionSystem::stop() {
    m_running = false;
    m_detectorCv.notify_all();
    if (m_captureThread.joinable())  m_captureThread.join();
    if (m_detectorThread.joinable()) m_detectorThread.join();
    if (m_workerThread.joinable())   m_workerThread.join();
}

void VisionSystem::update() {
    // No-op: all logic is driven by the internal worker threads
}

// ---------------------------------------------------------------------------
// Capture thread: reads frames from the camera, handles hot-swap
// ---------------------------------------------------------------------------
void VisionSystem::captureWorkerLoop() {
    m_log(LogLevel::INFO, "Capture thread started");
    cv::Mat frame;
    bool lastRequest4K = m_blackboard.getSettings().request4KCamera;
    
    // Playback state
    bool isPaused = false;
    int currentFrame = 0;

    while (m_running) {
        SystemSettings settings = m_blackboard.getSettings();

        // Detect resolution-mode change and trigger a self-swap
        if (settings.request4KCamera != lastRequest4K) {
            lastRequest4K = settings.request4KCamera;
            m_blackboard.requestCameraChange(m_cameraAddress);
        }

        // Handle camera hot-swap (from UI or resolution change)
        std::string newAddr;
        if (m_blackboard.consumeCameraChangeRequest(newAddr)) {
            int W = settings.request4KCamera ? 3840 : 1280;
            int H = settings.request4KCamera ? 2160 : 720;
            m_log(LogLevel::INFO, "Hot-swapping camera to: " + newAddr +
                  " at " + std::to_string(W) + "x" + std::to_string(H));
            m_camera->close();
            bool ok = m_camera->open(newAddr, W, H);
            m_motionDetector.reset();
            m_cameraAddress = newAddr;
            isPaused = false; // Reset pause on new source

            AppStatusState st = m_blackboard.getAppStatus();
            st.cameraStatus   = ok ? "OK — " + newAddr : "FAILED — " + newAddr;
            st.cameraStatusOk = ok;
            m_blackboard.setAppStatus(st);

            if (ok) {
                m_log(LogLevel::INFO, "Camera opened: " + newAddr +
                      " (backend=" + m_camera->getBackendName() + ", actual=" +
                      std::to_string(m_camera->getWidth()) + "x" +
                      std::to_string(m_camera->getHeight()) + ")");
                if (m_camera->getWidth() < W || m_camera->getHeight() < H)
                    m_log(LogLevel::WARN, "Camera did not accept requested resolution. Limited to " +
                          std::to_string(m_camera->getWidth()) + "x" + std::to_string(m_camera->getHeight()));
            } else {
                m_log(LogLevel::ERR, "Camera failed to open: " + newAddr);
            }
        }

        // Replay commands
        ReplayCommand cmd = m_blackboard.consumeReplayCommands();
        if (m_camera->isVideoFile()) {
            if (cmd.pauseRequested) isPaused = true;
            if (cmd.playRequested)  isPaused = false;
            if (cmd.seekRequested) {
                m_camera->seekToFrame(cmd.seekFrame);
            }
            if (cmd.stepRequested) {
                int next = m_camera->getCurrentFrame() + cmd.stepDirection;
                m_camera->seekToFrame(next);
            }
        }

        auto capStart = std::chrono::high_resolution_clock::now();
        bool readSuccess = false;
        
        if (!isPaused || !m_camera->isVideoFile()) {
            readSuccess = m_camera->read(frame);
        } else {
            // If paused, we still want to publish the "current" frame if it's new (e.g. after seek)
            // But we don't want to call read() which advances the pointer.
            // However, OpenCV doesn't have a "read current" without advancing easily, 
            // so we just read once after seek/step.
            if (cmd.seekRequested || cmd.stepRequested) {
                readSuccess = m_camera->read(frame);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
        }
        
        auto capEnd = std::chrono::high_resolution_clock::now();
        float capMs = std::chrono::duration<float, std::milli>(capEnd - capStart).count();

        if (m_camera->isVideoFile()) {
            ReplayState state;
            state.isFile = true;
            state.isPlaying = !isPaused;
            state.currentFrame = m_camera->getCurrentFrame();
            state.totalFrames = m_camera->getTotalFrames();
            state.fps = m_camera->getFps();
            m_blackboard.setReplayState(state);
        } else {
            ReplayState state;
            state.isFile = false;
            m_blackboard.setReplayState(state);
        }

        if (readSuccess) {
            if (!frame.empty()) {
                // Fast display path: UIManager reads this directly for low-latency rendering
                m_blackboard.updateDisplayFrame(frame);

                std::lock_guard<std::mutex> lock(m_captureMutex);
                m_captureFrame = frame; // Shallow copy
                m_captureNewFrameVision = true;

                m_blackboard.updateStatusCounts(-1, -1, 1);

                // Update performance metrics
                PerformanceMetrics perf = m_blackboard.getPerformanceMetrics();
                perf.captureTimeMs = capMs;
                m_blackboard.setPerformanceMetrics(perf);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    m_log(LogLevel::INFO, "Capture thread stopping");
}

// ---------------------------------------------------------------------------
// Detector thread: async YOLO inference
// ---------------------------------------------------------------------------
void VisionSystem::detectorWorkerLoop() {
    m_log(LogLevel::INFO, "Detector thread started");
    while (m_running) {
        cv::Mat frameToProcess;
        SystemSettings settings;

        {
            std::unique_lock<std::mutex> lock(m_detectorMutex);
            m_detectorCv.wait(lock, [this]() {
                return !m_running || m_detectorBusy.load();
            });
            if (!m_running) break;
            frameToProcess = m_detectorFrameCopy.clone(); // Clone to avoid threading issues
            settings = m_detectorSettingsCopy;
        }

        if (frameToProcess.empty()) {
            m_detectorBusy = false;
            continue;
        }

        std::vector<Detection> results;
        auto inferStart = std::chrono::high_resolution_clock::now();
        try {
            if (m_detector) {
                cv::Mat input = frameToProcess;
                if (settings.grayscaleInput) {
                    cv::Mat gray;
                    cv::cvtColor(frameToProcess, gray, cv::COLOR_BGR2GRAY);
                    cv::cvtColor(gray, input, cv::COLOR_GRAY2BGR);
                }
                results = m_detector->detect(input, settings);
            }
            // ROI filtering happens at the tracking stage via TrackingSystem's ROIManager ref.
        } catch (const std::exception& e) {
            m_log(LogLevel::ERR, std::string("[VisionSystem] Detection failed: ") + e.what());
        }
        auto inferEnd = std::chrono::high_resolution_clock::now();
        float inferMs = std::chrono::duration<float, std::milli>(inferEnd - inferStart).count();

        {
            std::lock_guard<std::mutex> lock(m_detectorMutex);
            m_detectorResults    = std::move(results);
            m_detectorNewResults = true;
            m_detectorBusy       = false;
        }

        m_blackboard.updateStatusCounts(static_cast<int>(m_detectorResults.size()), -1, 0);

        // Update performance metrics
        PerformanceMetrics perf = m_blackboard.getPerformanceMetrics();
        perf.inferenceTimeMs = inferMs;
        m_blackboard.setPerformanceMetrics(perf);

        // Publish remote RTT (only meaningful when remote inference is active)
        if (settings.remoteInferenceEnabled && m_detector) {
            AppStatusState st = m_blackboard.getAppStatus();
            st.remoteInferenceRttMs = m_detector->getLastRttMs();
            m_blackboard.setAppStatus(st);
        }

        // Handle model switch request
        {
            UICommandState cmd = m_blackboard.consumeVisionCommands();
            if (cmd.modelSwitchRequested && m_detector) {
                const std::string& newPath = (cmd.requestedModelIdx == 0) ? m_modelPathS : m_modelPathN;
                const std::string  modelName = (cmd.requestedModelIdx == 0) ? "yolov8s" : "yolov8n";
                if (!newPath.empty()) {
                    m_log(LogLevel::INFO, "[VisionSystem] Switching model to " + modelName);
                    try {
                        auto newDetector = std::make_unique<ObjectDetector>(newPath, m_labelsPath);
                        m_detector = std::move(newDetector);
                        AppStatusState st = m_blackboard.getAppStatus();
                        st.activeModelName = modelName;
                        st.classNames = m_detector->getClasses();
                        m_blackboard.setAppStatus(st);
                        m_log(LogLevel::INFO, "[VisionSystem] Model switched to " + modelName);
                    } catch (const std::exception& e) {
                        m_log(LogLevel::ERR, "[VisionSystem] Model switch failed: " + std::string(e.what()));
                    }
                } else {
                    m_log(LogLevel::WARN, "[VisionSystem] Model path for index " +
                          std::to_string(cmd.requestedModelIdx) + " not found, skipping switch");
                }
            }
        }
    }
    m_log(LogLevel::INFO, "Detector thread stopping");
}

// ---------------------------------------------------------------------------
// Worker thread: motion, sub-zooms, zoom crop, dispatches to tracking
// ---------------------------------------------------------------------------
void VisionSystem::workerLoop() {
    m_log(LogLevel::INFO, "Worker thread started");

    cv::Mat rawFrame;
    cv::Mat trackingFrame;
    // Persistent zero-heap buffers for low-light processing
    cv::Mat labFrame;
    std::vector<cv::Mat> labChannels;
    cv::Ptr<cv::CLAHE> clahe;
    cv::Mat zoomLab;
    std::vector<cv::Mat> zoomChannels;
    cv::Ptr<cv::CLAHE> zoomClahe;
    cv::Mat zoomCrop;

    float currentFps = 0.0f;
    auto lastTime    = std::chrono::steady_clock::now();
    int frameSkipCounter = 0;

    // Per-worker-loop state for pending detections
    bool     pendingDetections     = false;
    uint64_t detectionTriggerFrame = 0;

    while (m_running) {
        SystemSettings settings = m_blackboard.getSettings();

        // Consume next capture frame
        {
            std::lock_guard<std::mutex> lock(m_captureMutex);
            if (!m_captureNewFrameVision || settings.debugFreezeVision) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            m_captureFrame.copyTo(rawFrame);
            m_captureNewFrameVision = false;
        }

        auto now   = std::chrono::steady_clock::now();
        currentFps = 1.0f / std::chrono::duration<float>(now - lastTime).count();
        lastTime   = now;

        trackingFrame = rawFrame; // Shallow copy

        // Low-light enhancement on tracking frame
        if (settings.lowLightEnhancement && !trackingFrame.empty())
            ImageUtils::enhanceLowLight(trackingFrame, labFrame, labChannels, clahe,
                                         settings.lowLightClipLimit, settings.lowLightDenoiseKernel);

        // ── Motion Detection ────────────────────────────────────────────
        std::vector<cv::Rect> motionRegions;
        cv::Mat heatmapFrame;
        if (settings.motionDetectionEnabled && !trackingFrame.empty()) {
            m_motionDetector.process(trackingFrame, settings);
            motionRegions = m_motionDetector.getMotionRegions();
            if (settings.motionHeatmapOverlay)
                m_motionDetector.getHeatmapImage().copyTo(heatmapFrame);
        }

        // ── Motion track management ─────────────────────────────────────
        bool newMotionSpawned = false;
        SubZoomData localSubZooms[4];

        if (settings.motionDetectionEnabled) {
            auto nowTP = std::chrono::steady_clock::now();

            // Prune stale tracks (unseen > 2s)
            for (auto it = m_workerMotionTracks.begin(); it != m_workerMotionTracks.end(); ) {
                float elapsed = std::chrono::duration<float>(nowTP - it->lastSeen).count();
                it = (elapsed > 2.0f) ? m_workerMotionTracks.erase(it) : std::next(it);
            }

            std::vector<bool> regionMatched(motionRegions.size(), false);
            std::vector<bool> trackMatched(m_workerMotionTracks.size(), false);

            // Associate new regions to existing tracks by center proximity
            for (size_t i = 0; i < motionRegions.size(); ++i) {
                cv::Point2f rc(motionRegions[i].x + motionRegions[i].width  / 2.0f,
                               motionRegions[i].y + motionRegions[i].height / 2.0f);
                int best  = -1; float bestDist = 200.0f;
                for (size_t j = 0; j < m_workerMotionTracks.size(); ++j) {
                    if (trackMatched[j]) continue;
                    cv::Point2f tc = m_workerMotionTracks[j].smoothedCenter;
                    float d = static_cast<float>(cv::norm(rc - tc));
                    if (d < bestDist) { bestDist = d; best = static_cast<int>(j); }
                }
                if (best != -1) {
                    auto& track = m_workerMotionTracks[best];
                    track.box      = motionRegions[i];
                    track.lastSeen = nowTP;
                    track.active   = true;

                    if (track.kalman) {
                        track.kalman->predict();
                        cv::Mat measurement = (cv::Mat_<float>(4, 1) << 
                            rc.x, rc.y, (float)motionRegions[i].width, (float)motionRegions[i].height);
                        cv::Mat estimated = track.kalman->correct(measurement);
                        track.smoothedCenter.x    = estimated.at<float>(0);
                        track.smoothedCenter.y    = estimated.at<float>(1);
                        track.smoothedSize.width  = estimated.at<float>(2);
                        track.smoothedSize.height = estimated.at<float>(3);
                    }

                    regionMatched[i]    = true;
                    trackMatched[best]  = true;
                }
            }
            for (size_t i = 0; i < motionRegions.size(); ++i) {
                if (regionMatched[i]) continue;
                if (motionRegions[i].area() >= settings.motionMinArea) {
                    WorkerMotionTrack t;
                    t.id       = m_nextMotionId++;
                    t.box      = motionRegions[i];
                    t.lastSeen = nowTP;
                    t.active   = true;

                    t.smoothedCenter = cv::Point2f(t.box.x + t.box.width / 2.0f, t.box.y + t.box.height / 2.0f);
                    t.smoothedSize   = cv::Size2f((float)t.box.width, (float)t.box.height);

                    t.kalman = std::make_unique<cv::KalmanFilter>(4, 4, 0);
                    t.kalman->transitionMatrix = cv::Mat::eye(4, 4, CV_32F);
                    t.kalman->measurementMatrix = cv::Mat::eye(4, 4, CV_32F);
                    cv::setIdentity(t.kalman->processNoiseCov, cv::Scalar::all(1e-4));
                    cv::setIdentity(t.kalman->measurementNoiseCov, cv::Scalar::all(1e-2));
                    cv::setIdentity(t.kalman->errorCovPost, cv::Scalar::all(1.0));

                    t.kalman->statePost.at<float>(0) = t.smoothedCenter.x;
                    t.kalman->statePost.at<float>(1) = t.smoothedCenter.y;
                    t.kalman->statePost.at<float>(2) = t.smoothedSize.width;
                    t.kalman->statePost.at<float>(3) = t.smoothedSize.height;

                    m_workerMotionTracks.push_back(std::move(t));
                    trackMatched.push_back(false);
                    newMotionSpawned = true;
                }
            }
            for (size_t j = 0; j < m_workerMotionTracks.size(); ++j)
                if (!trackMatched[j]) m_workerMotionTracks[j].active = false;

            // Sub-zoom slot management (NMS + persistent slot assignment)
            if (settings.subZoomsEnabled) {
                std::vector<int> uniqueIdx;
                for (size_t i = 0; i < m_workerMotionTracks.size(); ++i) {
                    bool tooClose = false;
                    for (int ui : uniqueIdx) {
                        cv::Rect inter = m_workerMotionTracks[i].box & m_workerMotionTracks[ui].box;
                        if (inter.area() > 0) {
                            float iou = static_cast<float>(inter.area()) /
                                        (m_workerMotionTracks[i].box.area() +
                                         m_workerMotionTracks[ui].box.area() - inter.area());
                            if (iou > 0.3f) { tooClose = true; break; }
                        }
                    }
                    if (!tooClose) uniqueIdx.push_back(static_cast<int>(i));
                }

                // Read previous sub-zoom state from Blackboard
                VisionState prevVision = m_blackboard.getVisionState();

                std::vector<bool> slotUpdated(4, false);

                auto cropSubZoom = [&](int slot, const WorkerMotionTrack& track) {
                    int pad  = settings.subZoomPaddingPx;
                    float mag = std::max(1.0f, settings.subZoomMagnification);
                    
                    int sw = std::max(8, static_cast<int>(std::round((track.smoothedSize.width + 2 * pad) / mag)));
                    int sh = std::max(8, static_cast<int>(std::round((track.smoothedSize.height + 2 * pad) / mag)));
                    
                    cv::getRectSubPix(trackingFrame, cv::Size(sw, sh), track.smoothedCenter, localSubZooms[slot].frame);
                };

                // Keep already-assigned slots that are still valid
                for (int i = 0; i < 4; ++i) {
                    if (!prevVision.subZooms[i].active) continue;
                    int assignedId = prevVision.subZooms[i].motion_id;
                    auto tit = std::find_if(m_workerMotionTracks.begin(), m_workerMotionTracks.end(),
                                            [assignedId](const WorkerMotionTrack& t) { return t.id == assignedId; });
                    if (tit == m_workerMotionTracks.end()) continue;
                    int tidx = static_cast<int>(std::distance(m_workerMotionTracks.begin(), tit));
                    if (std::find(uniqueIdx.begin(), uniqueIdx.end(), tidx) == uniqueIdx.end()) continue;

                    localSubZooms[i].active    = true;
                    localSubZooms[i].motion_id = tit->id;
                    localSubZooms[i].box       = tit->box;
                    localSubZooms[i].isLost    = !tit->active;
                    cropSubZoom(i, *tit);
                    slotUpdated[i] = true;
                }

                // Fill empty slots with remaining unique tracks
                for (int tidx : uniqueIdx) {
                    bool already = false;
                    for (int i = 0; i < 4; ++i)
                        if (slotUpdated[i] && localSubZooms[i].motion_id == m_workerMotionTracks[tidx].id)
                            { already = true; break; }
                    if (already) continue;
                    int free = -1;
                    for (int i = 0; i < 4; ++i)
                        if (!slotUpdated[i] && !localSubZooms[i].active) { free = i; break; }
                    if (free == -1) continue;
                    const auto& track = m_workerMotionTracks[tidx];
                    localSubZooms[free].active    = true;
                    localSubZooms[free].motion_id = track.id;
                    localSubZooms[free].box       = track.box;
                    localSubZooms[free].isLost    = !track.active;
                    cropSubZoom(free, track);
                    slotUpdated[free] = true;
                }
            }
        } else {
            m_workerMotionTracks.clear();
        }

        // Motion audio alert
        if (settings.motionDetectionEnabled && newMotionSpawned &&
            m_audioEngine.motionCooldownElapsed()) {
            m_audioEngine.playMotionAlert();
            m_audioEngine.recordMotionBeep();
        }

        // ── Consume detector results ────────────────────────────────────
        int  detectorLag       = 0;
        bool hasNewDetections  = false;

        if (m_detectorNewResults.load()) {
            std::lock_guard<std::mutex> lock(m_detectorMutex);
            m_detections          = m_detectorResults;
            m_detectorNewResults  = false;
            pendingDetections     = true;
            detectionTriggerFrame = static_cast<uint64_t>(m_detectorTriggerFrame.load());
        }

        AppStatusState curStatus = m_blackboard.getAppStatus();

        // ── Hand off to TrackingSystem ──────────────────────────────────
        if (m_trackingSystem) {
            if (pendingDetections) {
                hasNewDetections = true;
                detectorLag = static_cast<int>(
                    static_cast<uint64_t>(curStatus.totalFramesProcessed) - detectionTriggerFrame);
                pendingDetections = false;
            }

            // Build MotionTrack list for TrackingSystem
            std::vector<MotionTrack> mTracks;
            mTracks.reserve(m_workerMotionTracks.size());
            for (const auto& wmt : m_workerMotionTracks) {
                MotionTrack mt;
                mt.id     = wmt.id;
                mt.box    = wmt.box;
                mt.active = wmt.active;
                mt.lastSeen = wmt.lastSeen;
                mTracks.push_back(mt);
            }

            uint64_t nowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());

            m_trackingSystem->submitFrame(
                trackingFrame, settings,
                hasNewDetections ? m_detections : std::vector<Detection>(),
                detectorLag, nowMs - m_logSessionStartMs, mTracks);
        }

        // ── Data logging session management ────────────────────────────
        if (settings.dataLoggingEnabled && !m_blackboard.getSettings().dataLoggingEnabled) {
            // no-op: logging session management is handled by TrackingSystem
        }
        // Track when logging was first enabled for session timing
        if (settings.dataLoggingEnabled && m_logSessionStartMs == 0) {
            m_logSessionStartMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
        } else if (!settings.dataLoggingEnabled) {
            m_logSessionStartMs = 0;
        }

        // ── Trigger async detector ──────────────────────────────────────
        if (settings.enableDetection && !m_detectorBusy.load()) {
            bool runDetector = (frameSkipCounter == 0);
            frameSkipCounter = (frameSkipCounter + 1) % (settings.detectionSkipFrames + 1);
            if (runDetector) {
                std::lock_guard<std::mutex> lock(m_detectorMutex);
                m_detectorFrameCopy = trackingFrame; // Shallow copy
                m_detectorSettingsCopy = settings;
                m_detectorTriggerFrame = curStatus.totalFramesProcessed;
                m_detectorBusy         = true;
                m_detectorCv.notify_one();
            }
        }

        // ── Zoom crop ──────────────────────────────────────────────────
        TrackingStateData tState = m_blackboard.getTrackingState();
        if (tState.lockedTarget.state != TrackingState::SEARCHING && !rawFrame.empty()) {
            cv::Rect roi = tState.lockedTarget.box;
            const bool valid = (roi.width > 0 && roi.height > 0 &&
                                roi.x >= 0 && roi.y >= 0 &&
                                roi.x + roi.width  <= trackingFrame.cols &&
                                roi.y + roi.height <= trackingFrame.rows);
            if (valid) {
                cv::Mat sourceFrame = rawFrame;
                cv::Rect targetRoi  = roi;

                if (settings.enable4KZoom) {
                    double sx = (double)rawFrame.cols / (double)trackingFrame.cols;
                    double sy = (double)rawFrame.rows / (double)trackingFrame.rows;
                    targetRoi.x      = static_cast<int>(std::round(roi.x      * sx));
                    targetRoi.y      = static_cast<int>(std::round(roi.y      * sy));
                    targetRoi.width  = static_cast<int>(std::round(roi.width  * sx));
                    targetRoi.height = static_cast<int>(std::round(roi.height * sy));
                } else {
                    sourceFrame = trackingFrame;
                }

                float zoomMag = std::max(1.0f, settings.targetZoomMagnification);
                if (zoomMag > 1.0f) {
                    int cx = targetRoi.x + targetRoi.width  / 2;
                    int cy = targetRoi.y + targetRoi.height / 2;
                    int zW = std::max(8, static_cast<int>(std::round(targetRoi.width  / zoomMag)));
                    int zH = std::max(8, static_cast<int>(std::round(targetRoi.height / zoomMag)));
                    targetRoi = cv::Rect(cx - zW / 2, cy - zH / 2, zW, zH);
                }

                int pad_w = static_cast<int>(targetRoi.width  * 0.15f);
                int pad_h = static_cast<int>(targetRoi.height * 0.15f);
                int x1 = std::max(0, targetRoi.x - pad_w);
                int y1 = std::max(0, targetRoi.y - pad_h);
                int x2 = std::min(sourceFrame.cols, targetRoi.x + targetRoi.width  + pad_w);
                int y2 = std::min(sourceFrame.rows, targetRoi.y + targetRoi.height + pad_h);

                if (x2 > x1 && y2 > y1) {
                    sourceFrame(cv::Rect(x1, y1, x2 - x1, y2 - y1)).copyTo(zoomCrop);
                    if (settings.enable4KZoom && settings.lowLightEnhancement && !zoomCrop.empty())
                        ImageUtils::enhanceLowLight(zoomCrop, zoomLab, zoomChannels, zoomClahe,
                                                     settings.lowLightClipLimit, settings.lowLightDenoiseKernel);
                } else {
                    zoomCrop.release();
                }
            } else {
                zoomCrop.release();
            }
        } else {
            zoomCrop.release();
        }

        // ── Push VisionState to Blackboard ─────────────────────────────
        VisionState vState;
        if (!heatmapFrame.empty()) vState.heatmapFrame = heatmapFrame;
        vState.zoomCrop      = zoomCrop;
        vState.cameraWidth   = rawFrame.cols;
        vState.cameraHeight  = rawFrame.rows;
        vState.trackingWidth  = trackingFrame.cols;
        vState.trackingHeight = trackingFrame.rows;
        vState.zoomWidth     = zoomCrop.cols;
        vState.zoomHeight    = zoomCrop.rows;
        vState.cameraFps     = currentFps;
        for (int i = 0; i < 4; ++i) vState.subZooms[i] = localSubZooms[i];
        m_blackboard.setVisionState(vState);

        // Push detection state
        DetectionState dState;
        dState.detections    = m_detections;
        dState.motionRegions = motionRegions;
        for (const auto& wmt : m_workerMotionTracks) {
            MotionTrack mt;
            mt.id = wmt.id; mt.box = wmt.box; mt.active = wmt.active; mt.lastSeen = wmt.lastSeen;
            dState.motionTracks.push_back(mt);
        }
        m_blackboard.setDetectionState(dState);
    }

    m_log(LogLevel::INFO, "Worker thread stopping");
}
