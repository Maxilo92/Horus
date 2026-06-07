#include "UIManager.hpp"
#include "UIManager_internal.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <opencv2/opencv.hpp>
#include <GLFW/glfw3.h>
#include <filesystem>
#include <iostream>
#include <cstring>

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
    
    m_devConsolePanel = std::make_unique<DevConsolePanel>();
    m_analyzerPanel   = std::make_unique<AnalyzerPanel>();
    m_audioVisualizerPanel = std::make_unique<AudioVisualizerPanel>();
    m_replayPanel     = std::make_unique<ReplayPanel>();

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
    m_lockedTarget   = tracking.lockedTarget;
    m_targetHistory  = tracking.targetHistory;
    m_motionRegions  = det.motionRegions;
    m_detections     = det.detections;

    if (!status.classNames.empty())
        m_classNames = status.classNames;

    // m_cameraAddress should not be overwritten by status here because it is the persistent "desired" state.
    // Overwriting it every frame prevents users from changing it and makes savePersistedSettings save the current state rather than the persistent one.
    // m_cameraAddress = status.cameraAddress; <--- Removed to fix persistence bug

    // ── Display frame (fast-path from capture thread) ─────────────────────
    {
        cv::Mat frame;
        if (m_blackboard.consumeDisplayFrame(frame) && !frame.empty()) {
            m_renderer->updateTexture(frame);
            // Cache dimensions directly from frame — VisionState may lag on first frames
            if (m_cameraWidth  == 0) m_cameraWidth  = frame.cols;
            if (m_cameraHeight == 0) m_cameraHeight = frame.rows;
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
            ImGui::Separator();
            if (ImGui::MenuItem("Keyboard Shortcuts", "F1"))
                m_showShortcutHelp = true;
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

    // ── Panels & Windows ──────────────────────────────────────────────────
    if (m_showDataPanel)      renderDataPanel(m_trackedObjects, m_lockedTarget, m_cameraFps, m_renderFps);
    if (m_showZoomWindow)      renderZoomWindow(vision.zoomCrop, m_lockedTarget);
    if (m_showSettingsWindow)  renderSettingsWindow();
    if (m_showDevConsole)      renderDevConsole(vision, tracking, status);
    if (m_showTargetAnalyzer)  renderTargetAnalyzer(tracking);
    
    // Replay Control Panel
    if (m_replayPanel) m_replayPanel->render(m_blackboard);

    // CameraView (the big one)
    cv::Mat frameForHUD;
    m_blackboard.consumeDisplayFrame(frameForHUD);
    renderCameraView(frameForHUD, m_trackedObjects, m_lockedTarget, m_motionRegions, vision);

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
