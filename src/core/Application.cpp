#include "Application.hpp"
#include <iostream>
#include <cstring>
#include <cmath>
#include <cinttypes>
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// -----------------------------------------------------------------------
// Helper: convert a packed ImU32 (AABBGGRR) to float[4] RGBA
// -----------------------------------------------------------------------
static void ImU32ToFloat4(ImU32 col, float out[4]) {
    out[0] = ((col >>  0) & 0xFF) / 255.0f;  // R
    out[1] = ((col >>  8) & 0xFF) / 255.0f;  // G
    out[2] = ((col >> 16) & 0xFF) / 255.0f;  // B
    out[3] = ((col >> 24) & 0xFF) / 255.0f;  // A
}

static ImU32 Float4ToImU32(const float col[4]) {
    return IM_COL32(
        static_cast<int>(col[0] * 255),
        static_cast<int>(col[1] * 255),
        static_cast<int>(col[2] * 255),
        static_cast<int>(col[3] * 255)
    );
}

static float GetAppSeconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

// -----------------------------------------------------------------------
// Construction / Init
// -----------------------------------------------------------------------

Application::Application()
    : m_window(nullptr), m_width(1280), m_height(720),
      m_title("Project Horus - Target Viewer"),
      m_lockRequested(false), m_requestedLockId(-1), m_releaseLockRequested(false),
      m_running(false), m_newDataAvailable(false) {

    m_appStart = std::chrono::steady_clock::now();
    m_lastRenderTime = m_appStart;

    m_fpsHistory.assign(kFpsHistorySize, 0.0f);

    m_camera       = std::make_unique<CameraModule>();
    m_renderer     = std::make_unique<VideoRenderer>();
    m_zoomRenderer = std::make_unique<VideoRenderer>();
    m_hud          = std::make_unique<HUD>();
    m_tracker      = std::make_unique<MultiTracker>();
    m_dataLogger   = std::make_unique<DataLogger>();
    m_roiManager   = std::make_unique<ROIManager>();

    log(LogLevel::INFO, "Application constructed");
}

Application::~Application() {
    cleanup();
}

void Application::log(LogLevel level, const std::string& msg) {
    if (static_cast<int>(level) < m_settings.logLevel) return;
    
    ConsoleEntry e;
    e.level     = level;
    e.message   = msg;
    e.timestamp = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_appStart).count();
    
    {
        std::lock_guard<std::mutex> lk(m_consoleMutex);
        m_consoleLog.push_back(std::move(e));
        if (m_consoleLog.size() > kMaxLogEntries)
            m_consoleLog.pop_front();
    }

    // Mirror to console
    if (level == LogLevel::ERR) std::cerr << "[ERR] " << msg << std::endl;
    else if (level == LogLevel::WARN) std::cout << "[WARN] " << msg << std::endl;
    else std::cout << "[INFO] " << msg << std::endl;
}

bool Application::init(int argc, char** argv) {
    if (argc > 1) m_cameraAddress = argv[1];
    else          m_cameraAddress = "0";

    log(LogLevel::INFO, "Opening camera: " + m_cameraAddress);
    if (!m_camera->open(m_cameraAddress)) {
        log(LogLevel::ERR, "Camera failed to open: " + m_cameraAddress);
        std::cerr << "[ERROR] Camera fail: " << m_cameraAddress << std::endl;
        return false;
    }
    log(LogLevel::INFO, "Camera opened successfully");

    try {
        std::string modelPath  = "../Resources/yolov8n.onnx";
        std::string labelsPath = "../Resources/coco.txt";
        FILE* f = fopen(modelPath.c_str(), "r");
        if (!f) { modelPath = "../assets/models/yolov8n.onnx"; labelsPath = "../assets/models/coco.txt"; f = fopen(modelPath.c_str(), "r"); }
        if (!f) { modelPath = "assets/models/yolov8n.onnx";    labelsPath = "assets/models/coco.txt";    f = fopen(modelPath.c_str(), "r"); }
        if (f) fclose(f);
        log(LogLevel::INFO, "Loading model: " + modelPath);
        m_detector = std::make_unique<ObjectDetector>(modelPath, labelsPath);
        log(LogLevel::INFO, "Detector initialized");
    } catch (const std::exception& e) {
        log(LogLevel::ERR, std::string("Detector init failed: ") + e.what());
        std::cerr << "[ERROR] Detector fail: " << e.what() << std::endl;
        return false;
    }

    if (!initGLFW()) { log(LogLevel::ERR, "GLFW init failed"); return false; }
    if (!initImGui()) { log(LogLevel::ERR, "ImGui init failed"); return false; }

    // Initialize color float arrays from defaults
    ImU32ToFloat4(IM_COL32(0, 200, 100, 220), m_hudColorF);
    ImU32ToFloat4(IM_COL32(255, 180, 0, 255),  m_targetColorF);

    m_settings.hudColor    = Float4ToImU32(m_hudColorF);
    m_settings.targetColor = Float4ToImU32(m_targetColorF);

    log(LogLevel::INFO, "System initialized — starting worker thread");

    m_running = true;
    m_workerThread = std::thread(&Application::workerLoop, this);
    return true;
}

bool Application::initGLFW() {
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    m_window = glfwCreateWindow(m_width, m_height, m_title.c_str(), NULL, NULL);
    if (!m_window) return false;
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);
    if (glewInit() != GLEW_OK) return false;
    return true;
}

bool Application::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ImGui_ImplGlfw_InitForOpenGL(m_window, true);
    ImGui_ImplOpenGL3_Init("#version 150");
    return true;
}

// -----------------------------------------------------------------------
// Worker Thread
// -----------------------------------------------------------------------

