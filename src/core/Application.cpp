#include "Application.hpp"
#include <iostream>
#include <cstring>
#include <cmath>
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
      m_lockRequested(false), m_running(false), m_newDataAvailable(false) {

    m_appStart = std::chrono::steady_clock::now();
    m_lastRenderTime = m_appStart;

    m_fpsHistory.assign(kFpsHistorySize, 0.0f);

    m_camera   = std::make_unique<CameraModule>();
    m_renderer = std::make_unique<VideoRenderer>();
    m_hud      = std::make_unique<HUD>();
    m_tracker  = std::make_unique<MultiTracker>();

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
                m_workerDetectionCount.store(static_cast<int>(detections.size()));
            }
        }

        std::vector<TrackedObject> tracked;
        if (currentSettings.enableTracking) {
            m_tracker->update(detections, currentSettings);
            tracked = m_tracker->getTrackedObjects(currentSettings.trackerMaxTrailLength);
            m_workerTrackCount.store(static_cast<int>(tracked.size()));
        }
        m_totalFramesProcessed.fetch_add(1);

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
    if (!ImGui::GetIO().WantCaptureMouse && ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
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
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed, 35.0f);
        ImGui::TableSetupColumn("Class",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Pos (X,Y)",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Conf",     ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("State",    ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableHeadersRow();

        for (const auto& obj : m_trackedObjects) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", obj.track_id);

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
            if (obj.is_confirmed)
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "LOCK");
            else if (obj.lost_frames > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "LOST");
            else
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "INIT");
        }
        ImGui::EndTable();
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
    ImGui::TextDisabled("v1.7.0");
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
            if (ImGui::Button("Reset All Settings", ImVec2(-1, 0))) {
                m_settings = SystemSettings{};
                m_settings.hudColor    = Float4ToImU32(m_hudColorF);
                m_settings.targetColor = Float4ToImU32(m_targetColorF);
                settingsChanged = true;
                log(LogLevel::WARN, "All settings reset to defaults");
            }
            if (ImGui::Button("Quit Application", ImVec2(-1, 0)))
                glfwSetWindowShouldClose(m_window, true);

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
            // Update FPS ring buffer
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

            handleTargetLocking(view);
            m_hud->render(drawList, static_cast<int>(avail.x), static_cast<int>(avail.y),
                          m_cameraFps, m_trackedObjects, view, m_settings);
        }
        ImGui::End();

        // ── 2. Data Panel ────────────────────────────────────────────────
        renderDataPanel();

        // ── 3. Dev Console ───────────────────────────────────────────────
        renderDevConsole();

        // ── 4. Settings Window (floating) ────────────────────────────────
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
