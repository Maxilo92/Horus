#include "UIManager.hpp"
#include "UIManager_internal.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <opencv2/opencv.hpp>
#include <GLFW/glfw3.h>
#include <filesystem>
#include <iostream>
#include <cstring>
#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#endif

// Opens a URL in the system's default browser, cross-platform.
static void openUrl(const std::string& url) {
    if (url.empty()) return;
#ifdef __APPLE__
    system(("open \"" + url + "\"").c_str());
#elif defined(_WIN32)
    ShellExecuteW(nullptr, L"open",
                  std::wstring(url.begin(), url.end()).c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
#else
    system(("xdg-open \"" + url + "\"").c_str());
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

UIManager::UIManager(Blackboard& blackboard, ROIManager& roiManager,
                     DataLogger& dataLogger, AudioEngine& audioEngine,
                     GLFWwindow* window, const std::string& settingsPath,
                     LogFn logFn)
    : m_blackboard(blackboard)
    , m_roiManager(roiManager)
    , m_dataLogger(dataLogger)
    , m_audioEngine(audioEngine)
    , m_window(window)
    , m_settingsPath(settingsPath)
    , m_log(std::move(logFn))
{
    m_appStart      = std::chrono::steady_clock::now();
    m_lastRenderTime = m_appStart;

    // Initialize model list for installation check
    const std::string baseUrl = "https://github.com/Maxilo92/Horus/releases/download/v0.12.0/";
    m_modelStatuses = {
        {"YOLOv8s (Genau)", "yolov8s.onnx", baseUrl + "yolov8s.onnx"},
        {"YOLOv8n (Schnell)", "yolov8n.onnx", baseUrl + "yolov8n.onnx"},
        {"Labels (COCO)", "coco.txt", baseUrl + "coco.txt"},
        {"Gesichtserkennung (Detektor)", "face_detection_yunet_2023mar.onnx", baseUrl + "face_detection_yunet_2023mar.onnx"},
        {"Gesichtserkennung (Feature)", "face_recognition_sface_2021dec.onnx", baseUrl + "face_recognition_sface_2021dec.onnx"}
    };
    
    m_devConsolePanel = std::make_unique<DevConsolePanel>();
    m_analyzerPanel   = std::make_unique<AnalyzerPanel>();
    m_audioVisualizerPanel = std::make_unique<AudioVisualizerPanel>();
    m_replayPanel     = std::make_unique<ReplayPanel>();
    m_radarPanel      = std::make_unique<RadarPanel>();

    m_exportWorkerRunning = true;
    m_exportWorker = std::thread(&UIManager::exportWorkerLoop, this);
}

UIManager::~UIManager() {
    savePersistedSettings();
    
    m_exportWorkerRunning = false;
    m_exportCv.notify_all();
    if (m_exportWorker.joinable())
        m_exportWorker.join();
}

// ─────────────────────────────────────────────────────────────────────────────
// IModule lifecycle
// ─────────────────────────────────────────────────────────────────────────────

bool UIManager::initRenderers() {
    m_renderer        = std::make_unique<VideoRenderer>();
    m_heatmapRenderer = std::make_unique<VideoRenderer>();
    m_zoomRenderer    = std::make_unique<VideoRenderer>();
    for (int i = 0; i < 4; ++i)
        m_subZoomRenderers[i] = std::make_unique<VideoRenderer>();
    m_hud = std::make_unique<HUD>();
    return true;
}

void UIManager::start() {
    loadPersistedSettings(m_settingsPath);
    // Explicitly push the loaded camera address to blackboard so VisionSystem sees it on init
    m_blackboard.setCameraAddress(m_cameraAddress);
    pushSettingsToBlackboard();
}

void UIManager::stop() {
    savePersistedSettings();
}

void UIManager::setDossierDatabase(DossierDatabase* db) {
    if (db) {
        m_dossierArchivePanel = std::make_unique<DossierArchivePanel>(*db);
    }
}

void UIManager::setUpdateChecker(UpdateChecker* uc) {
    m_updateChecker = uc;
}

// ─────────────────────────────────────────────────────────────────────────────
// appendLog  (thread-safe, callable from any thread)
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::appendLog(LogLevel level, const std::string& msg, float appSeconds) {
    if (m_devConsolePanel) {
        m_devConsolePanel->appendLog(level, msg, appSeconds);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// updateGLTexture  (upload / re-upload a cv::Mat to an OpenGL texture)
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::updateGLTexture(const cv::Mat& img, uint32_t& tex_id,
                                int& tex_version, int current_version) {
    if (img.empty()) return;
    if (tex_id != 0 && current_version <= tex_version) return;

    bool init = (tex_id == 0);
    if (init) glGenTextures(1, &tex_id);
    
    glBindTexture(GL_TEXTURE_2D, tex_id);
    
    if (init) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT,  1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(img.step / img.elemSize()));
    
    if (init) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     img.cols, img.rows, 0,
                     GL_BGR, GL_UNSIGNED_BYTE, img.data);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.cols, img.rows,
                        GL_BGR, GL_UNSIGNED_BYTE, img.data);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT,  4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    tex_version = current_version;
}

// ─────────────────────────────────────────────────────────────────────────────
// pushSettingsToBlackboard
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::pushSettingsToBlackboard() {
    m_blackboard.setSettings(m_settings);
    m_blackboard.setCameraAddress(m_cameraAddress);
}

// ─────────────────────────────────────────────────────────────────────────────
// update()  –  one full ImGui frame
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::update() {
    // ── Render timing ─────────────────────────────────────────────────────
    {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - m_lastRenderTime).count();
        m_lastRenderTime = now;
        if (dt > 1e-6f) {
            m_renderFps   = 1.0f / dt;
            m_frameTimeMs = dt * 1000.0f;
        }
        if (m_devConsolePanel) {
            m_devConsolePanel->addFpsSample(m_renderFps);
            m_devConsolePanel->addPerformanceSamples(m_perfMetrics.inferenceTimeMs, m_perfMetrics.trackingTimeMs);
        }
    }

    // ── Pull state from Blackboard ────────────────────────────────────────
    VisionState        vision   = m_blackboard.getVisionState();
    TrackingStateData  tracking = m_blackboard.getTrackingState();
    DetectionState     det      = m_blackboard.getDetectionState();
    AppStatusState     status   = m_blackboard.getAppStatus();
    DossierState       dossier  = m_blackboard.getDossierState();
    m_perfMetrics               = m_blackboard.getPerformanceMetrics();

    // Cache fields into UIManager members
    m_cameraFps     = vision.cameraFps;
    m_cameraWidth   = vision.cameraWidth;
    m_cameraHeight  = vision.cameraHeight;
    m_trackingWidth = vision.trackingWidth;
    m_trackingHeight= vision.trackingHeight;
    m_zoomWidth     = vision.zoomWidth;
    m_zoomHeight    = vision.zoomHeight;

    m_trackedObjects = tracking.activeTracks;
    
    // Support manual drag/edit: don't overwrite the locked target box if the user is currently 
    // manipulating the pixel target (ID 999). This prevents "jumping" back to the last tracked 
    // position before the tracking thread has processed the update.
    if (m_editZoneId == 999 && m_editState != ROIEditState::NONE) {
        cv::Rect localBox = m_lockedTarget.box;
        m_lockedTarget = tracking.lockedTarget;
        m_lockedTarget.box = localBox;
    } else {
        m_lockedTarget = tracking.lockedTarget;
    }

    m_targetHistory  = tracking.targetHistory;
    m_motionRegions  = det.motionRegions;
    m_detections     = det.detections;

    if (!status.classNames.empty())
        m_classNames = status.classNames;

    // m_cameraAddress should not be overwritten by status here because it is the persistent "desired" state.
    // Overwriting it every frame prevents users from changing it and makes savePersistedSettings save the current state rather than the persistent one.
    // m_cameraAddress = status.cameraAddress; <--- Removed to fix persistence bug

    // ── Display frame (fast-path from capture thread) ─────────────────────
    cv::Mat displayFrame;
    {
        if (m_blackboard.consumeDisplayFrame(displayFrame) && !displayFrame.empty()) {
            m_renderer->updateTexture(displayFrame);
            // Cache dimensions directly from frame — VisionState may lag on first frames
            if (m_cameraWidth  == 0) m_cameraWidth  = displayFrame.cols;
            if (m_cameraHeight == 0) m_cameraHeight = displayFrame.rows;
        }
    }

    // ── Heatmap texture ───────────────────────────────────────────────────
    if (!vision.heatmapFrame.empty())
        m_heatmapRenderer->updateTexture(vision.heatmapFrame);

    // ── Sub-zoom frames ───────────────────────────────────────────────────
    for (int i = 0; i < 4; ++i) {
        m_subZooms[i].active    = vision.subZooms[i].active;
        m_subZooms[i].motion_id = vision.subZooms[i].motion_id;
        m_subZooms[i].box       = vision.subZooms[i].box;
        m_subZooms[i].isLost    = vision.subZooms[i].isLost;
        if (!vision.subZooms[i].frame.empty())
            m_subZooms[i].frame = vision.subZooms[i].frame; // Shallow copy
        if (m_settings.subZoomsEnabled && m_subZooms[i].active && !m_subZooms[i].frame.empty())
            m_subZoomRenderers[i]->updateTexture(m_subZooms[i].frame);
    }

    // ── Window layout reset (applied before NewFrame so positions take effect) ──
    if (m_resetWindowsPending) {
        int fw, fh;
        glfwGetFramebufferSize(m_window, &fw, &fh);
        const int menuH    = 19;
        const int contentH = fh - menuH;
        const int conW     = std::min(360, fw / 4);
        const int camW     = fw - conW;

        // Build a default ini: Camera View fills the left area, Dev Console docks right.
        // All other panels are placed at non-overlapping positions and collapsed.
        char buf[4096];
        snprintf(buf, sizeof(buf),
            "[Window][Camera View]\n"
            "Pos=0,%d\nSize=%d,%d\nCollapsed=0\n\n"
            "[Window][Dev Console]\n"
            "Pos=%d,%d\nSize=%d,%d\nCollapsed=0\n\n"
            "[Window][Data Panel]\n"
            "Pos=10,%d\nSize=320,180\nCollapsed=1\n\n"
            "[Window][Target Zoom]\n"
            "Pos=10,%d\nSize=320,240\nCollapsed=1\n\n"
            "[Window][Settings]\n"
            "Pos=%d,%d\nSize=480,600\nCollapsed=1\n\n"
            "[Window][Target Analyzer]\n"
            "Pos=%d,%d\nSize=460,500\nCollapsed=1\n\n"
            "[Window][AI Dossier]\n"
            "Pos=%d,%d\nSize=420,400\nCollapsed=1\n\n"
            "[Window][AI Dossier Archive]\n"
            "Pos=%d,%d\nSize=500,450\nCollapsed=1\n\n"
            "[Window][Audio Visualizer]\n"
            "Pos=%d,%d\nSize=300,120\nCollapsed=1\n\n"
            "[Window][Replay Control]\n"
            "Pos=%d,%d\nSize=400,200\nCollapsed=1\n\n",
            // Camera View
            menuH, camW, contentH,
            // Dev Console
            camW, menuH, conW, contentH,
            // Data Panel
            menuH + 10,
            // Target Zoom
            menuH + 200,
            // Settings
            fw / 2 - 240, menuH + 60,
            // Target Analyzer
            fw / 2 - 230, menuH + 80,
            // AI Dossier
            fw / 2 - 210, menuH + 100,
            // AI Dossier Archive
            fw / 2 - 250, menuH + 50,
            // Audio Visualizer
            10, fh - 140,
            // Replay Control
            fw / 2 - 200, fh - 220
        );

        ImGui::LoadIniSettingsFromMemory(buf, strlen(buf));
        ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
        m_resetWindowsPending = false;
    }

    // ── ImGui frame begin ─────────────────────────────────────────────────
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // ── Menu bar ──────────────────────────────────────────────────────────
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Profile (Settings)")) {
                savePersistedSettings();
                m_log(LogLevel::INFO, "Settings saved to disk");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Alt+F4"))
                glfwSetWindowShouldClose(m_window, true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Settings Window", "C",   &m_showSettingsWindow))  savePersistedSettings();
            if (ImGui::MenuItem("Dev Console",     "F12", &m_showDevConsole))       savePersistedSettings();
            if (ImGui::MenuItem("Target Analyzer", nullptr, &m_showTargetAnalyzer)) savePersistedSettings();
            ImGui::Separator();
            if (ImGui::MenuItem("Data Panel",  nullptr, &m_showDataPanel))   savePersistedSettings();
            if (ImGui::MenuItem("Zoom Window", "Z",     &m_showZoomWindow))  savePersistedSettings();
            if (ImGui::MenuItem("Radar",       nullptr, &m_showRadar))       savePersistedSettings();
            ImGui::Separator();
            if (ImGui::MenuItem("Keyboard Shortcuts", "F1"))
                m_showShortcutHelp = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Windows")) {
                m_resetWindowsPending = true;
                m_log(LogLevel::INFO, "Window layout reset");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Take Screenshot", "PrintScreen"))
                m_screenshotPending = true;
            if (ImGui::MenuItem("Export Entire History (JSON)"))
                exportTargetHistory(m_targetHistory);
            ImGui::Separator();
            if (ImGui::MenuItem("Open Export Directory")) {
                std::string dir = m_settings.exportOutputDir.empty() ? "." : m_settings.exportOutputDir;
#ifdef __APPLE__
                system(("open " + dir).c_str());
#elif _WIN32
                system(("explorer " + dir).c_str());
#else
                system(("xdg-open " + dir).c_str());
#endif
                m_log(LogLevel::INFO, "Opening directory: " + dir);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Open Settings Window", nullptr, m_showSettingsWindow)) {
                m_showSettingsWindow = !m_showSettingsWindow;
                savePersistedSettings();
            }
            if (ImGui::MenuItem("Save Current Settings")) {
                savePersistedSettings();
                m_log(LogLevel::INFO, "Settings saved to disk");
            }
            if (ImGui::MenuItem("Standard"))    applyStandardPreset();
            if (ImGui::BeginMenu("Presets")) {
                if (ImGui::MenuItem("Performance")) applyPresetPerformance();
                if (ImGui::MenuItem("Balanced"))    applyPresetBalanced();
                if (ImGui::MenuItem("Precision"))   applyPresetPrecision();
                if (ImGui::MenuItem("Low Light"))   applyPresetLowLight();
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Report Bug")) {
                m_showFeedbackWindow = true;
                m_feedbackCategoryIdx = 0;
            }
            if (ImGui::MenuItem("Suggest Feature")) {
                m_showFeedbackWindow = true;
                m_feedbackCategoryIdx = 1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Open Feedback Folder")) {
                std::string feedbackDir = "feedback";
#ifdef __APPLE__
                system(("open " + feedbackDir).c_str());
#elif _WIN32
                system(("explorer " + feedbackDir).c_str());
#else
                system(("xdg-open " + feedbackDir).c_str());
#endif
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // ── Full-viewport DockSpace (must come before any dockable windows) ───
    // PassthruCentralNode lets mouse clicks and the OpenGL scene show through
    // the centre when no window is docked there.
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    // ── Panels & Windows ──────────────────────────────────────────────────
    if (m_showDataPanel)      renderDataPanel(m_trackedObjects, m_lockedTarget, m_cameraFps, m_renderFps);
    if (m_showZoomWindow)      renderZoomWindow(vision.zoomCrop, m_lockedTarget);
    if (m_showSettingsWindow)  renderSettingsWindow();
    if (m_showDevConsole)      renderDevConsole(vision, tracking, status);
    if (m_showTargetAnalyzer)  renderTargetAnalyzer(tracking);
    if (m_showDossierPanel)    renderDossierPanel(dossier);
    if (m_dossierArchivePanel) m_dossierArchivePanel->render(&m_showDossierArchive);
    if (m_showRadar)           renderRadar();
    
    // Replay Control Panel
    if (m_replayPanel) m_replayPanel->render(m_blackboard);

    // CameraView (the big one)
    renderCameraView(displayFrame, m_trackedObjects, m_lockedTarget, m_motionRegions, vision);

    // Feedback modal
    if (m_showFeedbackWindow) {
        ImGui::OpenPopup("Feedback");
        if (ImGui::BeginPopupModal("Feedback", &m_showFeedbackWindow, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char feedbackBuf[1024] = {0};
            static char contactBuf[128] = {0};
            const char* categories[] = {"Bug Report", "Feature Suggestion", "Other"};
            const char* priorities[] = {"Low", "Medium", "High", "Critical"};

            ImGui::Combo("Category", &m_feedbackCategoryIdx, categories, IM_ARRAYSIZE(categories));
            ImGui::Combo("Priority", &m_feedbackPriorityIdx, priorities, IM_ARRAYSIZE(priorities));
            ImGui::InputText("Contact (Optional)", contactBuf, sizeof(contactBuf));
            ImGui::Separator();
            ImGui::Text("Details:");
            ImGui::InputTextMultiline("##feedback", feedbackBuf, sizeof(feedbackBuf), ImVec2(400, 150));

            if (ImGui::Button("Submit", ImVec2(120, 0))) {
                if (saveFeedback(feedbackBuf, categories[m_feedbackCategoryIdx], priorities[m_feedbackPriorityIdx], contactBuf)) {
                    m_showFeedbackWindow = false;
                    feedbackBuf[0] = '\0';
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) m_showFeedbackWindow = false;
            ImGui::EndPopup();
        }
    }

    // Shortcut help
    if (m_showShortcutHelp) {
        ImGui::OpenPopup("Keyboard Shortcuts");
    }
    if (ImGui::BeginPopupModal("Keyboard Shortcuts", &m_showShortcutHelp, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.5f, 1.0f), "Tactileviewer — Hotkeys");
        ImGui::Separator();
        if (ImGui::BeginTable("ShortcutsTable", 2, ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Key",    ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            auto row = [](const char* key, const char* desc) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", key);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", desc);
            };
            row("Space",        "Toggle data logging");
            row("C",            "Toggle Settings window");
            row("Z",            "Toggle Target Zoom window");
            row("R",            "Toggle ROI Edit mode");
            row("Escape",       "Release target lock");
            row("F1",           "Show this shortcut overview");
            row("F12",          "Toggle Dev Console");
            row("PrintScreen",  "Take screenshot");
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(-1, 0))) {
            m_showShortcutHelp = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Splash screen overlay (shown until first camera frame) ───────────
    if (m_splashActive) {
        if (!m_splashTimerSet) {
            m_splashShownAt  = std::chrono::steady_clock::now();
            m_splashTimerSet = true;
        }
        float elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - m_splashShownAt).count();
        bool cameraReady = (m_cameraWidth > 0 && m_renderer->getTextureID() != 0);
        if (cameraReady && elapsed >= m_splashMinSec)
            m_splashActive = false;
        else
            renderSplashScreen();
    }

    // ── Setup wizard (first run, shown once after splash clears) ─────────
    if (m_setupWizardActive && !m_splashActive)
        renderSetupWizard();

    // ── Update notification ───────────────────────────────────────────────
    if (m_updateChecker && !m_updateDismissed && !m_setupWizardActive) {
        if (m_updateChecker->getState() == UpdateState::UPDATE_AVAILABLE)
            m_showUpdateDialog = true;
    }
    if (m_showUpdateDialog)
        renderUpdateDialog();

    // ── Render ────────────────────────────────────────────────────────────
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

    // ── Screenshot (after render so the frame is complete) ────────────────
    if (m_screenshotPending) {
        m_screenshotPending = false;
        takeScreenshot();
    }

    glfwSwapBuffers(m_window);
}

// ─────────────────────────────────────────────────────────────────────────────
// renderSplashScreen
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::renderSplashScreen() {
    // With ViewportsEnable, (0,0) is the monitor origin — use the main viewport
    // position and size so the splash always fills exactly the app window.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float W = vp->Size.x;
    const float H = vp->Size.y;

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.030f, 0.047f, 0.030f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("##splash", nullptr,
        ImGuiWindowFlags_NoDecoration    |
        ImGuiWindowFlags_NoInputs        |
        ImGuiWindowFlags_NoNav           |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking);

    ImGui::SetWindowFocus();

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Screen-space origin of the app window (non-zero when ViewportsEnable)
    const float ox = vp->Pos.x;
    const float oy = vp->Pos.y;
    // Convenience lambda: local → screen coords
    auto P = [&](float x, float y) { return ImVec2(ox + x, oy + y); };

    const ImU32 kGreen    = IM_COL32(0, 200, 100, 255);
    const ImU32 kGreenDim = IM_COL32(0, 140,  70, 180);
    const ImU32 kBg       = IM_COL32(8,  12,   8, 255);

    // Background fill
    dl->AddRectFilled(P(0, 0), P(W, H), kBg);

    // Subtle grid overlay
    const float gridSpacing = 60.0f;
    for (float x = 0; x < W; x += gridSpacing)
        dl->AddLine(P(x, 0), P(x, H), IM_COL32(0, 200, 100, 8));
    for (float y = 0; y < H; y += gridSpacing)
        dl->AddLine(P(0, y), P(W, y), IM_COL32(0, 200, 100, 8));

    // Corner brackets
    const float bLen   = 40.0f;
    const float bThk   = 2.0f;
    const float margin = 24.0f;
    // top-left
    dl->AddLine(P(margin,     margin),     P(margin + bLen, margin),          kGreen, bThk);
    dl->AddLine(P(margin,     margin),     P(margin,        margin + bLen),   kGreen, bThk);
    // top-right
    dl->AddLine(P(W - margin, margin),     P(W - margin - bLen, margin),      kGreen, bThk);
    dl->AddLine(P(W - margin, margin),     P(W - margin, margin + bLen),      kGreen, bThk);
    // bottom-left
    dl->AddLine(P(margin,     H - margin), P(margin + bLen, H - margin),      kGreen, bThk);
    dl->AddLine(P(margin,     H - margin), P(margin,        H - margin - bLen), kGreen, bThk);
    // bottom-right
    dl->AddLine(P(W - margin, H - margin), P(W - margin - bLen, H - margin),  kGreen, bThk);
    dl->AddLine(P(W - margin, H - margin), P(W - margin, H - margin - bLen),  kGreen, bThk);

    // Center decorative circle
    const float cx = W * 0.5f;
    const float cy = H * 0.40f;
    dl->AddCircle(P(cx, cy), 80.0f, IM_COL32(0, 200, 100, 40), 64, 1.0f);
    dl->AddCircle(P(cx, cy), 82.0f, kGreenDim,   64, 0.5f);

    // Horizontal rule above title
    const float ruleY = cy + 100.0f;
    dl->AddLine(P(cx - 220.0f, ruleY), P(cx - 10.0f, ruleY),  kGreenDim, 1.0f);
    dl->AddLine(P(cx + 10.0f,  ruleY), P(cx + 220.0f, ruleY), kGreenDim, 1.0f);

    // ── Title ─────────────────────────────────────────────────────────────
    {
        const char* title = "PROJECT HORUS";
        ImVec2 ts = ImGui::CalcTextSize(title);
        ImGui::SetCursorScreenPos(P((W - ts.x) * 0.5f, ruleY + 14.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.784f, 0.392f, 1.0f));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
    }

    // Subtitle
    {
        const char* sub = "TARGET ACQUISITION SYSTEM";
        ImVec2 ss = ImGui::CalcTextSize(sub);
        ImGui::SetCursorScreenPos(P((W - ss.x) * 0.5f,
                                    ruleY + 14.0f + ImGui::GetTextLineHeight() + 6.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.55f, 0.27f, 1.0f));
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
    }

    // Horizontal rule below title
    const float rule2Y = ruleY + 14.0f + ImGui::GetTextLineHeight() * 2.0f + 20.0f;
    dl->AddLine(P(cx - 220.0f, rule2Y), P(cx - 10.0f, rule2Y),  kGreenDim, 1.0f);
    dl->AddLine(P(cx + 10.0f,  rule2Y), P(cx + 220.0f, rule2Y), kGreenDim, 1.0f);

    // ── Status text (static) ─────────────────────────────────────────────
    bool cameraReady = (m_cameraWidth > 0 && m_renderer && m_renderer->getTextureID() != 0);
    const char* statusStr = cameraReady ? "CAMERA ONLINE" : "INITIALIZING CAMERA...";

    const float statusY = rule2Y + 20.0f;
    {
        ImVec2 stSz = ImGui::CalcTextSize(statusStr);
        ImGui::SetCursorScreenPos(P((W - stSz.x) * 0.5f, statusY));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.784f, 0.392f, 0.75f));
        ImGui::TextUnformatted(statusStr);
        ImGui::PopStyleColor();
    }

    // Version / build tag (bottom centre)
    {
#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif
        static const std::string verStr = std::string("v") + APP_VERSION + "  //  HORUS";
        ImVec2 vs = ImGui::CalcTextSize(verStr.c_str());
        ImGui::SetCursorScreenPos(P((W - vs.x) * 0.5f, H - margin - ImGui::GetTextLineHeight()));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.35f, 0.18f, 1.0f));
        ImGui::TextUnformatted(verStr.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ─────────────────────────────────────────────────────────────────────────────
// renderUpdateDialog
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::renderUpdateDialog() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::OpenPopup("##update_dialog");
    if (!ImGui::BeginPopupModal("##update_dialog", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 0.45f, 1.0f));
    ImGui::TextUnformatted("  UPDATE VERFUGBAR");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    std::string latest = m_updateChecker ? m_updateChecker->getLatestVersion() : "?";
    ImGui::Text("Aktuelle Version : v" APP_VERSION);
    ImGui::Text("Neue Version     : %s", latest.c_str());

    std::string notes = m_updateChecker ? m_updateChecker->getReleaseNotes() : "";
    if (!notes.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Release Notes:");
        ImGui::BeginChild("##release_notes", ImVec2(0, 120), true);
        ImGui::TextWrapped("%s", notes.c_str());
        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Direct download button (only shown when a platform asset was found)
    std::string dlUrl = m_updateChecker ? m_updateChecker->getDownloadUrl() : "";
    if (!dlUrl.empty()) {
        if (ImGui::Button("Herunterladen", ImVec2(160, 0))) {
            openUrl(dlUrl);
            m_showUpdateDialog = false;
            m_updateDismissed  = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
    }

    if (ImGui::Button("Release-Seite", ImVec2(dlUrl.empty() ? 200 : 130, 0))) {
        std::string url = m_updateChecker ? m_updateChecker->getReleaseUrl() : "";
        if (!url.empty()) openUrl(url);
        m_showUpdateDialog = false;
        m_updateDismissed  = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Spater", ImVec2(100, 0))) {
        m_showUpdateDialog = false;
        m_updateDismissed  = true;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// ─────────────────────────────────────────────────────────────────────────────
// renderSetupWizard  —  shown once on first launch, after splash clears
// ─────────────────────────────────────────────────────────────────────────────

static std::filesystem::path getExecutableDir() {
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
#elif defined(__APPLE__)
    // Rough estimate for macOS bundle or dev build
    return std::filesystem::current_path();
#else
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}

void UIManager::checkModelsExist() {
    auto exeDir = getExecutableDir();
    std::vector<std::filesystem::path> searchPaths = {
        exeDir / "assets" / "models",
        exeDir / ".." / "Resources",
        exeDir / "Resources",
        std::filesystem::path(m_settingsPath).parent_path() / "models"
    };

    for (auto& status : m_modelStatuses) {
        status.exists = false;
        for (const auto& sp : searchPaths) {
            if (std::filesystem::exists(sp / status.filename)) {
                status.exists = true;
                break;
            }
        }
        
        // Update progress if currently downloading
        if (m_updateChecker && status.downloading) {
            if (m_updateChecker->getDownloadState() == DownloadState::COMPLETED) {
                status.exists = true;
                status.downloading = false;
                status.progress = 1.0f;
            } else if (m_updateChecker->getDownloadState() == DownloadState::FAILED) {
                status.downloading = false;
                status.progress = 0.0f;
            } else {
                status.progress = m_updateChecker->getDownloadProgress();
            }
        }
    }
}

void UIManager::startModelDownload(int idx) {
    if (!m_updateChecker || idx < 0 || idx >= static_cast<int>(m_modelStatuses.size())) return;
    
    auto& status = m_modelStatuses[idx];
    if (status.exists || status.downloading) return;

    auto destDir = std::filesystem::path(m_settingsPath).parent_path() / "models";
    std::filesystem::create_directories(destDir);
    
    status.downloading = true;
    m_updateChecker->downloadFileAsync(status.url, (destDir / status.filename).string());
}

bool UIManager::allModelsPresent() {
    for (const auto& s : m_modelStatuses) if (!s.exists) return false;
    return true;
}

void UIManager::renderSetupWizard() {
    static const ImVec4 kGreen  = ImVec4(0.0f, 0.85f, 0.45f, 1.0f);
    static const ImVec4 kDim    = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    static const ImVec4 kBg     = ImVec4(0.07f, 0.10f, 0.07f, 1.0f);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowBgAlpha(0.97f);

    ImGui::OpenPopup("##setup_wizard");
    if (!ImGui::BeginPopupModal("##setup_wizard", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings))
        return;

    // ── Header ───────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
    ImGui::TextUnformatted("  PROJECT HORUS  —  ERSTEINRICHTUNG");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Step indicator ────────────────────────────────────────────────────
    const char* kStepLabels[] = { "Willkommen", "Installation", "Kamera", "Audio", "Modell", "Fertig" };
    constexpr int kTotalSteps = 6;
    for (int i = 0; i < kTotalSteps; ++i) {
        if (i > 0) { ImGui::SameLine(); ImGui::TextDisabled(" › "); ImGui::SameLine(); }
        if (i == m_setupWizardStep)
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        else
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(kStepLabels[i]);
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Step content ─────────────────────────────────────────────────────

    if (m_setupWizardStep == 0) {
        // ─ Willkommen ────────────────────────────────────────────────────
        ImGui::TextWrapped(
            "Willkommen bei Project Horus!\n\n"
            "Dieser Einrichtungsassistent hilft dir, die wichtigsten "
            "Einstellungen in wenigen Schritten zu konfigurieren.\n\n"
            "Du kannst alle Einstellungen jederzeit im Einstellungs-"
            "fenster anpassen.");

#ifdef _WIN32
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("Plattform: Windows");
        ImGui::PopStyleColor();
#elif defined(__APPLE__)
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("Plattform: macOS");
        ImGui::PopStyleColor();
#endif

    } else if (m_setupWizardStep == 1) {
        // ─ Installation ──────────────────────────────────────────────────
        ImGui::TextUnformatted("Schritt 2: KI-Modelle überprüfen");
        ImGui::Spacing();
        
        checkModelsExist();

        bool anyMissing = false;
        bool anyDownloading = false;
        for (size_t i = 0; i < m_modelStatuses.size(); ++i) {
            auto& s = m_modelStatuses[i];
            ImGui::Text("%-30s", s.name.c_str());
            ImGui::SameLine();
            if (s.exists) {
                ImGui::TextColored(kGreen, "[ VORHANDEN ]");
            } else if (s.downloading) {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "[ DOWNLOAD %3d%% ]", static_cast<int>(s.progress * 100));
                anyDownloading = true;
            } else {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "[ FEHLT ]");
                ImGui::SameLine();
                if (ImGui::SmallButton(("Download##" + std::to_string(i)).c_str())) {
                    startModelDownload(static_cast<int>(i));
                }
                anyMissing = true;
            }
        }

        ImGui::Spacing();
        if (anyMissing && !anyDownloading) {
            if (ImGui::Button("Alle fehlenden Modelle herunterladen")) {
                for (size_t i = 0; i < m_modelStatuses.size(); ++i) startModelDownload(static_cast<int>(i));
            }
        }

        if (anyDownloading) {
            ImGui::TextDisabled("Downloads laufen im Hintergrund...");
        }

    } else if (m_setupWizardStep == 2) {
        // ─ Kamera ────────────────────────────────────────────────────────
        ImGui::TextUnformatted("Kameraquelle auswählen:");
        ImGui::Spacing();

        const auto& status = m_blackboard.getAppStatus();
        if (!status.cameraDeviceNames.empty()) {
            // Enumerate detected devices
            for (int i = 0; i < static_cast<int>(status.cameraDeviceNames.size()); ++i) {
                const std::string label = std::to_string(status.cameraDeviceIDs[i])
                                        + "  —  " + status.cameraDeviceNames[i];
                bool sel = (m_wizardCameraInput[0] != '\0' &&
                            std::string(m_wizardCameraInput) == std::to_string(status.cameraDeviceIDs[i]));
                if (ImGui::RadioButton(label.c_str(), sel)) {
                    const std::string id = std::to_string(status.cameraDeviceIDs[i]);
                    std::strncpy(m_wizardCameraInput, id.c_str(), sizeof(m_wizardCameraInput) - 1);
                    m_wizardCameraInput[sizeof(m_wizardCameraInput) - 1] = '\0';
                }
            }
            ImGui::Spacing();
            ImGui::TextDisabled("— oder manuell eingeben —");
        } else {
            ImGui::TextDisabled("(Kameraerkennung läuft…)");
        }

        ImGui::Spacing();
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("Index / URL##wiz_cam", m_wizardCameraInput,
                         sizeof(m_wizardCameraInput));
        ImGui::TextDisabled("Zahl = lokale Kamera (0, 1, …)   |   rtsp://… = Netzwerk-Stream");

    } else if (m_setupWizardStep == 3) {
        // ─ Audio ─────────────────────────────────────────────────────────
        ImGui::TextUnformatted("Audioeingang (optional — für Audio-Visualizer):");
        ImGui::Spacing();

        const auto& status = m_blackboard.getAppStatus();

        bool sel_none = (m_wizardAudioIdx == -1);
        if (ImGui::RadioButton("Kein Audioeingang##wiz_audio_none", sel_none))
            m_wizardAudioIdx = -1;

        for (int i = 0; i < static_cast<int>(status.audioDeviceNames.size()); ++i) {
            bool sel = (m_wizardAudioIdx == static_cast<int>(status.audioDeviceIDs[i]));
            const std::string label = status.audioDeviceNames[i] + "##wiz_audio_" + std::to_string(i);
            if (ImGui::RadioButton(label.c_str(), sel))
                m_wizardAudioIdx = static_cast<int>(status.audioDeviceIDs[i]);
        }
        if (status.audioDeviceNames.empty())
            ImGui::TextDisabled("(Keine Eingabegeräte gefunden)");

    } else if (m_setupWizardStep == 4) {
        // ─ Modell ─────────────────────────────────────────────────────────
        ImGui::TextUnformatted("Erkennungsmodell:");
        ImGui::Spacing();

        if (ImGui::RadioButton("YOLOv8s  —  Genauer, langsamer (empfohlen)##wiz_model_s",
                               m_wizardModelIdx == 0))
            m_wizardModelIdx = 0;
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("    Besser für statische Szenen und hohe Erkennungsrate.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (ImGui::RadioButton("YOLOv8n  —  Schneller, leichter##wiz_model_n",
                               m_wizardModelIdx == 1))
            m_wizardModelIdx = 1;
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("    Für schwächere Hardware oder Echtzeit-Anforderungen.");
        ImGui::PopStyleColor();

    } else if (m_setupWizardStep == 5) {
        // ─ Fertig ─────────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::TextUnformatted("Einrichtung abgeschlossen!");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::Text("Kamera:  %s", m_wizardCameraInput);
        if (m_wizardAudioIdx >= 0) {
            ImGui::Text("Audio:   Gerät %d", m_wizardAudioIdx);
        } else {
            ImGui::TextUnformatted("Audio:   Kein Eingang");
        }
        ImGui::Text("Modell:  %s", m_wizardModelIdx == 0 ? "YOLOv8s" : "YOLOv8n");
        ImGui::Spacing();
        ImGui::TextWrapped("Diese Einstellungen können jederzeit über das Einstellungsfenster geändert werden.");
    }

    // ── Navigation ────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (m_setupWizardStep > 0) {
        if (ImGui::Button("< Zurück##wiz_back", ImVec2(100, 0)))
            --m_setupWizardStep;
        ImGui::SameLine();
    }

    const bool isLastStep = (m_setupWizardStep == kTotalSteps - 1);
    const char* nextLabel = isLastStep ? "Starten  >" : "Weiter  >";

    // Block "Weiter" if models are missing in step 1
    bool nextBlocked = (m_setupWizardStep == 1 && !allModelsPresent());

    // Align next/finish button to right
    float buttonW = isLastStep ? 130.0f : 110.0f;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - buttonW +
                         (m_setupWizardStep > 0 ? 108.0f : 0.0f));

    if (nextBlocked) {
        ImGui::BeginDisabled();
        ImGui::Button(nextLabel, ImVec2(buttonW, 0));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Alle Modelle müssen für den Betrieb installiert sein.");
    } else {
        if (ImGui::Button(nextLabel, ImVec2(buttonW, 0))) {
            if (isLastStep) {
                // Apply wizard choices
                if (m_wizardCameraInput[0] != '\0')
                    m_cameraAddress = std::string(m_wizardCameraInput);

                // Apply model choice
                m_settings.detectorModel = m_wizardModelIdx;

                // Apply audio capture device
                if (m_wizardAudioIdx >= 0) {
                    m_settings.audioCaptureEnabled  = true;
                    m_settings.audioCaptureDeviceId = static_cast<uint32_t>(m_wizardAudioIdx);
                } else {
                    m_settings.audioCaptureEnabled = false;
                }

                // Propagate to blackboard and save — this also writes setup_complete=1
                pushSettingsToBlackboard();
                savePersistedSettings();

                m_setupWizardActive = false;
                m_setupWizardStep   = 0;
                ImGui::CloseCurrentPopup();
            } else {
                ++m_setupWizardStep;
            }
        }
    }

    ImGui::EndPopup();
}