void Application::workerLoop() {
    cv::Mat frame;
    float currentFps = 0.0f;
    auto lastTime = std::chrono::steady_clock::now();
    int frameSkipCounter = 0;

    while (m_running) {
        // ── Camera hot-swap ────────────────────────────────────────────
        if (m_cameraChangeRequested.load()) {
            std::string newAddr;
            {
                std::lock_guard<std::mutex> lk(m_cameraChangeMutex);
                newAddr = m_pendingCameraAddress;
            }
            m_cameraChangeRequested.store(false);

            log(LogLevel::INFO, "Hot-swapping camera to: " + newAddr);
            m_camera->close();  // release old
            bool ok = m_camera->open(newAddr);

            // Write result back (read from render thread for status display)
            {
                std::lock_guard<std::mutex> lk(m_cameraChangeMutex);
                m_cameraStatus   = ok ? "OK — " + newAddr : "FAILED — " + newAddr;
                m_cameraStatusOk = ok;
            }
            if (ok) {
                m_cameraAddress = newAddr;
                log(LogLevel::INFO, "Camera opened: " + newAddr);
            } else {
                log(LogLevel::ERR, "Camera failed to open: " + newAddr);
            }
        }

        if (!m_camera->read(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        currentFps = 1.0f / std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        SystemSettings currentSettings;
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            currentSettings = m_sharedSettings;
        }

        std::vector<Detection> detections;
        if (currentSettings.enableDetection) {
            bool runDetector = (frameSkipCounter == 0);
            frameSkipCounter = (frameSkipCounter + 1) % (currentSettings.detectionSkipFrames + 1);

            if (runDetector) {
                cv::Mat inputFrame = frame;
                if (currentSettings.grayscaleInput) {
                    cv::Mat gray;
                    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
                    cv::cvtColor(gray, inputFrame, cv::COLOR_GRAY2BGR);
                }
                detections = m_detector->detect(inputFrame, currentSettings);

                // ── ROI Filter (Plan 04) ──────────────────────────────────
                // Filter out detections outside active ROI zones.
                // If no zone is active, all detections pass through unchanged.
                if (m_roiManager->hasActiveROI()) {
                    m_roiManager->filterDetections(detections);
                }

                m_workerDetectionCount.store(static_cast<int>(detections.size()));
            }
        }

        std::vector<TrackedObject> tracked;
        if (currentSettings.enableTracking) {
            m_tracker->update(detections, currentSettings);
            tracked = m_tracker->getTrackedObjects(currentSettings.trackerMaxTrailLength);
            m_workerTrackCount.store(static_cast<int>(tracked.size()));

            // ── Alarm Zone Checks ───────────────────────────────────────
            auto zones = m_roiManager->getROIs();
            std::unordered_set<int> activeZoneIds;
            for (const auto& z : zones) {
                if (z.active && z.function == ROIFunction::ALARM) {
                    activeZoneIds.insert(z.id);
                    std::unordered_set<int> currentTracksInZone;
                    for (const auto& obj : tracked) {
                        cv::Point center(
                            obj.box.x + obj.box.width  / 2,
                            obj.box.y + obj.box.height / 2);
                        if (z.rect.contains(center)) {
                            currentTracksInZone.insert(obj.track_id);
                        }
                    }

                    // Detect entry
                    for (int tid : currentTracksInZone) {
                        if (m_activeAlarms[z.id].find(tid) == m_activeAlarms[z.id].end()) {
                            std::string clsName = "unknown";
                            for (const auto& obj : tracked) {
                                if (obj.track_id == tid) { clsName = obj.className; break; }
                            }
                            log(LogLevel::WARN, "ALARM: Object #" + std::to_string(tid) + " (" + clsName + ") entered Alarm Zone '" + z.label + "'");
                        }
                    }

                    // Detect exit
                    for (int tid : m_activeAlarms[z.id]) {
                        if (currentTracksInZone.find(tid) == currentTracksInZone.end()) {
                            log(LogLevel::INFO, "Object #" + std::to_string(tid) + " left Alarm Zone '" + z.label + "'");
                        }
                    }

                    m_activeAlarms[z.id] = currentTracksInZone;
                }
            }

            // Cleanup removed or deactivated zones
            for (auto it = m_activeAlarms.begin(); it != m_activeAlarms.end(); ) {
                if (activeZoneIds.find(it->first) == activeZoneIds.end()) {
                    it = m_activeAlarms.erase(it);
                } else {
                    ++it;
                }
            }
        }
        m_totalFramesProcessed.fetch_add(1);

        // ── Data-Logging (Plan 03) ────────────────────────────────────────
        // Start/stop logger based on UI-controlled setting.
        if (currentSettings.dataLoggingEnabled && !m_dataLogger->isOpen()) {
            // Logging was just turned on — open a new file
            LogFormat fmt = (currentSettings.dataLoggingFormat == 1)
                ? LogFormat::JSON : LogFormat::CSV;
            if (m_dataLogger->open(currentSettings.dataLoggingOutputDir, fmt)) {
                log(LogLevel::INFO, "Data logging started: " + m_dataLogger->getCurrentPath());
            } else {
                log(LogLevel::ERR, "Data logger failed to open file");
            }
            m_logFrameCounter = 0;
            // Record session start time in ms
            m_logSessionStartMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
        } else if (!currentSettings.dataLoggingEnabled && m_dataLogger->isOpen()) {
            // Logging was just turned off — close file
            uint64_t rows = m_dataLogger->getRowsWritten();
            m_dataLogger->close();
            log(LogLevel::INFO, "Data logging stopped. Rows written: " + std::to_string(rows));
        }

        if (currentSettings.dataLoggingEnabled && m_dataLogger->isOpen()) {
            m_logFrameCounter++;
            if (m_logFrameCounter >= currentSettings.dataLoggingFreqFrames) {
                m_logFrameCounter = 0;
                // Compute monotonic timestamp in ms from session start
                uint64_t nowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                double sessionMs = static_cast<double>(nowMs - m_logSessionStartMs);
                m_dataLogger->logFrame(sessionMs, tracked, 0.0 /* no calibration yet */);
            }
        }

        // --- Handle Target Locking Logic ---
        if (m_lockRequested.exchange(false)) {
            m_sharedLockedTarget.track_id = m_requestedLockId.load();
            m_sharedLockedTarget.state = TrackingState::LOCKED;
        }

        if (m_releaseLockRequested.exchange(false)) {
            m_sharedLockedTarget.state = TrackingState::SEARCHING;
            m_sharedLockedTarget.track_id = -1;
        }

        if (m_sharedLockedTarget.state != TrackingState::SEARCHING) {
            bool found = false;
            for (const auto& t : tracked) {
                if (t.track_id == m_sharedLockedTarget.track_id) {
                    m_sharedLockedTarget.box = t.box;
                    m_sharedLockedTarget.className = t.className;
                    m_sharedLockedTarget.confidence = t.confidence;
                    m_sharedLockedTarget.trail = t.trail;
                    found = true;
                    break;
                }
            }
            if (!found) m_sharedLockedTarget.state = TrackingState::LOST;
            else        m_sharedLockedTarget.state = TrackingState::LOCKED;
        }

        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            m_sharedFrame            = frame.clone();
            m_sharedDetections       = detections;
            m_sharedTrackedObjects   = tracked;
            m_sharedCameraFps        = currentFps;
            m_newDataAvailable       = true;
        }
    }
}

// -----------------------------------------------------------------------
// Target Locking
// -----------------------------------------------------------------------

void Application::handleTargetLocking(const ViewportInfo& view) {
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        // Check if mouse click is inside the actual video viewport bounds
        if (mousePos.x >= view.pos_x && mousePos.x <= view.pos_x + view.target_w &&
            mousePos.y >= view.pos_y && mousePos.y <= view.pos_y + view.target_h) {
            float videoX = (mousePos.x - view.pos_x) / view.scale;
            float videoY = (mousePos.y - view.pos_y) / view.scale;
            cv::Point clickPoint(static_cast<int>(videoX), static_cast<int>(videoY));
            for (const auto& obj : m_trackedObjects) {
                if (obj.box.contains(clickPoint)) {
                    m_requestedLockId.store(obj.track_id);
                    m_lockRequested.store(true);
                    log(LogLevel::INFO, "Target lock requested: " + obj.className +
                        " TrackID=" + std::to_string(obj.track_id));
                    break;
                }
            }
        }
    }
}

// -----------------------------------------------------------------------
// UI: Camera View
// -----------------------------------------------------------------------

