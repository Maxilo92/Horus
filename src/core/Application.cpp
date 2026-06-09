#include "Application.hpp"
#include "../vision/VisionSystem.hpp"
#include "../tracking/TrackingSystem.hpp"
#include "../ui/UIManager.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <imgui.h>
#include <iostream>
#include <cstdlib>
#include <filesystem>

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static std::string GetDefaultSettingsPath() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    std::filesystem::path p = base ? base : ".";
    p /= "Tactileviewer";
#else
    const char* home = std::getenv("HOME");
    std::filesystem::path p = home ? home : ".";
    p /= ".tactileviewer";
#endif
    std::filesystem::create_directories(p);
    return (p / "settings.ini").string();
}

// -----------------------------------------------------------------------
// Construction / Destruction
// -----------------------------------------------------------------------

Application::Application()
    : m_title("Project Horus - Target Viewer") {
    m_appStart = std::chrono::steady_clock::now();
    m_roiManager  = std::make_unique<ROIManager>();
    m_dataLogger  = std::make_unique<DataLogger>();
    log(LogLevel::INFO, "Application constructed");
}

Application::~Application() {
    cleanup();
}

// -----------------------------------------------------------------------
// Logging
// -----------------------------------------------------------------------

void Application::log(LogLevel level, const std::string& msg) {
    float t = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - m_appStart).count();

    if (m_uiManager) {
        m_uiManager->appendLog(level, msg, t);
    }

    if (level == LogLevel::ERR)       std::cerr << "[ERR]  " << msg << std::endl;
    else if (level == LogLevel::WARN) std::cout << "[WARN] " << msg << std::endl;
    else                              std::cout << "[INFO] " << msg << std::endl;
}

// -----------------------------------------------------------------------
// GLFW / ImGui init
// -----------------------------------------------------------------------

