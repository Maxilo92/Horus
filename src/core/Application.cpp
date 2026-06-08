#include "Application.hpp"
#include "../vision/VisionSystem.hpp"
#include "../tracking/TrackingSystem.hpp"
#include "../ui/UIManager.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
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

    auto logFn = [this](LogLevel l, const std::string& m) { this->log(l, m); };

    m_uiManager = std::make_unique<UIManager>(
        m_blackboard, *m_roiManager, *m_dataLogger, m_audioEngine,
        m_window, m_settingsPath, logFn);
    if (!m_uiManager->initRenderers()) {
        log(LogLevel::ERR, "UIManager renderer init failed");
        return false;
    }
    m_uiManager->start();

    // ── AI Dossier System ────────────────────────────────────────────────
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
        if (!m_dossierDb->init()) {
            log(LogLevel::ERR, "DossierDatabase init failed");
        }

        m_aiAgent = std::make_unique<AIAgent>(m_blackboard, *m_dossierDb, logFn);
        m_aiAgent->start();

        m_uiManager->setDossierDatabase(m_dossierDb.get());
    }

    // ── AudioCapture ─────────────────────────────────────────────────────
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
        
        SystemSettings s = m_blackboard.getSettings();
        if (s.audioCaptureEnabled) {
            m_audioCapture.start(s.audioCaptureDeviceId, [this](float intensity) {
                m_blackboard.updateAudioIntensity(intensity);
            });
        }
    }

    std::string finalCameraAddress = (argc > 1) ? argv[1] : m_blackboard.getAppStatus().cameraAddress;
    if (finalCameraAddress.empty()) finalCameraAddress = "1";

    // ── AudioEngine ──────────────────────────────────────────────────────
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

    // Find model paths — search order:
    //  1. macOS bundle Resources (../Resources/ relative to MacOS/ dir)
    //  2. Development build (../assets/models/ relative to binary in build/)
    //  3. CWD-relative (assets/models/ — works when launched from app dir)
    //  4. Exe-directory-relative (most reliable for installed release builds)
    auto exeDir = [&]() -> std::filesystem::path {
        return std::filesystem::path(argc > 0 ? argv[0] : ".").parent_path();
    };
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
    if (f) fclose(f);

    // ── TrackingSystem ───────────────────────────────────────────────────
    m_trackingSystem = std::make_unique<TrackingSystem>(m_blackboard, *m_roiManager, *m_dataLogger, m_audioEngine, logFn);
    
    // Setup Face Recognizer if models exist
    // Search order mirrors the YOLO model lookup: bundle Resources first, then project fallbacks.
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
    if (f) {
        fclose(f);
        if (!m_trackingSystem->init(faceDetPath, faceRecPath, m_dossierDb.get())) {
            log(LogLevel::ERR, "TrackingSystem face recognizer init failed");
        }
    }

    // ── VisionSystem ──────────────────────────────────────────────────────
    m_visionSystem = std::make_unique<VisionSystem>(m_blackboard, m_audioEngine, m_dossierDb.get(), m_aiAgent.get(), logFn);
    if (!m_visionSystem->init(finalCameraAddress, modelPath, labelsPath, finalCameraAddress)) {
        log(LogLevel::ERR, "VisionSystem init failed");
        return false;
    }

    m_visionSystem->setTrackingSystem(m_trackingSystem.get());

    m_visionSystem->start();
    m_trackingSystem->start();

    // ── UpdateChecker ─────────────────────────────────────────────────────
    m_updateChecker = std::make_unique<UpdateChecker>();
    m_uiManager->setUpdateChecker(m_updateChecker.get());
    m_updateChecker->checkAsync(5); // non-blocking; waits 5s before hitting API

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