void Application::renderCameraView() {
    ImGui::Begin("Camera View", nullptr,
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ViewportInfo view = {0};
    cv::Mat currentFrame;
    {
        // We already updated currentFrame in run(); just use member data
    }

    // This is called from run() which already holds currentFrame — see below
    // The actual rendering is inlined in run() to access currentFrame directly.
    ImGui::End();
}

// -----------------------------------------------------------------------
// UI: Data Panel
// -----------------------------------------------------------------------

void Application::renderDataPanel() {
    ImGui::Begin("Data Panel");

    // Summary metrics
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.5f, 1.0f), "TRACKS: %zu", m_trackedObjects.size());
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.0f, 1.0f), "  |  CAM FPS: %.1f", m_cameraFps);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "  |  RENDER FPS: %.1f", m_renderFps);
    ImGui::Separator();

    if (ImGui::BeginTable("Tracks", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed, 35.0f);
        ImGui::TableSetupColumn("Class",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Pos (X,Y)",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Conf",     ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("State",    ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableHeadersRow();

        for (const auto& obj : m_trackedObjects) {
            ImGui::TableNextRow();
            
            bool isSelected = (m_lockedTarget.state != TrackingState::SEARCHING && m_lockedTarget.track_id == obj.track_id);
            
            ImGui::TableSetColumnIndex(0);
            char idLabel[32];
            snprintf(idLabel, sizeof(idLabel), "%03d", obj.track_id);
            if (ImGui::Selectable(idLabel, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                if (isSelected) {
                    m_releaseLockRequested.store(true);
                } else {
                    m_requestedLockId.store(obj.track_id);
                    m_lockRequested.store(true);
                }
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", obj.className.c_str());

            ImGui::TableSetColumnIndex(2);
            float cx = static_cast<float>(obj.box.x) + obj.box.width  / 2.0f;
            float cy = static_cast<float>(obj.box.y) + obj.box.height / 2.0f;
            ImGui::Text("%.0f, %.0f", cx, cy);

            ImGui::TableSetColumnIndex(3);
            ImVec4 confColor = (obj.confidence > 0.6f)
                ? ImVec4(0.0f, 1.0f, 0.4f, 1.0f)
                : (obj.confidence > 0.4f ? ImVec4(1.0f, 0.8f, 0.0f, 1.0f)
                                         : ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextColored(confColor, "%.2f", obj.confidence);

            ImGui::TableSetColumnIndex(4);
            if (isSelected)
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "LOCKED");
            else if (obj.is_confirmed)
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "LOCK");
            else if (obj.lost_frames > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "LOST");
            else
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "INIT");
        }
        ImGui::EndTable();
    }
    
    if (m_lockedTarget.state != TrackingState::SEARCHING) {
        ImGui::Separator();
        if (ImGui::Button("Release Lock", ImVec2(-FLT_MIN, 0))) {
            m_releaseLockRequested.store(true);
        }
    }
    
    ImGui::End();
}

// -----------------------------------------------------------------------
// UI: Zoom Window
// -----------------------------------------------------------------------
static ImU32 ApplyBrightnessLocal(ImU32 col, float brightness) {
    float r = ((col >>  0) & 0xFF) * brightness;
    float g = ((col >>  8) & 0xFF) * brightness;
    float b = ((col >> 16) & 0xFF) * brightness;
    float a = ((col >> 24) & 0xFF);
    return IM_COL32(
        static_cast<int>(r),
        static_cast<int>(g),
        static_cast<int>(b),
        static_cast<int>(a)
    );
}

void Application::renderZoomWindow(const cv::Mat& currentFrame) {
    ImGui::Begin("Target Zoom");

    if (m_lockedTarget.state != TrackingState::SEARCHING && !currentFrame.empty()) {
        cv::Rect roi = m_lockedTarget.box;

        // Validate bounding box before any crop operation
        const bool bbox_valid = (roi.width > 0 && roi.height > 0 &&
                                  roi.x >= 0 && roi.y >= 0 &&
                                  roi.x + roi.width  <= currentFrame.cols &&
                                  roi.y + roi.height <= currentFrame.rows);
        if (bbox_valid) {
        // Add 15% padding around the target bounding box
        int pad_w = static_cast<int>(roi.width * 0.15f);
        int pad_h = static_cast<int>(roi.height * 0.15f);
        int x1 = std::max(0, roi.x - pad_w);
        int y1 = std::max(0, roi.y - pad_h);
        int x2 = std::min(currentFrame.cols, roi.x + roi.width + pad_w);
        int y2 = std::min(currentFrame.rows, roi.y + roi.height + pad_h);

        if (x2 > x1 && y2 > y1) {
            cv::Mat cropped = currentFrame(cv::Rect(x1, y1, x2 - x1, y2 - y1));
            m_zoomRenderer->updateTexture(cropped);

            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 pos   = ImGui::GetCursorScreenPos();

            float frame_aspect  = static_cast<float>(cropped.cols) / static_cast<float>(cropped.rows);
            float window_aspect = avail.x / avail.y;

            float target_w, target_h;
            float draw_x, draw_y;

            if (window_aspect > frame_aspect) {
                target_h = avail.y;
                target_w = target_h * frame_aspect;
                draw_x    = pos.x + (avail.x - target_w) / 2.0f;
                draw_y    = pos.y;
            } else {
                target_w = avail.x;
                target_h = target_w / frame_aspect;
                draw_y    = pos.y + (avail.y - target_h) / 2.0f;
                draw_x    = pos.x;
            }

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddImage(reinterpret_cast<void*>(static_cast<intptr_t>(m_zoomRenderer->getTextureID())),
                               ImVec2(draw_x, draw_y),
                               ImVec2(draw_x + target_w, draw_y + target_h));

            // Resolve and apply colors
            ImU32 hudColor = (m_settings.hudColor != 0) ? m_settings.hudColor : IM_COL32(0, 200, 100, 220);
            ImU32 targetColor = (m_settings.targetColor != 0) ? m_settings.targetColor : IM_COL32(255, 180, 0, 255);
            
            hudColor = ApplyBrightnessLocal(hudColor, m_settings.hudBrightness);
            targetColor = ApplyBrightnessLocal(targetColor, m_settings.hudBrightness);

            // Draw center reticle/crosshair in target zoom view
            ImVec2 zoomCenter = ImVec2(draw_x + target_w / 2.0f, draw_y + target_h / 2.0f);
            float reticleSize = 15.0f;
            drawList->AddLine(ImVec2(zoomCenter.x - reticleSize, zoomCenter.y), ImVec2(zoomCenter.x - 4, zoomCenter.y), targetColor, 1.0f);
            drawList->AddLine(ImVec2(zoomCenter.x + 4, zoomCenter.y), ImVec2(zoomCenter.x + reticleSize, zoomCenter.y), targetColor, 1.0f);
            drawList->AddLine(ImVec2(zoomCenter.x, zoomCenter.y - reticleSize), ImVec2(zoomCenter.x, zoomCenter.y - 4), targetColor, 1.0f);
            drawList->AddLine(ImVec2(zoomCenter.x, zoomCenter.y + 4), ImVec2(zoomCenter.x, zoomCenter.y + reticleSize), targetColor, 1.0f);
            drawList->AddCircle(zoomCenter, 6.0f, targetColor, 12, 1.0f);

            // Corner brackets inside the zoom view
            float bracketLen = 12.0f;
            // Top-left
            drawList->AddLine(ImVec2(draw_x, draw_y), ImVec2(draw_x + bracketLen, draw_y), hudColor, 1.5f);
            drawList->AddLine(ImVec2(draw_x, draw_y), ImVec2(draw_x, draw_y + bracketLen), hudColor, 1.5f);
            // Top-right
            drawList->AddLine(ImVec2(draw_x + target_w, draw_y), ImVec2(draw_x + target_w - bracketLen, draw_y), hudColor, 1.5f);
            drawList->AddLine(ImVec2(draw_x + target_w, draw_y), ImVec2(draw_x + target_w, draw_y + bracketLen), hudColor, 1.5f);
            // Bottom-left
            drawList->AddLine(ImVec2(draw_x, draw_y + target_h), ImVec2(draw_x + bracketLen, draw_y + target_h), hudColor, 1.5f);
            drawList->AddLine(ImVec2(draw_x, draw_y + target_h), ImVec2(draw_x, draw_y + target_h - bracketLen), hudColor, 1.5f);
            // Bottom-right
            drawList->AddLine(ImVec2(draw_x + target_w, draw_y + target_h), ImVec2(draw_x + target_w - bracketLen, draw_y + target_h), hudColor, 1.5f);
            drawList->AddLine(ImVec2(draw_x + target_w, draw_y + target_h), ImVec2(draw_x + target_w, draw_y + target_h - bracketLen), hudColor, 1.5f);

            // Overlay info text
            char infoText[128];
            snprintf(infoText, sizeof(infoText), "TRK ID: %03d | CLASS: %s", m_lockedTarget.track_id, m_lockedTarget.className.c_str());
            drawList->AddText(ImVec2(draw_x + 8.0f, draw_y + 8.0f), hudColor, infoText);

            snprintf(infoText, sizeof(infoText), "CONF: %.2f | SIZE: %dx%d", m_lockedTarget.confidence, roi.width, roi.height);
            drawList->AddText(ImVec2(draw_x + 8.0f, draw_y + 22.0f), hudColor, infoText);
        }   // end if (x2 > x1 && y2 > y1)
        }   // end if (bbox_valid)
        else {
            // Bounding box is degenerate or out of frame
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            const char* text = "INVALID TARGET BOUNDS";
            ImVec2 textSize = ImGui::CalcTextSize(text);
            ImGui::SetCursorScreenPos(ImVec2(pos.x + (avail.x - textSize.x) * 0.5f, pos.y + (avail.y - textSize.y) * 0.5f));
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", text);
        }
    } else {
        // Render cool standby grid and text
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImU32 gridColor = IM_COL32(0, 200, 100, 20);
        float step = 30.0f;
        for (float x = pos.x; x < pos.x + avail.x; x += step) {
            drawList->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + avail.y), gridColor);
        }
        for (float y = pos.y; y < pos.y + avail.y; y += step) {
            drawList->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + avail.x, y), gridColor);
        }

        const char* text = "ZOOM STANDBY\nNO TARGET LOCKED";
        ImVec2 textSize = ImGui::CalcTextSize(text);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + (avail.x - textSize.x) * 0.5f, pos.y + (avail.y - textSize.y) * 0.5f));
        ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 0.6f), "%s", text);
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// UI: Dev Console  (the big one)
// -----------------------------------------------------------------------