bool Application::initGLFW() {
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    m_window = glfwCreateWindow(m_width, m_height, m_title.c_str(), nullptr, nullptr);
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
// init
// -----------------------------------------------------------------------

bool Application::init(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    m_settingsPath = GetDefaultSettingsPath();

    const std::string cameraAddress = (argc > 1) ? argv[1] : "1";

    if (!initGLFW()) { log(LogLevel::ERR, "GLFW init failed"); return false; }
    if (!initImGui()) { log(LogLevel::ERR, "ImGui init failed"); return false; }

    // ── First frame: empty boot terminal ─────────────────────────────────
    drawSplashFrame();

    auto logFn = [this](LogLevel l, const std::string& m) { this->log(l, m); };

    // ── UI system ─────────────────────────────────────────────────────────
    bootBegin("UI system initializing...");
    m_uiManager = std::make_unique<UIManager>(
        m_blackboard, *m_roiManager, *m_dataLogger, m_audioEngine,
        m_window, m_settingsPath, logFn);
    if (!m_uiManager->initRenderers()) {
        bootEnd(false);
        log(LogLevel::ERR, "UIManager renderer init failed");
        return false;
    }
    m_uiManager->start();
    bootEnd(true);

    // ── AI Dossier System ────────────────────────────────────────────────
    bootBegin("Dossier database initializing...");
    {
#ifdef _WIN32
        const char* appData = std::getenv("APPDATA");
        std::filesystem::path dbPath = appData ? appData : ".";
        dbPath /= "Tactileviewer";
#else
        const char* home = std::getenv("HOME");
        std::filesystem::path dbPath = home ? home : ".";
        dbPath /= ".tactileviewer";
#endif
        std::filesystem::create_directories(dbPath);
        dbPath /= "dossiers.db";

        m_dossierDb = std::make_unique<DossierDatabase>(dbPath.string());
        bool dbOk = m_dossierDb->init();
        if (!dbOk) log(LogLevel::ERR, "DossierDatabase init failed");
        bootEnd(dbOk);

        bootBegin("AI agent starting...");
        m_aiAgent = std::make_unique<AIAgent>(m_blackboard, *m_dossierDb, logFn);
        m_aiAgent->start();
        bootEnd(true);

        m_uiManager->setDossierDatabase(m_dossierDb.get());
    }

    // ── AudioCapture ─────────────────────────────────────────────────────
    bootBegin("Enumerating audio & camera devices...");
    {
        auto devices = AudioCapture::listDevices();
        std::vector<std::string> names;
        std::vector<uint32_t> ids;
        for (const auto& d : devices) {
            names.push_back(d.name);
            ids.push_back(d.id);
        }
        m_blackboard.setAudioDevices(names, ids);

        auto camDevices = CameraModule::listDevices();
        std::vector<std::string> cNames;
        std::vector<int> cIds;
        for (const auto& d : camDevices) {
            cNames.push_back(d.name);
            cIds.push_back(d.id);
        }
        m_blackboard.setCameraDevices(cNames, cIds);
        bootEnd(true);

        SystemSettings s = m_blackboard.getSettings();
        if (s.audioCaptureEnabled) {
            bootBegin("Audio capture starting...");
            m_audioCapture.start(s.audioCaptureDeviceId, [this](float intensity) {
                m_blackboard.updateAudioIntensity(intensity);
            });
            bootEnd(true);
        }
    }

    std::string finalCameraAddress = (argc > 1) ? argv[1] : m_blackboard.getAppStatus().cameraAddress;
    if (finalCameraAddress.empty()) finalCameraAddress = "1";

    // ── AudioEngine ──────────────────────────────────────────────────────
    bootBegin("Audio engine configuring...");
    SystemSettings s = m_blackboard.getSettings();
    AudioEngine::Config cfg;
    cfg.masterEnabled       = s.audioEnabled;
    cfg.masterVolume        = s.audioMasterVolume;
    cfg.motionEnabled       = s.audioMotionEnabled;
    cfg.motionFreqHz        = s.audioMotionFreqHz;
    cfg.motionDurationMs    = s.audioMotionDurationMs;
    cfg.motionCooldownSec   = s.audioMotionCooldownSec;
    cfg.alarmEntryEnabled   = s.audioAlarmEntryEnabled;
    cfg.alarmEntryFreqHz    = s.audioAlarmEntryFreqHz;
    cfg.alarmEntryDurMs     = s.audioAlarmEntryDurMs;
    cfg.alarmExitEnabled    = s.audioAlarmExitEnabled;
    cfg.alarmExitFreqHz     = s.audioAlarmExitFreqHz;
    cfg.alarmExitDurMs      = s.audioAlarmExitDurMs;
    cfg.lockAcquiredEnabled = s.audioLockAcquiredEnabled;
    cfg.lockAcquiredFreqHz  = s.audioLockAcquiredFreqHz;
    cfg.lockAcquiredDurMs   = s.audioLockAcquiredDurMs;
    cfg.lockLostEnabled            = s.audioLockLostEnabled;
    cfg.lockLostFreqHz             = s.audioLockLostFreqHz;
    cfg.lockLostDurMs              = s.audioLockLostDurMs;
    cfg.lockPulseEnabled           = s.audioLockPulseEnabled;
    cfg.lockPulseFreqHz            = s.audioLockPulseFreqHz;
    cfg.lockPulseDurMs             = s.audioLockPulseDurMs;
    cfg.lockPulseMinIntervalMs     = s.audioLockPulseMinIntervalMs;
    cfg.lockPulseMaxIntervalMs     = s.audioLockPulseMaxIntervalMs;
    cfg.lockPulseSolutionThresh    = s.audioLockPulseSolutionThresh;
    cfg.lockPulseSolutionFreqHz    = s.audioLockPulseSolutionFreqHz;
    cfg.lockPulseSolutionDurMs     = s.audioLockPulseSolutionDurMs;
    m_audioEngine.applyConfig(cfg);
    bootEnd(true);

    // Find model paths — search order:
    //  1. macOS bundle Resources (../Resources/ relative to MacOS/ dir)
    //  2. Development build (../assets/models/ relative to binary in build/)
    //  3. CWD-relative (assets/models/ — works when launched from app dir)
    //  4. Exe-directory-relative (most reliable for installed release builds)
    //  5. User-data models directory (where the Setup Wizard downloads them)
    auto exeDir = [&]() -> std::filesystem::path {
        return std::filesystem::path(argc > 0 ? argv[0] : ".").parent_path();
    };
    auto userModelsDir = std::filesystem::path(m_settingsPath).parent_path() / "models";

    std::string modelPath  = "../Resources/yolov8n.onnx";
    std::string labelsPath = "../Resources/coco.txt";
    FILE* f = fopen(modelPath.c_str(), "r");
    if (!f) { modelPath = "../assets/models/yolov8n.onnx"; labelsPath = "../assets/models/coco.txt"; f = fopen(modelPath.c_str(), "r"); }
    if (!f) { modelPath = "assets/models/yolov8n.onnx";    labelsPath = "assets/models/coco.txt";    f = fopen(modelPath.c_str(), "r"); }
    if (!f) {
        modelPath  = (exeDir() / "assets" / "models" / "yolov8n.onnx").string();
        labelsPath = (exeDir() / "assets" / "models" / "coco.txt").string();
        f = fopen(modelPath.c_str(), "r");
    }
    if (!f) {
        modelPath  = (userModelsDir / "yolov8n.onnx").string();
        labelsPath = (userModelsDir / "coco.txt").string();
        f = fopen(modelPath.c_str(), "r");
    }
    if (f) fclose(f);

    // ── TrackingSystem ───────────────────────────────────────────────────
    bootBegin("Tracking system initializing...");
    m_trackingSystem = std::make_unique<TrackingSystem>(m_blackboard, *m_roiManager, *m_dataLogger, m_audioEngine, logFn);
    bootEnd(true);

    // Setup Face Recognizer if models exist
    bootBegin("Face recognition models loading...");
    std::string faceDetPath = "../Resources/face_detection_yunet_2023mar.onnx";
    std::string faceRecPath = "../Resources/face_recognition_sface_2021dec.onnx";
    f = fopen(faceDetPath.c_str(), "r");
    if (!f) { faceDetPath = "../assets/models/face_detection_yunet_2023mar.onnx"; faceRecPath = "../assets/models/face_recognition_sface_2021dec.onnx"; f = fopen(faceDetPath.c_str(), "r"); }
    if (!f) { faceDetPath = "assets/models/face_detection_yunet_2023mar.onnx";    faceRecPath = "assets/models/face_recognition_sface_2021dec.onnx";    f = fopen(faceDetPath.c_str(), "r"); }
    if (!f) {
        faceDetPath = (exeDir() / "assets" / "models" / "face_detection_yunet_2023mar.onnx").string();
        faceRecPath = (exeDir() / "assets" / "models" / "face_recognition_sface_2021dec.onnx").string();
        f = fopen(faceDetPath.c_str(), "r");
    }
    if (!f) {
        faceDetPath = (userModelsDir / "face_detection_yunet_2023mar.onnx").string();
        faceRecPath = (userModelsDir / "face_recognition_sface_2021dec.onnx").string();
        f = fopen(faceDetPath.c_str(), "r");
    }
    if (f) {
        fclose(f);
        bool faceOk = m_trackingSystem->init(faceDetPath, faceRecPath, m_dossierDb.get());
        if (!faceOk) log(LogLevel::ERR, "TrackingSystem face recognizer init failed");
        bootEnd(faceOk);
    } else {
        bootEnd(false);  // models not found — non-fatal
    }

    // ── VisionSystem ──────────────────────────────────────────────────────
    bootBegin("Object detection model loading...");
    m_visionSystem = std::make_unique<VisionSystem>(m_blackboard, m_audioEngine, m_dossierDb.get(), m_aiAgent.get(), logFn);
    if (!m_visionSystem->init(finalCameraAddress, modelPath, labelsPath, finalCameraAddress)) {
        bootEnd(false);
        log(LogLevel::ERR, "VisionSystem init failed");
        return false;
    }
    bootEnd(true);

    m_visionSystem->setTrackingSystem(m_trackingSystem.get());

    bootBegin("Camera stream starting...");
    m_visionSystem->start();
    m_trackingSystem->start();
    bootEnd(true);

    // ── UpdateChecker ─────────────────────────────────────────────────────
    m_updateChecker = std::make_unique<UpdateChecker>();
    m_uiManager->setUpdateChecker(m_updateChecker.get());
    m_updateChecker->checkAsync(5);

    return true;
}

// -----------------------------------------------------------------------
// run
// -----------------------------------------------------------------------

void Application::run() {
    SystemSettings initialSettings = m_blackboard.getSettings();
    uint32_t lastAudioId = initialSettings.audioCaptureDeviceId;
    bool lastAudioEnabled = initialSettings.audioCaptureEnabled;

    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();
        m_uiManager->update();

        SystemSettings s = m_blackboard.getSettings();
        if (s.audioCaptureEnabled != lastAudioEnabled || s.audioCaptureDeviceId != lastAudioId) {
            if (s.audioCaptureEnabled) {
                log(LogLevel::INFO, "Restarting audio capture with device ID: " + std::to_string(s.audioCaptureDeviceId));
                m_audioCapture.start(s.audioCaptureDeviceId, [this](float intensity) {
                    m_blackboard.updateAudioIntensity(intensity);
                });
            } else {
                log(LogLevel::INFO, "Stopping audio capture");
                m_audioCapture.stop();
            }
            lastAudioEnabled = s.audioCaptureEnabled;
            lastAudioId = s.audioCaptureDeviceId;
        }
    }
}