void Application::renderDevConsole() {
    ImGui::Begin("Dev Console");

    // ── Header bar ──────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.5f, 1.0f), "HORUS DEV CONSOLE");
    ImGui::SameLine();
    ImGui::TextDisabled("v1.10.1");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.0f);
    if (ImGui::Button("Settings...", ImVec2(110, 0)))
        m_showSettingsWindow = !m_showSettingsWindow;
    ImGui::Separator();

    // ── Tab bar ─────────────────────────────────────────────────────────
    bool settingsChanged = false;

    if (ImGui::BeginTabBar("DevTabBar")) {

        // ── TAB: System ────────────────────────────────────────────────
        if (ImGui::BeginTabItem("System")) {
            // Performance graph
            {
                char overlay[32];
                snprintf(overlay, sizeof(overlay), "%.1f fps", m_renderFps);
                ImGui::PlotLines("##fps", m_fpsHistory.data(),
                                 static_cast<int>(m_fpsHistory.size()),
                                 m_fpsHistoryIdx, overlay,
                                 0.0f, 120.0f, ImVec2(0, 60));
            }

            ImGui::Separator();

            // Two-column metric table
            ImGui::Columns(2, "sysmetrics", true);
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Render FPS");
            ImGui::NextColumn();
            ImGui::Text("%.2f", m_renderFps);
            ImGui::NextColumn();

            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Frame Time");
            ImGui::NextColumn();
            ImGui::Text("%.2f ms", m_frameTimeMs);
            ImGui::NextColumn();

            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Camera FPS");
            ImGui::NextColumn();
            ImGui::Text("%.2f", m_cameraFps);
            ImGui::NextColumn();

            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Active Tracks");
            ImGui::NextColumn();
            ImGui::Text("%d", m_workerTrackCount.load());
            ImGui::NextColumn();

            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Detections");
            ImGui::NextColumn();
            ImGui::Text("%d", m_workerDetectionCount.load());
            ImGui::NextColumn();

            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Total Frames");
            ImGui::NextColumn();
            ImGui::Text("%d", m_totalFramesProcessed.load());
            ImGui::NextColumn();
            ImGui::Columns(1);

            ImGui::Separator();

            // Pipeline toggles
            ImGui::TextDisabled("Pipeline");
            settingsChanged |= ImGui::Checkbox("Enable Detection",  &m_settings.enableDetection);
            ImGui::SameLine();
            settingsChanged |= ImGui::Checkbox("Enable Tracking",   &m_settings.enableTracking);
            settingsChanged |= ImGui::Checkbox("Grayscale Input (faster)", &m_settings.grayscaleInput);
            ImGui::SetNextItemWidth(160);
            settingsChanged |= ImGui::SliderInt("Detection Skip Frames",
                                                &m_settings.detectionSkipFrames, 0, 10);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Run detector every N+1 frames. 0 = every frame.");

            ImGui::Separator();

            // ── Camera Selection ────────────────────────────────────────
            ImGui::TextDisabled("Camera Source");

            // Status badge
            {
                std::string statusStr;
                bool statusOk = true;
                {
                    std::lock_guard<std::mutex> lk(m_cameraChangeMutex);
                    statusStr = m_cameraStatus;
                    statusOk  = m_cameraStatusOk;
                }
                if (statusStr.empty()) {
                    ImGui::TextColored(ImVec4(0.0f,0.9f,0.4f,1.0f),
                        "ACTIVE  [%s]", m_cameraAddress.c_str());
                } else if (statusOk) {
                    ImGui::TextColored(ImVec4(0.0f,1.0f,0.5f,1.0f),
                        "OK  [%s]", m_cameraAddress.c_str());
                } else {
                    ImGui::TextColored(ImVec4(1.0f,0.3f,0.3f,1.0f),
                        "FAILED  [%s]", statusStr.c_str());
                }
            }

            // Quick-select combo (camera indices 0–5 + common network URIs)
            ImGui::SetNextItemWidth(-1);
            static const char* kPresets[] = {
                "0  (built-in / first cam)",
                "1  (USB cam #1)",
                "2  (USB cam #2)",
                "3  (USB cam #3)",
                "4  (USB cam #4)",
                "5  (USB cam #5)",
                "rtsp://  (custom RTSP)",
                "http://  (custom HTTP)",
            };
            static int selectedPreset = -1;
            if (ImGui::BeginCombo("##campreset", "Quick Select...")) {
                for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i) {
                    if (ImGui::Selectable(kPresets[i])) {
                        selectedPreset = i;
                        if (i < 6) {
                            // Numeric index
                            snprintf(m_cameraInputBuf, sizeof(m_cameraInputBuf), "%d", i);
                        } else if (i == 6) {
                            strncpy(m_cameraInputBuf, "rtsp://", sizeof(m_cameraInputBuf));
                        } else {
                            strncpy(m_cameraInputBuf, "http://", sizeof(m_cameraInputBuf));
                        }
                    }
                }
                ImGui::EndCombo();
            }

            // Manual text input
            ImGui::SetNextItemWidth(-80);
            ImGui::InputText("##camaddr", m_cameraInputBuf, sizeof(m_cameraInputBuf),
                             ImGuiInputTextFlags_None);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Numeric ID (e.g. 0, 1) or full URI\n"
                                  "e.g. rtsp://192.168.1.10:554/stream\n"
                                  "     /dev/video0\n"
                                  "     http://cam.example.com/mjpg");
            ImGui::SameLine();

            // Apply button — triggers hot-swap in worker thread
            bool canApply = (m_cameraInputBuf[0] != '\0') &&
                            !m_cameraChangeRequested.load();
            if (!canApply) ImGui::BeginDisabled();
            if (ImGui::Button("Apply", ImVec2(-1, 0))) {
                std::string newAddr(m_cameraInputBuf);
                {
                    std::lock_guard<std::mutex> lk(m_cameraChangeMutex);
                    m_pendingCameraAddress = newAddr;
                    m_cameraStatus.clear();
                }
                m_cameraChangeRequested.store(true);
                log(LogLevel::INFO, "Camera change requested: " + newAddr);
            }
            if (!canApply) ImGui::EndDisabled();

            if (m_cameraChangeRequested.load())
                ImGui::TextColored(ImVec4(1.0f,0.8f,0.1f,1.0f), "  Switching...");

            ImGui::Separator();
            static bool confirmQuit = false;
            ImGui::Checkbox("Enable Admin Actions", &confirmQuit);
            if (confirmQuit) {
                if (ImGui::Button("Reset All Settings", ImVec2(-1, 0))) {
                    m_settings = SystemSettings{};
                    m_settings.hudColor    = Float4ToImU32(m_hudColorF);
                    m_settings.targetColor = Float4ToImU32(m_targetColorF);
                    settingsChanged = true;
                    log(LogLevel::WARN, "All settings reset to defaults");
                }
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
                if (ImGui::Button("Quit Application", ImVec2(-1, 0)))
                    glfwSetWindowShouldClose(m_window, true);
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("(Admin actions locked)");
            }

            ImGui::EndTabItem();

        }

        // ── TAB: Detector ──────────────────────────────────────────────
        if (ImGui::BeginTabItem("Detector")) {
            ImGui::TextDisabled("Thresholds");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Confidence##det",
                &m_settings.detectorConfThreshold, 0.01f, 1.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimum box confidence to keep a detection");

            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Score##det",
                &m_settings.detectorScoreThreshold, 0.01f, 1.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Class score threshold");

            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("NMS IoU##det",
                &m_settings.detectorNmsThreshold, 0.01f, 1.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Non-Maximum Suppression IoU threshold");

            ImGui::Separator();
            ImGui::TextDisabled("Class Filter");
            settingsChanged |= ImGui::Checkbox("Filter by Priority Classes",
                                               &m_settings.filterByPriorityClasses);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Only detect: person(0), bicycle(1), car(2), motorcycle(3), bus(5), truck(7)");

            if (!m_settings.filterByPriorityClasses) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
                    "  WARNING: All COCO classes active");
            }

            ImGui::Separator();
            ImGui::TextDisabled("Presets");
            if (ImGui::Button("Fast (low quality)")) {
                m_settings.detectorConfThreshold  = 0.4f;
                m_settings.detectorScoreThreshold = 0.4f;
                m_settings.detectorNmsThreshold   = 0.5f;
                m_settings.detectionSkipFrames    = 2;
                settingsChanged = true;
                log(LogLevel::INFO, "Detector preset: Fast");
            }
            ImGui::SameLine();
            if (ImGui::Button("Balanced")) {
                m_settings.detectorConfThreshold  = 0.25f;
                m_settings.detectorScoreThreshold = 0.25f;
                m_settings.detectorNmsThreshold   = 0.45f;
                m_settings.detectionSkipFrames    = 0;
                settingsChanged = true;
                log(LogLevel::INFO, "Detector preset: Balanced");
            }
            ImGui::SameLine();
            if (ImGui::Button("Precise")) {
                m_settings.detectorConfThreshold  = 0.10f;
                m_settings.detectorScoreThreshold = 0.10f;
                m_settings.detectorNmsThreshold   = 0.35f;
                m_settings.detectionSkipFrames    = 0;
                settingsChanged = true;
                log(LogLevel::INFO, "Detector preset: Precise");
            }

            ImGui::EndTabItem();
        }

        // ── TAB: Tracker ───────────────────────────────────────────────
        if (ImGui::BeginTabItem("Tracker")) {
            ImGui::TextDisabled("Matching");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Min Match Score",
                &m_settings.trackerMinMatchScore, 0.01f, 1.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Combined IoU+Distance score threshold for track-to-detection assignment");

            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Min Match IoU",
                &m_settings.trackerMinMatchIOU, 0.01f, 1.0f, "%.3f");

            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Max Center Dist (px)",
                &m_settings.trackerMaxCenterDistPx, 10.0f, 800.0f, "%.0f px");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum pixel distance between predicted and detected centroid for a valid match");

            ImGui::Separator();
            ImGui::TextDisabled("Track Lifecycle");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderInt("Max Lost Frames",
                &m_settings.trackerMaxLostFrames, 1, 120);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frames a track persists without a detection before being deleted");

            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderInt("Confirm Frames",
                &m_settings.trackerConfirmFrames, 1, 20);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Consecutive matches needed to confirm a track");

            ImGui::Separator();
            ImGui::TextDisabled("Trail");
            settingsChanged |= ImGui::Checkbox("Show Trails", &m_settings.showTrails);
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderInt("Trail Length",
                &m_settings.trackerMaxTrailLength, 5, 200);
            settingsChanged |= ImGui::Checkbox("Fading Trail Alpha", &m_settings.showTrailFade);

            ImGui::Separator();
            ImGui::TextDisabled("Dead Reckoning (Single-Target Lock)");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Velocity Smoothing",
                &m_settings.trackerVelocitySmoothing, 0.0f, 1.0f, "%.2f");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Damping",
                &m_settings.trackerDeadReckoningDamping, 0.0f, 1.0f, "%.2f");

            ImGui::Separator();
            ImGui::TextDisabled("Live Stats");
            ImGui::Text("Active Tracks : %d", m_workerTrackCount.load());
            ImGui::Text("Detections    : %d", m_workerDetectionCount.load());

            ImGui::EndTabItem();
        }

        // ── TAB: HUD ───────────────────────────────────────────────────
        if (ImGui::BeginTabItem("HUD")) {
            ImGui::TextDisabled("Visibility Toggles");

            bool c1 = m_settings.showCrosshair;
            bool c2 = m_settings.showTacticalOverlay;
            bool c3 = m_settings.showCornerBrackets;
            bool c4 = m_settings.showStatusWindows;
            bool c5 = m_settings.showTrackIDs;
            bool c6 = m_settings.showConfidence;

            settingsChanged |= ImGui::Checkbox("Crosshair",          &m_settings.showCrosshair);
            ImGui::SameLine();
            settingsChanged |= ImGui::Checkbox("Tactical Overlay",   &m_settings.showTacticalOverlay);
            settingsChanged |= ImGui::Checkbox("Corner Brackets",    &m_settings.showCornerBrackets);
            ImGui::SameLine();
            settingsChanged |= ImGui::Checkbox("Status Windows",     &m_settings.showStatusWindows);
            settingsChanged |= ImGui::Checkbox("Show Track IDs",     &m_settings.showTrackIDs);
            ImGui::SameLine();
            settingsChanged |= ImGui::Checkbox("Show Confidence",    &m_settings.showConfidence);

            ImGui::Separator();
            ImGui::TextDisabled("Style");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("HUD Brightness",
                &m_settings.hudBrightness, 0.2f, 1.0f, "%.2f");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Crosshair Scale",
                &m_settings.crosshairScale, 0.25f, 3.0f, "%.2f");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Box Line Width",
                &m_settings.boxLineWidth, 0.5f, 5.0f, "%.1f px");

            ImGui::Separator();
            ImGui::TextDisabled("Colors");

            if (ImGui::ColorEdit4("HUD Color", m_hudColorF,
                    ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_PickerHueWheel)) {
                m_settings.hudColor = Float4ToImU32(m_hudColorF);
                settingsChanged = true;
            }
            if (ImGui::ColorEdit4("Target Color", m_targetColorF,
                    ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_PickerHueWheel)) {
                m_settings.targetColor = Float4ToImU32(m_targetColorF);
                settingsChanged = true;
            }

            ImGui::EndTabItem();
        }

        // ── TAB: Console ───────────────────────────────────────────────
        if (ImGui::BeginTabItem("Console")) {
            // Toolbar
            ImGui::SetNextItemWidth(120);
            const char* levelNames[] = {"VERBOSE", "INFO", "WARN", "ERROR"};
            if (ImGui::BeginCombo("Min Level", levelNames[m_settings.logLevel])) {
                for (int i = 0; i < 4; ++i) {
                    bool sel = (m_settings.logLevel == i);
                    if (ImGui::Selectable(levelNames[i], sel)) {
                        m_settings.logLevel = i;
                        settingsChanged = true;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180);
            ImGui::InputText("Filter", m_logFilter, sizeof(m_logFilter));
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                std::lock_guard<std::mutex> lk(m_consoleMutex);
                m_consoleLog.clear();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-Scroll", &m_autoScrollLog);

            ImGui::Separator();

            // Scrollable log area
            ImGui::BeginChild("ConsoleScrollRegion", ImVec2(0, 0), false,
                              ImGuiWindowFlags_HorizontalScrollbar);

            std::lock_guard<std::mutex> lk(m_consoleMutex);
            for (const auto& entry : m_consoleLog) {
                // Apply log level filter
                if (static_cast<int>(entry.level) < m_settings.logLevel) continue;

                // Apply text filter
                if (m_logFilter[0] != '\0' &&
                    entry.message.find(m_logFilter) == std::string::npos) continue;

                ImVec4 col;
                const char* prefix;
                switch (entry.level) {
                    case LogLevel::VERBOSE: col = {0.6f,0.6f,0.6f,1.0f}; prefix = "[VRB]"; break;
                    case LogLevel::INFO:    col = {0.8f,0.9f,1.0f,1.0f}; prefix = "[INF]"; break;
                    case LogLevel::WARN:    col = {1.0f,0.8f,0.2f,1.0f}; prefix = "[WRN]"; break;
                    case LogLevel::ERR:     col = {1.0f,0.3f,0.3f,1.0f}; prefix = "[ERR]"; break;
                    default:               col = {1.0f,1.0f,1.0f,1.0f}; prefix = "[???]"; break;
                }

                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1.0f), "%6.2f", entry.timestamp);
                ImGui::SameLine();
                ImGui::TextColored(col, "%s %s", prefix, entry.message.c_str());
            }

            if (m_autoScrollLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f)
                ImGui::SetScrollHereY(1.0f);

            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ── TAB: Logging ───────────────────────────────────────────────
        if (ImGui::BeginTabItem("Logging")) {
            // Start / Stop button
            bool logActive = m_settings.dataLoggingEnabled;
            if (logActive) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("  STOP Logging  ", ImVec2(-1, 0))) {
                    m_settings.dataLoggingEnabled = false;
                    settingsChanged = true;
                }
                ImGui::PopStyleColor(2);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.55f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.75f, 0.15f, 1.0f));
                if (ImGui::Button("  START Logging  ", ImVec2(-1, 0))) {
                    m_settings.dataLoggingEnabled = true;
                    settingsChanged = true;
                }
                ImGui::PopStyleColor(2);
            }

            ImGui::Separator();
            ImGui::TextDisabled("Output Format");

            bool fmtDisabled = m_settings.dataLoggingEnabled;
            if (fmtDisabled) ImGui::BeginDisabled();
            static const char* kFmtNames[] = { "CSV", "JSON-Lines (.jsonl)" };
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::Combo("##logfmt", &m_settings.dataLoggingFormat,
                                            kFmtNames, IM_ARRAYSIZE(kFmtNames));
            if (ImGui::IsItemHovered() && !fmtDisabled)
                ImGui::SetTooltip("CSV: one row per tracked object per frame.\n"
                                  "JSON-Lines: one JSON object per line (streamable).");
            if (fmtDisabled) ImGui::EndDisabled();

            ImGui::Separator();
            ImGui::TextDisabled("Frequency");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderInt("Log every N frames##lfreq",
                &m_settings.dataLoggingFreqFrames, 1, 30);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("1 = log every frame. Higher = lower I/O overhead.");

            ImGui::Separator();
            ImGui::TextDisabled("Output Directory");
            static char s_logDirBuf[256] = {0};
            // Sync buffer on first render
            if (s_logDirBuf[0] == '\0' && !m_settings.dataLoggingOutputDir.empty())
                strncpy(s_logDirBuf, m_settings.dataLoggingOutputDir.c_str(), sizeof(s_logDirBuf)-1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##logdir", s_logDirBuf, sizeof(s_logDirBuf))) {
                m_settings.dataLoggingOutputDir = s_logDirBuf;
                settingsChanged = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Leave empty to write into the current working directory.");

            ImGui::Separator();
            ImGui::TextDisabled("Session Status");
            if (m_dataLogger->isOpen()) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "RECORDING");
                ImGui::Text("File : %s", m_dataLogger->getCurrentPath().c_str());
                ImGui::Text("Rows : %" PRIu64, m_dataLogger->getRowsWritten());
                uint64_t kb = m_dataLogger->getBytesWritten() / 1024;
                ImGui::Text("Size : %" PRIu64 " KB", kb);
            } else {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "IDLE");
            }

            ImGui::EndTabItem();
        }

        // ── TAB: ROI ───────────────────────────────────────────────────
        if (ImGui::BeginTabItem("ROI")) {
            ImGui::TextDisabled("Region of Interest Management");

            // Edit mode toggle
            if (m_roiEditMode) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.5f, 0.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.65f, 0.0f, 1.0f));
                if (ImGui::Button("  Exit Edit Mode  ", ImVec2(-1, 0))) {
                    m_roiEditMode = false;
                    if (m_roiManager->isDragging()) m_roiManager->cancelDrag();
                }
                ImGui::PopStyleColor(2);
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                    "  Drag in Camera View to draw a zone.");
            } else {
                // Check capacity
                bool atCapacity = (static_cast<int>(m_roiManager->getROIs().size()) >= ROIManager::kMaxZones);
                if (atCapacity) ImGui::BeginDisabled();
                if (ImGui::Button("  Enter Edit Mode  ", ImVec2(-1, 0))) {
                    m_roiEditMode = true;
                }
                if (atCapacity) {
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "MAX 4 ZONES");
                }
            }

            if (ImGui::Button("Clear All Zones", ImVec2(-1, 0))) {
                auto zones = m_roiManager->getROIs();
                for (const auto& z : zones) m_roiManager->removeROI(z.id);
            }

            settingsChanged |= ImGui::Checkbox("Show ROI Overlay##roi", &m_settings.showROIOverlay);

            ImGui::Separator();
            ImGui::TextDisabled("Active Zones");

            auto zones = m_roiManager->getROIs();
            if (zones.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  No zones defined.");
            } else {
                if (ImGui::BeginTable("ROIZones", 5,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("ID",     ImGuiTableColumnFlags_WidthFixed, 30.0f);
                    ImGui::TableSetupColumn("Label",  ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Rect",   ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Ctrl",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableHeadersRow();

                    for (auto& z : zones) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%d", z.id);

                        ImGui::TableSetColumnIndex(1);
                        // Inline label edit
                        int zidxBuf = z.id % ROIManager::kMaxZones;
                        // Sync buffer if label changed externally
                        if (strncmp(m_roiLabelBuf[zidxBuf], z.label.c_str(), 63) != 0 &&
                            m_roiLabelBuf[zidxBuf][0] == '\0') {
                            strncpy(m_roiLabelBuf[zidxBuf], z.label.c_str(), 63);
                        }
                        char labelId[16];
                        snprintf(labelId, sizeof(labelId), "##lbl%d", z.id);
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText(labelId, m_roiLabelBuf[zidxBuf], 64)) {
                            m_roiManager->setLabel(z.id, m_roiLabelBuf[zidxBuf]);
                        }

                        ImGui::TableSetColumnIndex(2);
                        // Inline function selector
                        const char* funcNames[] = { "Detection (Inc)", "Exclude (Exc)", "Alarm (Alm)" };
                        int currentFunc = static_cast<int>(z.function);
                        char funcId[16];
                        snprintf(funcId, sizeof(funcId), "##func%d", z.id);
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::Combo(funcId, &currentFunc, funcNames, 3)) {
                            m_roiManager->setFunction(z.id, static_cast<ROIFunction>(currentFunc));
                        }

                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%d,%d %dx%d", z.rect.x, z.rect.y,
                                    z.rect.width, z.rect.height);

                        ImGui::TableSetColumnIndex(4);
                        char togId[16], delId[16];
                        snprintf(togId, sizeof(togId), "%s##t%d",
                                 z.active ? "Dis" : "Ena", z.id);
                        snprintf(delId, sizeof(delId), "Del##d%d", z.id);

                        if (ImGui::SmallButton(togId))
                            m_roiManager->toggleROI(z.id);
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,1.0f));
                        if (ImGui::SmallButton(delId))
                            m_roiManager->removeROI(z.id);
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndTable();
                }
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // Push changed settings to worker thread
    if (settingsChanged) {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_sharedSettings = m_settings;
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// UI: Settings Window (floating, dockable)
// -----------------------------------------------------------------------

void Application::renderSettingsWindow() {
    if (!m_showSettingsWindow) return;

    ImGui::SetNextWindowSize(ImVec2(420, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &m_showSettingsWindow)) {
        ImGui::End();
        return;
    }

    bool changed = false;
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.5f, 1.0f), "APPLICATION SETTINGS");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::SliderFloat("HUD Brightness##s",
            &m_settings.hudBrightness, 0.2f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Crosshair Scale##s",
            &m_settings.crosshairScale, 0.25f, 3.0f, "%.2f");
        changed |= ImGui::SliderFloat("Box Line Width##s",
            &m_settings.boxLineWidth, 0.5f, 5.0f, "%.1f");
        if (ImGui::ColorEdit4("HUD Color##s",    m_hudColorF,
                ImGuiColorEditFlags_AlphaBar)) {
            m_settings.hudColor = Float4ToImU32(m_hudColorF);
            changed = true;
        }
        if (ImGui::ColorEdit4("Target Color##s", m_targetColorF,
                ImGuiColorEditFlags_AlphaBar)) {
            m_settings.targetColor = Float4ToImU32(m_targetColorF);
            changed = true;
        }
    }

    if (ImGui::CollapsingHeader("HUD Elements")) {
        changed |= ImGui::Checkbox("Crosshair##s",           &m_settings.showCrosshair);
        changed |= ImGui::Checkbox("Tactical Overlay##s",    &m_settings.showTacticalOverlay);
        changed |= ImGui::Checkbox("Corner Brackets##s",     &m_settings.showCornerBrackets);
        changed |= ImGui::Checkbox("Status Windows##s",      &m_settings.showStatusWindows);
        changed |= ImGui::Checkbox("Show Track IDs##s",      &m_settings.showTrackIDs);
        changed |= ImGui::Checkbox("Show Confidence##s",     &m_settings.showConfidence);
        changed |= ImGui::Checkbox("Show Trails##s",         &m_settings.showTrails);
        changed |= ImGui::Checkbox("Fading Trail Alpha##s",  &m_settings.showTrailFade);
    }

    if (ImGui::CollapsingHeader("Detector")) {
        changed |= ImGui::SliderFloat("Confidence##ds",  &m_settings.detectorConfThreshold,  0.01f, 1.0f, "%.3f");
        changed |= ImGui::SliderFloat("Score##ds",       &m_settings.detectorScoreThreshold, 0.01f, 1.0f, "%.3f");
        changed |= ImGui::SliderFloat("NMS##ds",         &m_settings.detectorNmsThreshold,   0.01f, 1.0f, "%.3f");
        changed |= ImGui::SliderInt("Skip Frames##ds",   &m_settings.detectionSkipFrames, 0, 10);
        changed |= ImGui::Checkbox("Grayscale Input##ds",&m_settings.grayscaleInput);
    }

    if (ImGui::CollapsingHeader("Tracker")) {
        changed |= ImGui::SliderFloat("Min Match Score##ts", &m_settings.trackerMinMatchScore, 0.01f, 1.0f, "%.3f");
        changed |= ImGui::SliderFloat("Max Center Dist##ts", &m_settings.trackerMaxCenterDistPx, 10.0f, 800.0f, "%.0f");
        changed |= ImGui::SliderInt("Max Lost Frames##ts",   &m_settings.trackerMaxLostFrames,  1, 120);
        changed |= ImGui::SliderInt("Trail Length##ts",      &m_settings.trackerMaxTrailLength, 5, 200);
    }

    if (ImGui::CollapsingHeader("Logging")) {
        const char* levelNames[] = {"VERBOSE","INFO","WARN","ERROR"};
        int lvl = m_settings.logLevel;
        if (ImGui::Combo("Log Level##ls", &lvl, levelNames, 4)) {
            m_settings.logLevel = lvl;
            changed = true;
        }
        changed |= ImGui::Checkbox("Log to File (not yet implemented)", &m_settings.logToFile);
    }

    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(-1, 0)))
        m_showSettingsWindow = false;

    if (changed) {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_sharedSettings = m_settings;
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// Main Loop
// -----------------------------------------------------------------------

void Application::run() {
    cv::Mat currentFrame;
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();

        // Track render timing
        {
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - m_lastRenderTime).count();
            m_lastRenderTime = now;
            if (dt > 1e-6f) {
                m_renderFps   = 1.0f / dt;
                m_frameTimeMs = dt * 1000.0f;
            }
            m_fpsHistory[m_fpsHistoryIdx] = m_renderFps;
            m_fpsHistoryIdx = (m_fpsHistoryIdx + 1) % static_cast<int>(kFpsHistorySize);
        }

        // Consume shared data
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            if (m_newDataAvailable) {
                currentFrame       = m_sharedFrame.clone();
                m_detections       = m_sharedDetections;
                m_trackedObjects   = m_sharedTrackedObjects;
                m_lockedTarget     = m_sharedLockedTarget;
                m_cameraFps        = m_sharedCameraFps;
                m_renderer->updateTexture(currentFrame);
                m_newDataAvailable = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        // ── 1. Camera View ──────────────────────────────────────────────
        ImGui::Begin("Camera View", nullptr,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ViewportInfo view = {0};
        if (!currentFrame.empty()) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 pos   = ImGui::GetCursorScreenPos();

            float frame_aspect  = static_cast<float>(currentFrame.cols) / static_cast<float>(currentFrame.rows);
            float window_aspect = avail.x / avail.y;

            if (window_aspect > frame_aspect) {
                view.target_h = avail.y;
                view.target_w = view.target_h * frame_aspect;
                view.pos_x    = pos.x + (avail.x - view.target_w) / 2.0f;
                view.pos_y    = pos.y;
                view.scale    = view.target_h / static_cast<float>(currentFrame.rows);
            } else {
                view.target_w = avail.x;
                view.target_h = view.target_w / frame_aspect;
                view.pos_y    = pos.y + (avail.y - view.target_h) / 2.0f;
                view.pos_x    = pos.x;
                view.scale    = view.target_w / static_cast<float>(currentFrame.cols);
            }

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddImage(reinterpret_cast<void*>(static_cast<intptr_t>(m_renderer->getTextureID())),
                               ImVec2(view.pos_x, view.pos_y),
                               ImVec2(view.pos_x + view.target_w, view.pos_y + view.target_h));

            // ── ROI Edit Mode: drag-to-draw/move/resize ───────────────
            int hoveredZoneId = -1;
            ROIEditState hoveredAction = ROIEditState::NONE;

            if (m_roiEditMode && ImGui::IsWindowHovered(ImGuiHoveredFlags_None)) {
                ImVec2 mpos = ImGui::GetMousePos();
                cv::Point mVideo(
                    static_cast<int>((mpos.x - view.pos_x) / view.scale),
                    static_cast<int>((mpos.y - view.pos_y) / view.scale));

                auto zones = m_roiManager->getROIs();
                double tol = std::max(4.0, 8.0 / view.scale);

                if (m_editState == ROIEditState::NONE) {
                    // Check corners first
                    for (const auto& z : zones) {
                        cv::Rect r = z.rect;
                        cv::Point tl(r.x, r.y);
                        cv::Point tr(r.x + r.width, r.y);
                        cv::Point bl(r.x, r.y + r.height);
                        cv::Point br(r.x + r.width, r.y + r.height);

                        if (std::hypot(mVideo.x - tl.x, mVideo.y - tl.y) <= tol) {
                            hoveredZoneId = z.id; hoveredAction = ROIEditState::RESIZING_TL; break;
                        }
                        if (std::hypot(mVideo.x - tr.x, mVideo.y - tr.y) <= tol) {
                            hoveredZoneId = z.id; hoveredAction = ROIEditState::RESIZING_TR; break;
                        }
                        if (std::hypot(mVideo.x - bl.x, mVideo.y - bl.y) <= tol) {
                            hoveredZoneId = z.id; hoveredAction = ROIEditState::RESIZING_BL; break;
                        }
                        if (std::hypot(mVideo.x - br.x, mVideo.y - br.y) <= tol) {
                            hoveredZoneId = z.id; hoveredAction = ROIEditState::RESIZING_BR; break;
                        }
                    }

                    // Check edges if no corner hovered
                    if (hoveredZoneId == -1) {
                        for (const auto& z : zones) {
                            cv::Rect r = z.rect;
                            if (std::abs(mVideo.x - r.x) <= tol && mVideo.y >= r.y && mVideo.y <= r.y + r.height) {
                                hoveredZoneId = z.id; hoveredAction = ROIEditState::RESIZING_L; break;
                            }
                            if (std::abs(mVideo.x - (r.x + r.width)) <= tol && mVideo.y >= r.y && mVideo.y <= r.y + r.height) {
                                hoveredZoneId = z.id; hoveredAction = ROIEditState::RESIZING_R; break;
                            }
                            if (std::abs(mVideo.y - r.y) <= tol && mVideo.x >= r.x && mVideo.x <= r.x + r.width) {
                                hoveredZoneId = z.id; hoveredAction = ROIEditState::RESIZING_T; break;
                            }
                            if (std::abs(mVideo.y - (r.y + r.height)) <= tol && mVideo.x >= r.x && mVideo.x <= r.x + r.width) {
                                hoveredZoneId = z.id; hoveredAction = ROIEditState::RESIZING_B; break;
                            }
                        }
                    }

                    // Check center (moving) if no edge/corner hovered
                    if (hoveredZoneId == -1) {
                        for (const auto& z : zones) {
                            cv::Rect r = z.rect;
                            if (mVideo.x > r.x && mVideo.x < r.x + r.width &&
                                mVideo.y > r.y && mVideo.y < r.y + r.height) {
                                hoveredZoneId = z.id; hoveredAction = ROIEditState::MOVING; break;
                            }
                        }
                    }
                } else {
                    // We are actively editing
                    hoveredZoneId = m_editZoneId;
                    hoveredAction = m_editState;
                }

                // Handle cursors
                if (m_editState == ROIEditState::DRAWING) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                } else {
                    ROIEditState currentAction = (m_editState == ROIEditState::NONE) ? hoveredAction : m_editState;
                    switch (currentAction) {
                        case ROIEditState::RESIZING_TL:
                        case ROIEditState::RESIZING_BR:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                            break;
                        case ROIEditState::RESIZING_TR:
                        case ROIEditState::RESIZING_BL:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
                            break;
                        case ROIEditState::RESIZING_L:
                        case ROIEditState::RESIZING_R:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                            break;
                        case ROIEditState::RESIZING_T:
                        case ROIEditState::RESIZING_B:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                            break;
                        case ROIEditState::MOVING:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                            break;
                        default:
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
                            break;
                    }
                }

                // Mouse actions
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    if (hoveredAction != ROIEditState::NONE) {
                        m_editState = hoveredAction;
                        m_editZoneId = hoveredZoneId;
                        m_editDragStartMouse = mVideo;
                        // Get the zone starting rect
                        for (const auto& z : zones) {
                            if (z.id == m_editZoneId) {
                                m_editDragStartRect = z.rect;
                                break;
                            }
                        }
                    } else if (zones.size() < ROIManager::kMaxZones) {
                        m_editState = ROIEditState::DRAWING;
                        m_roiManager->beginDrag(mVideo);
                    }
                }

                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    if (m_editState == ROIEditState::DRAWING && m_roiManager->isDragging()) {
                        m_roiManager->updateDrag(mVideo);
                    } else if (m_editState != ROIEditState::NONE && m_editZoneId != -1) {
                        int dx = mVideo.x - m_editDragStartMouse.x;
                        int dy = mVideo.y - m_editDragStartMouse.y;
                        int cols = currentFrame.cols;
                        int rows = currentFrame.rows;
                        cv::Rect newRect = m_editDragStartRect;

                        switch (m_editState) {
                            case ROIEditState::MOVING: {
                                int x1 = std::clamp(m_editDragStartRect.x + dx, 0, cols - m_editDragStartRect.width);
                                int y1 = std::clamp(m_editDragStartRect.y + dy, 0, rows - m_editDragStartRect.height);
                                newRect = cv::Rect(x1, y1, m_editDragStartRect.width, m_editDragStartRect.height);
                                break;
                            }
                            case ROIEditState::RESIZING_TL: {
                                int x1 = std::clamp(m_editDragStartRect.x + dx, 0, m_editDragStartRect.x + m_editDragStartRect.width - 4);
                                int y1 = std::clamp(m_editDragStartRect.y + dy, 0, m_editDragStartRect.y + m_editDragStartRect.height - 4);
                                newRect = cv::Rect(x1, y1, (m_editDragStartRect.x + m_editDragStartRect.width) - x1, (m_editDragStartRect.y + m_editDragStartRect.height) - y1);
                                break;
                            }
                            case ROIEditState::RESIZING_TR: {
                                int x2 = std::clamp(m_editDragStartRect.x + m_editDragStartRect.width + dx, m_editDragStartRect.x + 4, cols);
                                int y1 = std::clamp(m_editDragStartRect.y + dy, 0, m_editDragStartRect.y + m_editDragStartRect.height - 4);
                                newRect = cv::Rect(m_editDragStartRect.x, y1, x2 - m_editDragStartRect.x, (m_editDragStartRect.y + m_editDragStartRect.height) - y1);
                                break;
                            }
                            case ROIEditState::RESIZING_BL: {
                                int x1 = std::clamp(m_editDragStartRect.x + dx, 0, m_editDragStartRect.x + m_editDragStartRect.width - 4);
                                int y2 = std::clamp(m_editDragStartRect.y + m_editDragStartRect.height + dy, m_editDragStartRect.y + 4, rows);
                                newRect = cv::Rect(x1, m_editDragStartRect.y, (m_editDragStartRect.x + m_editDragStartRect.width) - x1, y2 - m_editDragStartRect.y);
                                break;
                            }
                            case ROIEditState::RESIZING_BR: {
                                int x2 = std::clamp(m_editDragStartRect.x + m_editDragStartRect.width + dx, m_editDragStartRect.x + 4, cols);
                                int y2 = std::clamp(m_editDragStartRect.y + m_editDragStartRect.height + dy, m_editDragStartRect.y + 4, rows);
                                newRect = cv::Rect(m_editDragStartRect.x, m_editDragStartRect.y, x2 - m_editDragStartRect.x, y2 - m_editDragStartRect.y);
                                break;
                            }
                            case ROIEditState::RESIZING_L: {
                                int x1 = std::clamp(m_editDragStartRect.x + dx, 0, m_editDragStartRect.x + m_editDragStartRect.width - 4);
                                newRect = cv::Rect(x1, m_editDragStartRect.y, (m_editDragStartRect.x + m_editDragStartRect.width) - x1, m_editDragStartRect.height);
                                break;
                            }
                            case ROIEditState::RESIZING_R: {
                                int x2 = std::clamp(m_editDragStartRect.x + m_editDragStartRect.width + dx, m_editDragStartRect.x + 4, cols);
                                newRect = cv::Rect(m_editDragStartRect.x, m_editDragStartRect.y, x2 - m_editDragStartRect.x, m_editDragStartRect.height);
                                break;
                            }
                            case ROIEditState::RESIZING_T: {
                                int y1 = std::clamp(m_editDragStartRect.y + dy, 0, m_editDragStartRect.y + m_editDragStartRect.height - 4);
                                newRect = cv::Rect(m_editDragStartRect.x, y1, m_editDragStartRect.width, (m_editDragStartRect.y + m_editDragStartRect.height) - y1);
                                break;
                            }
                            case ROIEditState::RESIZING_B: {
                                int y2 = std::clamp(m_editDragStartRect.y + m_editDragStartRect.height + dy, m_editDragStartRect.y + 4, rows);
                                newRect = cv::Rect(m_editDragStartRect.x, m_editDragStartRect.y, m_editDragStartRect.width, y2 - m_editDragStartRect.y);
                                break;
                            }
                            default:
                                break;
                        }
                        m_roiManager->updateRect(m_editZoneId, newRect);
                    }
                }

                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    if (m_editState == ROIEditState::DRAWING && m_roiManager->isDragging()) {
                        int newId = m_roiManager->commitDrag();
                        if (newId >= 0) {
                            int bufIdx = newId % ROIManager::kMaxZones;
                            auto currentZones = m_roiManager->getROIs();
                            for (const auto& z : currentZones) {
                                if (z.id == newId) {
                                    strncpy(m_roiLabelBuf[bufIdx], z.label.c_str(), 63);
                                    break;
                                }
                            }
                            log(LogLevel::INFO, "ROI zone added: ID " + std::to_string(newId));
                        }
                        if (static_cast<int>(m_roiManager->getROIs().size()) >= ROIManager::kMaxZones)
                            m_roiEditMode = false;
                    }
                    m_editState = ROIEditState::NONE;
                    m_editZoneId = -1;
                }

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    if (m_editState == ROIEditState::DRAWING && m_roiManager->isDragging()) {
                        m_roiManager->cancelDrag();
                    }
                    m_editState = ROIEditState::NONE;
                    m_editZoneId = -1;
                }

                // Draw in-progress drag rectangle
                if (m_editState == ROIEditState::DRAWING && m_roiManager->isDragging()) {
                    cv::Rect dr = m_roiManager->getDragRect();
                    ImVec2 rMin(view.pos_x + dr.x * view.scale,
                                view.pos_y + dr.y * view.scale);
                    ImVec2 rMax(view.pos_x + (dr.x + dr.width)  * view.scale,
                                view.pos_y + (dr.y + dr.height) * view.scale);
                    drawList->AddRect(rMin, rMax, IM_COL32(255, 220, 0, 220), 0.0f, 0, 2.0f);
                    drawList->AddRectFilled(rMin, rMax, IM_COL32(255, 220, 0, 25));
                }
            }

            // ── ROI Overlay: draw existing zones ──────────────────────
            if (m_settings.showROIOverlay) {
                auto zones = m_roiManager->getROIs();
                for (const auto& z : zones) {
                    bool isHoveredZone = (m_roiEditMode && hoveredZoneId == z.id);
                    bool isEditingZone = (m_roiEditMode && m_editZoneId == z.id);

                    // Check if alarm triggered (object inside active alarm zone)
                    bool hasObjectInside = false;
                    if (z.active && z.function == ROIFunction::ALARM) {
                        for (const auto& obj : m_trackedObjects) {
                            cv::Point center(
                                obj.box.x + obj.box.width  / 2,
                                obj.box.y + obj.box.height / 2);
                            if (z.rect.contains(center)) {
                                hasObjectInside = true;
                                break;
                            }
                        }
                    }

                    // Style colors based on function and state
                    ImU32 col = IM_COL32(150, 150, 150, 120); // Gray if inactive
                    ImU32 fillCol = IM_COL32(150, 150, 150, 10);
                    float borderThickness = (isHoveredZone || isEditingZone) ? 2.5f : 1.5f;

                    if (z.active) {
                        if (z.function == ROIFunction::DETECTION) {
                            col = IM_COL32(100, 255, 80, 200); // Green
                            fillCol = IM_COL32(100, 255, 80, 18);
                        } else if (z.function == ROIFunction::EXCLUDE) {
                            col = IM_COL32(240, 140, 30, 200); // Orange
                            fillCol = IM_COL32(240, 140, 30, 15);
                        } else if (z.function == ROIFunction::ALARM) {
                            if (hasObjectInside) {
                                float timeSec = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_appStart).count();
                                bool flash = (static_cast<int>(timeSec * 4.0f) % 2 == 0);
                                col = flash ? IM_COL32(255, 0, 0, 255) : IM_COL32(150, 0, 0, 220); // Flashing Red
                                fillCol = IM_COL32(255, 0, 0, flash ? 35 : 15);
                                borderThickness = 3.0f;
                            } else {
                                col = IM_COL32(220, 50, 50, 180); // Red
                                fillCol = IM_COL32(220, 50, 50, 15);
                            }
                        }
                    }

                    // Highlights
                    if (isHoveredZone || isEditingZone) {
                        // Blend yellow glow if hovered/editing
                        col = IM_COL32(255, 255, 0, 255);
                    }

                    ImVec2 rMin(view.pos_x + z.rect.x * view.scale,
                                view.pos_y + z.rect.y * view.scale);
                    ImVec2 rMax(view.pos_x + (z.rect.x + z.rect.width)  * view.scale,
                                view.pos_y + (z.rect.y + z.rect.height) * view.scale);

                    drawList->AddRect(rMin, rMax, col, 0.0f, 0, borderThickness);
                    drawList->AddRectFilled(rMin, rMax, fillCol);

                    // Render corner handles (little squares) in edit mode
                    if (m_roiEditMode) {
                        ImVec2 tl(rMin.x, rMin.y);
                        ImVec2 tr(rMax.x, rMin.y);
                        ImVec2 bl(rMin.x, rMax.y);
                        ImVec2 br(rMax.x, rMax.y);

                        drawList->AddRectFilled(ImVec2(tl.x - 3, tl.y - 3), ImVec2(tl.x + 3, tl.y + 3), col);
                        drawList->AddRectFilled(ImVec2(tr.x - 3, tr.y - 3), ImVec2(tr.x + 3, tr.y + 3), col);
                        drawList->AddRectFilled(ImVec2(bl.x - 3, bl.y - 3), ImVec2(bl.x + 3, bl.y + 3), col);
                        drawList->AddRectFilled(ImVec2(br.x - 3, br.y - 3), ImVec2(br.x + 3, br.y + 3), col);
                    }

                    // Render label with function prefix
                    std::string funcPrefix = "";
                    if (z.function == ROIFunction::DETECTION) funcPrefix = "[DET] ";
                    else if (z.function == ROIFunction::EXCLUDE) funcPrefix = "[EXC] ";
                    else if (z.function == ROIFunction::ALARM) {
                        funcPrefix = hasObjectInside ? "[ALARM TRIGGERED] " : "[ALM] ";
                    }

                    char zoneLbl[120];
                    snprintf(zoneLbl, sizeof(zoneLbl), "%s[%d] %s%s",
                             funcPrefix.c_str(), z.id, z.label.c_str(), z.active ? "" : " (off)");
                    drawList->AddText(ImVec2(rMin.x + 4, rMin.y + 3), col, zoneLbl);
                }
            }

            // ── Regular HUD and target locking ────────────────────────
            if (!m_roiEditMode) {
                handleTargetLocking(view);
            }
            m_hud->render(drawList, static_cast<int>(avail.x), static_cast<int>(avail.y),
                          m_cameraFps, m_trackedObjects, m_lockedTarget, view, m_settings);
        }
        ImGui::End();

        // ── 2. Data Panel ────────────────────────────────────────────────
        renderDataPanel();

        // ── 3. Zoom Window ───────────────────────────────────────────────
        renderZoomWindow(currentFrame);

        // ── 4. Dev Console ───────────────────────────────────────────────
        renderDevConsole();

        // ── 5. Settings Window (floating) ────────────────────────────────
        renderSettingsWindow();

        // Render
        int display_w, display_h;
        glfwGetFramebufferSize(m_window, &display_w, &display_h);
        ImGui::Render();
        glViewport(0, 0, display_w, display_h);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup);
        }

        glfwSwapBuffers(m_window);
    }
}
void Application::cleanup() {
    m_running = false;
    if (m_workerThread.joinable()) m_workerThread.join();
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
}