// -----------------------------------------------------------------------
// cleanup
// -----------------------------------------------------------------------

void Application::cleanup() {
    if (m_visionSystem)   m_visionSystem->stop();
    if (m_trackingSystem) m_trackingSystem->stop();
    if (m_aiAgent)        m_aiAgent->stop();
    if (m_uiManager)      m_uiManager->stop();

    m_visionSystem.reset();
    m_trackingSystem.reset();
    m_aiAgent.reset();
    m_dossierDb.reset();
    m_uiManager.reset();

    curl_global_cleanup();

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

// -----------------------------------------------------------------------
// Boot helpers
// -----------------------------------------------------------------------

void Application::bootBegin(const std::string& msg) {
    m_bootLog.push_back({ msg, BootStatus::PENDING });
    drawSplashFrame();
}

void Application::bootEnd(bool ok) {
    if (!m_bootLog.empty())
        m_bootLog.back().status = ok ? BootStatus::OK : BootStatus::FAIL;
    drawSplashFrame();
}

// -----------------------------------------------------------------------
// drawSplashFrame  –  boot terminal, re-rendered on every bootBegin/bootEnd
// -----------------------------------------------------------------------

void Application::drawSplashFrame() {
    glfwPollEvents();   // keep OS happy during long init

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float W  = vp->Size.x;
    const float H  = vp->Size.y;
    const float ox = vp->Pos.x;
    const float oy = vp->Pos.y;
    auto P = [&](float x, float y) { return ImVec2(ox + x, oy + y); };

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.020f, 0.035f, 0.020f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("##boot", nullptr,
        ImGuiWindowFlags_NoDecoration    |
        ImGuiWindowFlags_NoInputs        |
        ImGuiWindowFlags_NoNav           |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 kBg       = IM_COL32( 5,  9,  5, 255);
    const ImU32 kGreen    = IM_COL32( 0, 200, 100, 255);
    const ImU32 kGreenDim = IM_COL32( 0, 120,  60, 200);
    const ImU32 kYellow   = IM_COL32(220, 200,  40, 255);
    const ImU32 kRed      = IM_COL32(220,  60,  60, 255);
    const ImU32 kGray     = IM_COL32(100, 130, 100, 200);

    dl->AddRectFilled(P(0, 0), P(W, H), kBg);

    // Faint grid
    for (float x = 0; x < W; x += 60.0f)
        dl->AddLine(P(x, 0), P(x, H), IM_COL32(0, 180, 80, 6));
    for (float y = 0; y < H; y += 60.0f)
        dl->AddLine(P(0, y), P(W, y), IM_COL32(0, 180, 80, 6));

    // Corner brackets
    const float bm = 20.0f, bL = 32.0f, bT = 1.5f;
    dl->AddLine(P(bm,   bm),   P(bm+bL, bm),   kGreenDim, bT);
    dl->AddLine(P(bm,   bm),   P(bm,   bm+bL), kGreenDim, bT);
    dl->AddLine(P(W-bm, bm),   P(W-bm-bL, bm), kGreenDim, bT);
    dl->AddLine(P(W-bm, bm),   P(W-bm, bm+bL), kGreenDim, bT);
    dl->AddLine(P(bm,   H-bm), P(bm+bL, H-bm), kGreenDim, bT);
    dl->AddLine(P(bm,   H-bm), P(bm,   H-bm-bL), kGreenDim, bT);
    dl->AddLine(P(W-bm, H-bm), P(W-bm-bL, H-bm), kGreenDim, bT);
    dl->AddLine(P(W-bm, H-bm), P(W-bm,   H-bm-bL), kGreenDim, bT);

    const float lh     = ImGui::GetTextLineHeight();
    const float margin = 60.0f;   // left margin for log lines
    float curY         = 36.0f;   // current Y cursor

    // ── Header ──────────────────────────────────────────────────────────
    {
        const char* title = "PROJECT HORUS  //  TARGET ACQUISITION SYSTEM";
        ImGui::SetCursorScreenPos(P(margin, curY));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.784f, 0.392f, 1.0f));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        curY += lh + 4.0f;
    }
    {
        const char* sub = "BOOT SEQUENCE";
        ImGui::SetCursorScreenPos(P(margin, curY));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.45f, 0.22f, 1.0f));
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
        curY += lh + 2.0f;
    }

    // Separator line
    dl->AddLine(P(margin, curY + 4.0f), P(W - margin, curY + 4.0f), kGreenDim, 1.0f);
    curY += 16.0f;

    // ── Boot log entries ────────────────────────────────────────────────
    const float tagW = 70.0f;   // width of the status badge column
    for (const auto& entry : m_bootLog) {
        // Status badge
        const char* tag;
        ImU32       tagCol;
        switch (entry.status) {
            case BootStatus::OK:      tag = "[  OK  ]"; tagCol = kGreen;  break;
            case BootStatus::FAIL:    tag = "[ FAIL ]"; tagCol = kRed;    break;
            default:                  tag = "[ .... ]"; tagCol = kYellow; break;
        }
        ImGui::SetCursorScreenPos(P(margin, curY));
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImVec4(((tagCol >>  0) & 0xFF) / 255.0f,
                   ((tagCol >>  8) & 0xFF) / 255.0f,
                   ((tagCol >> 16) & 0xFF) / 255.0f, 1.0f));
        ImGui::TextUnformatted(tag);
        ImGui::PopStyleColor();

        // Message text
        ImGui::SetCursorScreenPos(P(margin + tagW + 8.0f, curY));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.88f, 0.72f, 1.0f));
        ImGui::TextUnformatted(entry.msg.c_str());
        ImGui::PopStyleColor();

        curY += lh + 3.0f;
    }

    // Blinking cursor after last line (only if something is pending)
    bool anyPending = !m_bootLog.empty() &&
                      m_bootLog.back().status == BootStatus::PENDING;
    if (anyPending) {
        ImGui::SetCursorScreenPos(P(margin + tagW + 8.0f, curY + 2.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.784f, 0.392f, 0.9f));
        ImGui::TextUnformatted("_");
        ImGui::PopStyleColor();
    }

    // ── Version footer ──────────────────────────────────────────────────
    {
        const char* ver = "HORUS  v0.1";
        ImVec2 vs = ImGui::CalcTextSize(ver);
        ImGui::SetCursorScreenPos(P(W - margin - vs.x, H - bm - lh));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.28f, 0.14f, 1.0f));
        ImGui::TextUnformatted(ver);
        ImGui::PopStyleColor();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    int fw, fh;
    glfwGetFramebufferSize(m_window, &fw, &fh);
    ImGui::Render();
    glViewport(0, 0, fw, fh);
    glClearColor(0.020f, 0.035f, 0.020f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Multi-viewport contract: with ViewportsEnable set, every frame must end with these
    // calls (same as the main render loop in UIManager). Without them, the next NewFrame()
    // aborts with the "Forgot to call UpdatePlatformWindows()" sanity-check assertion.
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }

    glfwSwapBuffers(m_window);
}
