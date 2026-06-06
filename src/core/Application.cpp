#include "Application.hpp"
#include <iostream>
#include <cstring>
#include <cmath>
#include <cinttypes>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <ctime>
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

static constexpr ImU32 kDefaultHudColor = IM_COL32(0, 200, 100, 220);
static constexpr ImU32 kDefaultTargetColor = IM_COL32(255, 180, 0, 255);

static std::string GetDefaultSettingsPath() {
    const char* home = std::getenv("HOME");
    std::filesystem::path base = home ? home : ".";
    base /= ".tactileviewer";
    return (base / "settings.ini").string();
}

static bool ParseBoolSetting(const std::string& value) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
}

static std::set<int> ParsePriorityClasses(const std::string& value) {
    std::set<int> classes;
    std::stringstream stream(value);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (token.empty()) continue;
        try {
            classes.insert(std::stoi(token));
        } catch (...) {
        }
    }
    return classes;
}

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

static std::string SerializePriorityClasses(const std::set<int>& classes) {
    std::ostringstream out;
    bool first = true;
    for (int classId : classes) {
        if (!first) out << ',';
        out << classId;
        first = false;
    }
    return out.str();
}

static std::string EscapeJsonString(const std::string& value) {
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec << std::setfill(' ');
                } else {
                    out << ch;
                }
                break;
        }
    }
    return out.str();
}

static SystemSettings MakeStandardSettings() {
    SystemSettings settings;
    settings.hudColor = kDefaultHudColor;
    settings.targetColor = kDefaultTargetColor;
    return settings;
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
    m_heatmapRenderer = std::make_unique<VideoRenderer>();
    m_zoomRenderer = std::make_unique<VideoRenderer>();
    for (int i = 0; i < 4; ++i) {
        m_subZoomRenderers[i] = std::make_unique<VideoRenderer>();
    }
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
    curl_global_init(CURL_GLOBAL_DEFAULT);
    m_settingsPath = GetDefaultSettingsPath();
    loadPersistedSettings();

    if (argc > 1) {
        m_cameraAddress = argv[1];
    }
    if (m_cameraAddress.empty()) {
        m_cameraAddress = "1";
    }
    snprintf(m_cameraInputBuf, sizeof(m_cameraInputBuf), "%s", m_cameraAddress.c_str());
    syncSettingsToSharedState();

    log(LogLevel::INFO, "Opening camera: " + m_cameraAddress);
    int requestedW = m_settings.request4KCamera ? 3840 : 1280;
    int requestedH = m_settings.request4KCamera ? 2160 : 720;
    if (!m_camera->open(m_cameraAddress, requestedW, requestedH)) {
        log(LogLevel::ERR, "Camera failed to open: " + m_cameraAddress);
        std::cerr << "[ERROR] Camera fail: " << m_cameraAddress << std::endl;
        return false;
    }
    int actualW = m_camera->getWidth();
    int actualH = m_camera->getHeight();
    std::string backend = m_camera->getBackendName();
    log(LogLevel::INFO, "Camera opened successfully (backend=" + backend + ")");
    log(LogLevel::INFO, "Camera resolution requested=" + std::to_string(requestedW) + "x" + std::to_string(requestedH) +
        ", actual=" + std::to_string(actualW) + "x" + std::to_string(actualH));
    if (actualW < requestedW || actualH < requestedH) {
        log(LogLevel::WARN, "Camera did not accept requested 4K mode. Source likely limited to " +
            std::to_string(actualW) + "x" + std::to_string(actualH));
    }

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

        // --- Face Recognizer (Plan 11) ---
        std::string faceDetPath = "../Resources/face_detection_yunet_2023mar.onnx";
        std::string faceRecPath = "../Resources/face_recognition_sface_2021dec.onnx";
        FILE* f2 = fopen(faceDetPath.c_str(), "r");
        if (!f2) { faceDetPath = "../assets/models/face_detection_yunet_2023mar.onnx"; faceRecPath = "../assets/models/face_recognition_sface_2021dec.onnx"; f2 = fopen(faceDetPath.c_str(), "r"); }
        if (!f2) { faceDetPath = "assets/models/face_detection_yunet_2023mar.onnx";    faceRecPath = "assets/models/face_recognition_sface_2021dec.onnx";    f2 = fopen(faceDetPath.c_str(), "r"); }
        if (f2) {
            fclose(f2);
            log(LogLevel::INFO, "Loading face models: " + faceDetPath);
            m_faceRecognizer = std::make_unique<FaceRecognizer>(faceDetPath, faceRecPath);
            log(LogLevel::INFO, "Face Recognizer initialized");
        } else {
            log(LogLevel::WARN, "Face recognition models not found. Feature disabled.");
        }
    } catch (const std::exception& e) {
        log(LogLevel::ERR, std::string("Detector init failed: ") + e.what());
        std::cerr << "[ERROR] Detector fail: " << e.what() << std::endl;
        return false;
    }

    if (!initGLFW()) { log(LogLevel::ERR, "GLFW init failed"); return false; }
    if (!initImGui()) { log(LogLevel::ERR, "ImGui init failed"); return false; }

    syncColorEditorsFromSettings();

    // ── Audio Engine ──────────────────────────────────────────────────────
    {
        AudioEngine::Config audioCfg;
        audioCfg.masterEnabled       = m_settings.audioEnabled;
        audioCfg.masterVolume        = m_settings.audioMasterVolume;
        audioCfg.motionEnabled       = m_settings.audioMotionEnabled;
        audioCfg.motionFreqHz        = m_settings.audioMotionFreqHz;
        audioCfg.motionDurationMs    = m_settings.audioMotionDurationMs;
        audioCfg.motionCooldownSec   = m_settings.audioMotionCooldownSec;
        audioCfg.alarmEntryEnabled   = m_settings.audioAlarmEntryEnabled;
        audioCfg.alarmEntryFreqHz    = m_settings.audioAlarmEntryFreqHz;
        audioCfg.alarmEntryDurMs     = m_settings.audioAlarmEntryDurMs;
        audioCfg.alarmExitEnabled    = m_settings.audioAlarmExitEnabled;
        audioCfg.alarmExitFreqHz     = m_settings.audioAlarmExitFreqHz;
        audioCfg.alarmExitDurMs      = m_settings.audioAlarmExitDurMs;
        audioCfg.lockAcquiredEnabled = m_settings.audioLockAcquiredEnabled;
        audioCfg.lockAcquiredFreqHz  = m_settings.audioLockAcquiredFreqHz;
        audioCfg.lockAcquiredDurMs   = m_settings.audioLockAcquiredDurMs;
        audioCfg.lockLostEnabled     = m_settings.audioLockLostEnabled;
        audioCfg.lockLostFreqHz      = m_settings.audioLockLostFreqHz;
        audioCfg.lockLostDurMs       = m_settings.audioLockLostDurMs;
        m_audioEngine.init(audioCfg);
        log(LogLevel::INFO, "Audio engine initialised");
    }

    log(LogLevel::INFO, "System initialized — starting threads");

    m_running = true;
    m_captureThread  = std::thread(&Application::captureWorkerLoop, this);
    m_detectorThread = std::thread(&Application::detectorWorkerLoop, this);
    m_trackingThread = std::thread(&Application::trackingWorkerLoop, this);
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

void Application::syncColorEditorsFromSettings() {
    ImU32 hudColor = (m_settings.hudColor != 0) ? m_settings.hudColor : kDefaultHudColor;
    ImU32 targetColor = (m_settings.targetColor != 0) ? m_settings.targetColor : kDefaultTargetColor;
    ImU32 motionColor = (m_settings.motionOverlayColor != 0) ? m_settings.motionOverlayColor : kDefaultMotionColor;
    ImU32ToFloat4(hudColor, m_hudColorF);
    ImU32ToFloat4(targetColor, m_targetColorF);
    ImU32ToFloat4(motionColor, m_motionOverlayColorF);
    m_settings.hudColor          = hudColor;
    m_settings.targetColor       = targetColor;
    m_settings.motionOverlayColor = motionColor;
}

void Application::syncSettingsToSharedState() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_sharedSettings = m_settings;
}

void Application::savePersistedSettings() const {
    std::filesystem::path settingsPath(m_settingsPath.empty() ? GetDefaultSettingsPath() : m_settingsPath);
    std::error_code ec;
    std::filesystem::create_directories(settingsPath.parent_path(), ec);

    const std::string cameraAddress = m_cameraAddress.empty() ? "1" : m_cameraAddress;

    std::ofstream out(settingsPath);
    if (!out) {
        std::cerr << "[WARN] Could not save settings to " << settingsPath.string() << std::endl;
        return;
    }

    out << "# Tactileviewer persistent settings\n";
    out << "cameraAddress=" << cameraAddress << '\n';
    out << "showSettingsWindow=" << (m_showSettingsWindow ? 1 : 0) << '\n';
    out << "showDevConsole=" << (m_showDevConsole ? 1 : 0) << '\n';
    out << "showDataPanel=" << (m_showDataPanel ? 1 : 0) << '\n';
    out << "showZoomWindow=" << (m_showZoomWindow ? 1 : 0) << '\n';
    out << "showTargetAnalyzer=" << (m_showTargetAnalyzer ? 1 : 0) << '\n';
    out << "remoteInferenceEnabled=" << (m_settings.remoteInferenceEnabled ? 1 : 0) << '\n';
    out << "remoteInferenceIp=" << m_settings.remoteInferenceIp << '\n';
    out << "remoteInferencePort=" << m_settings.remoteInferencePort << '\n';
    out << "detectorConfThreshold=" << m_settings.detectorConfThreshold << '\n';
    out << "detectorScoreThreshold=" << m_settings.detectorScoreThreshold << '\n';
    out << "detectorNmsThreshold=" << m_settings.detectorNmsThreshold << '\n';
    out << "filterByPriorityClasses=" << (m_settings.filterByPriorityClasses ? 1 : 0) << '\n';
    out << "priorityClasses=" << SerializePriorityClasses(m_settings.priorityClasses) << '\n';
    out << "trackerMaxLostFrames=" << m_settings.trackerMaxLostFrames << '\n';
    out << "trackerMinMatchIOU=" << m_settings.trackerMinMatchIOU << '\n';
    out << "trackerMaxTrailLength=" << m_settings.trackerMaxTrailLength << '\n';
    out << "showTrails=" << (m_settings.showTrails ? 1 : 0) << '\n';
    out << "trackerMinMatchScore=" << m_settings.trackerMinMatchScore << '\n';
    out << "trackerMaxCenterDistPx=" << m_settings.trackerMaxCenterDistPx << '\n';
    out << "trackerReacquisitionEnabled=" << (m_settings.trackerReacquisitionEnabled ? 1 : 0) << '\n';
    out << "trackerReacquisitionZoom=" << m_settings.trackerReacquisitionZoom << '\n';
    out << "trackerUseMotionFallback=" << (m_settings.trackerUseMotionFallback ? 1 : 0) << '\n';
    out << "trackerReacquisitionMaxDist=" << m_settings.trackerReacquisitionMaxDist << '\n';
    out << "trackerConfirmFrames=" << m_settings.trackerConfirmFrames << '\n';
    out << "trackerVelocitySmoothing=" << m_settings.trackerVelocitySmoothing << '\n';
    out << "trackerDeadReckoningDamping=" << m_settings.trackerDeadReckoningDamping << '\n';
    out << "showTacticalOverlay=" << (m_settings.showTacticalOverlay ? 1 : 0) << '\n';
    out << "showCrosshair=" << (m_settings.showCrosshair ? 1 : 0) << '\n';
    out << "showCornerBrackets=" << (m_settings.showCornerBrackets ? 1 : 0) << '\n';
    out << "showStatusWindows=" << (m_settings.showStatusWindows ? 1 : 0) << '\n';
    out << "showDetections=" << (m_settings.showDetections ? 1 : 0) << '\n';
    out << "showTrackIDs=" << (m_settings.showTrackIDs ? 1 : 0) << '\n';
    out << "showConfidence=" << (m_settings.showConfidence ? 1 : 0) << '\n';
    out << "showTrailFade=" << (m_settings.showTrailFade ? 1 : 0) << '\n';
    out << "hudBrightness=" << m_settings.hudBrightness << '\n';
    out << "crosshairScale=" << m_settings.crosshairScale << '\n';
    out << "boxLineWidth=" << m_settings.boxLineWidth << '\n';
    out << "hudColor=" << m_settings.hudColor << '\n';
    out << "targetColor=" << m_settings.targetColor << '\n';
    out << "enableDetection=" << (m_settings.enableDetection ? 1 : 0) << '\n';
    out << "enableTracking=" << (m_settings.enableTracking ? 1 : 0) << '\n';
    out << "detectionSkipFrames=" << m_settings.detectionSkipFrames << '\n';
    out << "grayscaleInput=" << (m_settings.grayscaleInput ? 1 : 0) << '\n';
    out << "logLevel=" << m_settings.logLevel << '\n';
    out << "logToFile=" << (m_settings.logToFile ? 1 : 0) << '\n';
    out << "dataLoggingEnabled=" << (m_settings.dataLoggingEnabled ? 1 : 0) << '\n';
    out << "dataLoggingFormat=" << m_settings.dataLoggingFormat << '\n';
    out << "dataLoggingFreqFrames=" << m_settings.dataLoggingFreqFrames << '\n';
    out << "dataLoggingOutputDir=" << m_settings.dataLoggingOutputDir << '\n';
    out << "exportOutputDir=" << m_settings.exportOutputDir << '\n';
    out << "showROIOverlay=" << (m_settings.showROIOverlay ? 1 : 0) << '\n';
    out << "request4KCamera=" << (m_settings.request4KCamera ? 1 : 0) << '\n';
    out << "enable4KZoom=" << (m_settings.enable4KZoom ? 1 : 0) << '\n';
    out << "targetZoomMagnification=" << m_settings.targetZoomMagnification << '\n';
    out << "lowLightEnhancement=" << (m_settings.lowLightEnhancement ? 1 : 0) << '\n';
    out << "lowLightClipLimit=" << m_settings.lowLightClipLimit << '\n';
    out << "lowLightDenoiseKernel=" << m_settings.lowLightDenoiseKernel << '\n';
    out << "motionDetectionEnabled=" << (m_settings.motionDetectionEnabled ? 1 : 0) << '\n';
    out << "motionShowOverlay=" << (m_settings.motionShowOverlay ? 1 : 0) << '\n';
    out << "motionHeatmapOverlay=" << (m_settings.motionHeatmapOverlay ? 1 : 0) << '\n';
    out << "motionHeatmapDecay=" << m_settings.motionHeatmapDecay << '\n';
    out << "motionHeatmapSensitivity=" << m_settings.motionHeatmapSensitivity << '\n';
    out << "motionHeatmapAlpha=" << m_settings.motionHeatmapAlpha << '\n';
    out << "motionSensitivity=" << m_settings.motionSensitivity << '\n';
    out << "motionMinArea=" << m_settings.motionMinArea << '\n';
    out << "motionBlurKernel=" << m_settings.motionBlurKernel << '\n';
    out << "motionOverlayAlpha=" << m_settings.motionOverlayAlpha << '\n';
    out << "motionOverlayColor=" << m_settings.motionOverlayColor << '\n';
    out << "motionDetectShadows=" << (m_settings.motionDetectShadows ? 1 : 0) << '\n';
    out << "motionLearningRate=" << m_settings.motionLearningRate << '\n';
    out << "subZoomsEnabled=" << (m_settings.subZoomsEnabled ? 1 : 0) << '\n';
    out << "subZoomsUseSeparateWindows=" << (m_settings.subZoomsUseSeparateWindows ? 1 : 0) << '\n';
    out << "subZoomPaddingPx=" << m_settings.subZoomPaddingPx << '\n';
    out << "subZoomMagnification=" << m_settings.subZoomMagnification << '\n';
    // Face Recognition Settings (Plan 11)
    out << "faceRecognitionEnabled=" << (m_settings.faceRecognitionEnabled ? 1 : 0) << '\n';
    out << "faceRecognitionThreshold=" << m_settings.faceRecognitionThreshold << '\n';
    // Audio Feedback Settings
    out << "audioEnabled=" << (m_settings.audioEnabled ? 1 : 0) << '\n';
    out << "audioMasterVolume=" << m_settings.audioMasterVolume << '\n';
    out << "audioMotionEnabled=" << (m_settings.audioMotionEnabled ? 1 : 0) << '\n';
    out << "audioMotionFreqHz=" << m_settings.audioMotionFreqHz << '\n';
    out << "audioMotionDurationMs=" << m_settings.audioMotionDurationMs << '\n';
    out << "audioMotionCooldownSec=" << m_settings.audioMotionCooldownSec << '\n';
    out << "audioAlarmEntryEnabled=" << (m_settings.audioAlarmEntryEnabled ? 1 : 0) << '\n';
    out << "audioAlarmEntryFreqHz=" << m_settings.audioAlarmEntryFreqHz << '\n';
    out << "audioAlarmEntryDurMs=" << m_settings.audioAlarmEntryDurMs << '\n';
    out << "audioAlarmExitEnabled=" << (m_settings.audioAlarmExitEnabled ? 1 : 0) << '\n';
    out << "audioAlarmExitFreqHz=" << m_settings.audioAlarmExitFreqHz << '\n';
    out << "audioAlarmExitDurMs=" << m_settings.audioAlarmExitDurMs << '\n';
    out << "audioLockAcquiredEnabled=" << (m_settings.audioLockAcquiredEnabled ? 1 : 0) << '\n';
    out << "audioLockAcquiredFreqHz=" << m_settings.audioLockAcquiredFreqHz << '\n';
    out << "audioLockAcquiredDurMs=" << m_settings.audioLockAcquiredDurMs << '\n';
    out << "audioLockLostEnabled=" << (m_settings.audioLockLostEnabled ? 1 : 0) << '\n';
    out << "audioLockLostFreqHz=" << m_settings.audioLockLostFreqHz << '\n';
    out << "audioLockLostDurMs=" << m_settings.audioLockLostDurMs << '\n';
}


void Application::loadPersistedSettings() {
    m_settings = MakeStandardSettings();
    syncColorEditorsFromSettings();

    std::filesystem::path settingsPath(m_settingsPath.empty() ? GetDefaultSettingsPath() : m_settingsPath);
    std::ifstream in(settingsPath);
    if (!in) {
        if (m_cameraAddress.empty()) {
            m_cameraAddress = "1";
        }
        std::filesystem::create_directories(settingsPath.parent_path());
        savePersistedSettings();
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) continue;

        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);

        try {
            if (key == "cameraAddress") m_cameraAddress = value;
            else if (key == "showSettingsWindow") m_showSettingsWindow = ParseBoolSetting(value);
            else if (key == "showDevConsole") m_showDevConsole = ParseBoolSetting(value);
            else if (key == "showDataPanel") m_showDataPanel = ParseBoolSetting(value);
            else if (key == "showZoomWindow") m_showZoomWindow = ParseBoolSetting(value);
            else if (key == "showTargetAnalyzer") m_showTargetAnalyzer = ParseBoolSetting(value);
            else if (key == "remoteInferenceEnabled") m_settings.remoteInferenceEnabled = ParseBoolSetting(value);
            else if (key == "remoteInferenceIp") m_settings.remoteInferenceIp = value;
            else if (key == "remoteInferencePort") m_settings.remoteInferencePort = std::stoi(value);
            else if (key == "detectorConfThreshold") m_settings.detectorConfThreshold = std::stof(value);
            else if (key == "detectorScoreThreshold") m_settings.detectorScoreThreshold = std::stof(value);
            else if (key == "detectorNmsThreshold") m_settings.detectorNmsThreshold = std::stof(value);
            else if (key == "filterByPriorityClasses") m_settings.filterByPriorityClasses = ParseBoolSetting(value);
            else if (key == "priorityClasses") m_settings.priorityClasses = ParsePriorityClasses(value);
            else if (key == "trackerMaxLostFrames") m_settings.trackerMaxLostFrames = std::stoi(value);
            else if (key == "trackerMinMatchIOU") m_settings.trackerMinMatchIOU = std::stof(value);
            else if (key == "trackerMaxTrailLength") m_settings.trackerMaxTrailLength = std::stoi(value);
            else if (key == "showTrails") m_settings.showTrails = ParseBoolSetting(value);
            else if (key == "trackerMinMatchScore") m_settings.trackerMinMatchScore = std::stof(value);
            else if (key == "trackerMaxCenterDistPx") m_settings.trackerMaxCenterDistPx = std::stof(value);
            else if (key == "trackerReacquisitionEnabled") m_settings.trackerReacquisitionEnabled = ParseBoolSetting(value);
            else if (key == "trackerReacquisitionZoom") m_settings.trackerReacquisitionZoom = std::stof(value);
            else if (key == "trackerUseMotionFallback") m_settings.trackerUseMotionFallback = ParseBoolSetting(value);
            else if (key == "trackerReacquisitionMaxDist") m_settings.trackerReacquisitionMaxDist = std::stof(value);
            else if (key == "trackerConfirmFrames") m_settings.trackerConfirmFrames = std::stoi(value);
            else if (key == "trackerVelocitySmoothing") m_settings.trackerVelocitySmoothing = std::stof(value);
            else if (key == "trackerDeadReckoningDamping") m_settings.trackerDeadReckoningDamping = std::stof(value);
            else if (key == "showTacticalOverlay") m_settings.showTacticalOverlay = ParseBoolSetting(value);
            else if (key == "showCrosshair") m_settings.showCrosshair = ParseBoolSetting(value);
            else if (key == "showCornerBrackets") m_settings.showCornerBrackets = ParseBoolSetting(value);
            else if (key == "showStatusWindows") m_settings.showStatusWindows = ParseBoolSetting(value);
            else if (key == "showDetections") m_settings.showDetections = ParseBoolSetting(value);
            else if (key == "showTrackIDs") m_settings.showTrackIDs = ParseBoolSetting(value);
            else if (key == "showConfidence") m_settings.showConfidence = ParseBoolSetting(value);
            else if (key == "showTrailFade") m_settings.showTrailFade = ParseBoolSetting(value);
            else if (key == "hudBrightness") m_settings.hudBrightness = std::stof(value);
            else if (key == "crosshairScale") m_settings.crosshairScale = std::stof(value);
            else if (key == "boxLineWidth") m_settings.boxLineWidth = std::stof(value);
            else if (key == "hudColor") m_settings.hudColor = static_cast<uint32_t>(std::stoul(value));
            else if (key == "targetColor") m_settings.targetColor = static_cast<uint32_t>(std::stoul(value));
            else if (key == "enableDetection") m_settings.enableDetection = ParseBoolSetting(value);
            else if (key == "enableTracking") m_settings.enableTracking = ParseBoolSetting(value);
            else if (key == "detectionSkipFrames") m_settings.detectionSkipFrames = std::stoi(value);
            else if (key == "grayscaleInput") m_settings.grayscaleInput = ParseBoolSetting(value);
            else if (key == "logLevel") m_settings.logLevel = std::stoi(value);
            else if (key == "logToFile") m_settings.logToFile = ParseBoolSetting(value);
            else if (key == "dataLoggingEnabled") m_settings.dataLoggingEnabled = ParseBoolSetting(value);
            else if (key == "dataLoggingFormat") m_settings.dataLoggingFormat = std::stoi(value);
            else if (key == "dataLoggingFreqFrames") m_settings.dataLoggingFreqFrames = std::stoi(value);
            else if (key == "dataLoggingOutputDir") m_settings.dataLoggingOutputDir = value;
            else if (key == "exportOutputDir") m_settings.exportOutputDir = value;
            else if (key == "showROIOverlay") m_settings.showROIOverlay = ParseBoolSetting(value);
            else if (key == "request4KCamera") m_settings.request4KCamera = ParseBoolSetting(value);
            else if (key == "enable4KZoom") m_settings.enable4KZoom = ParseBoolSetting(value);
            else if (key == "targetZoomMagnification") m_settings.targetZoomMagnification = std::stof(value);
            else if (key == "lowLightEnhancement") m_settings.lowLightEnhancement = ParseBoolSetting(value);
            else if (key == "lowLightClipLimit") m_settings.lowLightClipLimit = std::stof(value);
            else if (key == "lowLightDenoiseKernel") m_settings.lowLightDenoiseKernel = std::stoi(value);
            else if (key == "motionDetectionEnabled") m_settings.motionDetectionEnabled = ParseBoolSetting(value);
            else if (key == "motionShowOverlay") m_settings.motionShowOverlay = ParseBoolSetting(value);
            else if (key == "motionHeatmapOverlay") m_settings.motionHeatmapOverlay = ParseBoolSetting(value);
            else if (key == "motionHeatmapDecay") m_settings.motionHeatmapDecay = std::stof(value);
            else if (key == "motionHeatmapSensitivity") m_settings.motionHeatmapSensitivity = std::stof(value);
            else if (key == "motionHeatmapAlpha") m_settings.motionHeatmapAlpha = std::stof(value);
            else if (key == "motionSensitivity") m_settings.motionSensitivity = std::stof(value);
            else if (key == "motionMinArea") m_settings.motionMinArea = std::stoi(value);
            else if (key == "motionBlurKernel") m_settings.motionBlurKernel = std::stoi(value);
            else if (key == "motionOverlayAlpha") m_settings.motionOverlayAlpha = std::stof(value);
            else if (key == "motionOverlayColor") m_settings.motionOverlayColor = static_cast<uint32_t>(std::stoul(value));
            else if (key == "motionDetectShadows") m_settings.motionDetectShadows = ParseBoolSetting(value);
            else if (key == "motionLearningRate") m_settings.motionLearningRate = std::stoi(value);
            else if (key == "subZoomsEnabled") m_settings.subZoomsEnabled = ParseBoolSetting(value);
            else if (key == "subZoomsUseSeparateWindows") m_settings.subZoomsUseSeparateWindows = ParseBoolSetting(value);
            else if (key == "subZoomPaddingPx") m_settings.subZoomPaddingPx = std::stoi(value);
            else if (key == "subZoomMagnification") m_settings.subZoomMagnification = std::stof(value);
            // Face Recognition Settings (Plan 11)
            else if (key == "faceRecognitionEnabled") m_settings.faceRecognitionEnabled = ParseBoolSetting(value);
            else if (key == "faceRecognitionThreshold") m_settings.faceRecognitionThreshold = std::stof(value);
            // Audio Feedback Settings
            else if (key == "audioEnabled") m_settings.audioEnabled = ParseBoolSetting(value);
            else if (key == "audioMasterVolume") m_settings.audioMasterVolume = std::stof(value);
            else if (key == "audioMotionEnabled") m_settings.audioMotionEnabled = ParseBoolSetting(value);
            else if (key == "audioMotionFreqHz") m_settings.audioMotionFreqHz = std::stof(value);
            else if (key == "audioMotionDurationMs") m_settings.audioMotionDurationMs = std::stof(value);
            else if (key == "audioMotionCooldownSec") m_settings.audioMotionCooldownSec = std::stof(value);
            else if (key == "audioAlarmEntryEnabled") m_settings.audioAlarmEntryEnabled = ParseBoolSetting(value);
            else if (key == "audioAlarmEntryFreqHz") m_settings.audioAlarmEntryFreqHz = std::stof(value);
            else if (key == "audioAlarmEntryDurMs") m_settings.audioAlarmEntryDurMs = std::stof(value);
            else if (key == "audioAlarmExitEnabled") m_settings.audioAlarmExitEnabled = ParseBoolSetting(value);
            else if (key == "audioAlarmExitFreqHz") m_settings.audioAlarmExitFreqHz = std::stof(value);
            else if (key == "audioAlarmExitDurMs") m_settings.audioAlarmExitDurMs = std::stof(value);
            else if (key == "audioLockAcquiredEnabled") m_settings.audioLockAcquiredEnabled = ParseBoolSetting(value);
            else if (key == "audioLockAcquiredFreqHz") m_settings.audioLockAcquiredFreqHz = std::stof(value);
            else if (key == "audioLockAcquiredDurMs") m_settings.audioLockAcquiredDurMs = std::stof(value);
            else if (key == "audioLockLostEnabled") m_settings.audioLockLostEnabled = ParseBoolSetting(value);
            else if (key == "audioLockLostFreqHz") m_settings.audioLockLostFreqHz = std::stof(value);
            else if (key == "audioLockLostDurMs") m_settings.audioLockLostDurMs = std::stof(value);

        } catch (...) {
            std::cerr << "[WARN] Ignoring invalid settings entry: " << key << std::endl;
        }
    }

    if (m_cameraAddress.empty()) {
        m_cameraAddress = "1";
    }
    syncColorEditorsFromSettings();
}

void Application::applyStandardPreset() {
    m_settings = MakeStandardSettings();
    syncColorEditorsFromSettings();
    syncSettingsToSharedState();
    savePersistedSettings();
    log(LogLevel::INFO, "Settings preset applied: Standard");
}

void Application::applyPresetPerformance() {
    m_settings = MakeStandardSettings();
    m_settings.request4KCamera = false;
    m_settings.enable4KZoom = false;
    m_settings.grayscaleInput = true;
    m_settings.detectionSkipFrames = 2;
    m_settings.lowLightEnhancement = false;
    m_settings.showTrails = false;
    syncColorEditorsFromSettings();
    syncSettingsToSharedState();
    savePersistedSettings();
    log(LogLevel::INFO, "Settings preset applied: Performance");
}

void Application::applyPresetBalanced() {
    m_settings = MakeStandardSettings();
    m_settings.request4KCamera = true;
    m_settings.enable4KZoom = true;
    m_settings.grayscaleInput = false;
    m_settings.detectionSkipFrames = 0;
    m_settings.lowLightEnhancement = false;
    m_settings.showTrails = true;
    m_settings.showStatusWindows = true;
    syncColorEditorsFromSettings();
    syncSettingsToSharedState();
    savePersistedSettings();
    log(LogLevel::INFO, "Settings preset applied: Balanced");
}

void Application::applyPresetPrecision() {
    m_settings = MakeStandardSettings();
    m_settings.request4KCamera = true;
    m_settings.enable4KZoom = true;
    m_settings.detectorConfThreshold = 0.10f;
    m_settings.detectorScoreThreshold = 0.10f;
    m_settings.detectorNmsThreshold = 0.35f;
    m_settings.detectionSkipFrames = 0;
    m_settings.lowLightEnhancement = true;
    m_settings.lowLightClipLimit = 3.5f;
    m_settings.lowLightDenoiseKernel = 3;
    m_settings.showTrails = true;
    syncColorEditorsFromSettings();
    syncSettingsToSharedState();
    savePersistedSettings();
    log(LogLevel::INFO, "Settings preset applied: Precision");
}

void Application::applyPresetLowLight() {
    m_settings = MakeStandardSettings();
    m_settings.request4KCamera = true;
    m_settings.enable4KZoom = true;
    m_settings.lowLightEnhancement = true;
    m_settings.lowLightClipLimit = 4.5f;
    m_settings.lowLightDenoiseKernel = 5;
    m_settings.grayscaleInput = false;
    m_settings.detectionSkipFrames = 0;
    m_settings.showTrails = true;
    syncColorEditorsFromSettings();
    syncSettingsToSharedState();
    savePersistedSettings();
    log(LogLevel::INFO, "Settings preset applied: Low Light");
}

// -----------------------------------------------------------------------
// Worker Thread
// -----------------------------------------------------------------------

void Application::captureWorkerLoop() {
    log(LogLevel::INFO, "Capture thread started");
    cv::Mat frame;
    while (m_running) {
        // --- Read current settings for camera resolution ---
        SystemSettings currentSettings;
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            currentSettings = m_sharedSettings;
        }

        // ── Camera resolution / mode change ────────────────────────────
        static bool lastRequest4K = currentSettings.request4KCamera;
        if (currentSettings.request4KCamera != lastRequest4K) {
            lastRequest4K = currentSettings.request4KCamera;
            log(LogLevel::INFO, "Camera resolution setting changed. Reopening camera...");
            m_cameraChangeRequested.store(true);
            {
                std::lock_guard<std::mutex> lk(m_cameraChangeMutex);
                m_pendingCameraAddress = m_cameraAddress;
            }
        }

        // ── Camera hot-swap ────────────────────────────────────────────
        if (m_cameraChangeRequested.load()) {
            std::string newAddr;
            {
                std::lock_guard<std::mutex> lk(m_cameraChangeMutex);
                newAddr = m_pendingCameraAddress;
            }
            m_cameraChangeRequested.store(false);

            int requestedW = currentSettings.request4KCamera ? 3840 : 1280;
            int requestedH = currentSettings.request4KCamera ? 2160 : 720;

            log(LogLevel::INFO, "Hot-swapping camera to: " + newAddr + " at resolution: " + std::to_string(requestedW) + "x" + std::to_string(requestedH));
            m_camera->close();  // release old
            bool ok = m_camera->open(newAddr, requestedW, requestedH);

            // Reset motion background model: old scene is now invalid
            m_motionDetector.reset();

            // Write result back (read from render thread for status display)
            {
                std::lock_guard<std::mutex> lk(m_cameraChangeMutex);
                m_cameraStatus   = ok ? "OK — " + newAddr : "FAILED — " + newAddr;
                m_cameraStatusOk = ok;
            }
            if (ok) {
                m_cameraAddress = newAddr;
                int actualW = m_camera->getWidth();
                int actualH = m_camera->getHeight();
                std::string backend = m_camera->getBackendName();
                log(LogLevel::INFO, "Camera opened: " + newAddr + " (backend=" + backend + ")");
                log(LogLevel::INFO, "Camera resolution requested=" + std::to_string(requestedW) + "x" + std::to_string(requestedH) +
                    ", actual=" + std::to_string(actualW) + "x" + std::to_string(actualH));
                if (actualW < requestedW || actualH < requestedH) {
                    log(LogLevel::WARN, "Camera did not accept requested 4K mode. Source likely limited to " +
                        std::to_string(actualW) + "x" + std::to_string(actualH));
                }
            } else {
                log(LogLevel::ERR, "Camera failed to open: " + newAddr);
            }
        }

        if (m_camera->read(frame)) {
            if (!frame.empty()) {
                std::lock_guard<std::mutex> lock(m_captureMutex);
                frame.copyTo(m_captureFrame);
                m_captureNewFrameVision = true;
                m_captureNewFrameDisplay = true;
                m_totalFramesProcessed.fetch_add(1);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    log(LogLevel::INFO, "Capture thread stopping");
}

void Application::workerLoop() {
    cv::Mat rawFrame;
    cv::Mat trackingFrame;
    // Persistent buffers for zero-heap low-light processing
    cv::Mat labFrame;
    std::vector<cv::Mat> labChannels;
    cv::Ptr<cv::CLAHE> clahe;
    cv::Mat zoomLab;
    std::vector<cv::Mat> zoomChannels;
    cv::Ptr<cv::CLAHE> zoomClahe;
    cv::Mat zoomCrop;
    float currentFps = 0.0f;
    auto lastTime = std::chrono::steady_clock::now();
    int frameSkipCounter = 0;

    while (m_running) {
        // --- Read current settings ---
        SystemSettings currentSettings;
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            currentSettings = m_sharedSettings;
        }
        SharedSubZoom localSubZooms[4];

        // ── Consume from Capture Thread ────────────────────────────────
        {
            std::lock_guard<std::mutex> lock(m_captureMutex);
            if (!m_captureNewFrameVision) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue; 
            }
            m_captureFrame.copyTo(rawFrame);
            m_captureNewFrameVision = false;
        }

        auto now = std::chrono::steady_clock::now();
        currentFps = 1.0f / std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // Keep the original camera frame for display and tracking so we do not
        // lose resolution or distort non-16:9 sources. The detector already
        // performs its own internal letterbox resize.
        rawFrame.copyTo(trackingFrame);

        // Apply low-light enhancement to tracking frame (HD) if enabled
        if (currentSettings.lowLightEnhancement && !trackingFrame.empty()) {
            ImageUtils::enhanceLowLight(trackingFrame, labFrame, labChannels, clahe, currentSettings.lowLightClipLimit, currentSettings.lowLightDenoiseKernel);
        }

        // ── Motion Detection (Plan 10) ──────────────────────────────────
        // Runs before object detection, entirely independent of YOLO/tracker.
        // Resets the background model on camera hot-swap.
        std::vector<cv::Rect> motionRegions;
        if (currentSettings.motionDetectionEnabled && !trackingFrame.empty()) {
            m_motionDetector.process(trackingFrame, currentSettings);
            motionRegions = m_motionDetector.getMotionRegions();
            
            if (currentSettings.motionHeatmapOverlay) {
                std::lock_guard<std::mutex> lock(m_dataMutex);
                m_motionDetector.getHeatmapImage().copyTo(m_sharedHeatmapFrame);
            }
        }

        // Update motion tracks and sub zooms in worker thread
        bool newMotionSpawned = false;
        if (currentSettings.motionDetectionEnabled) {
            auto now = std::chrono::steady_clock::now();
            
            // Clean up old inactive tracks
            for (auto it = m_workerMotionTracks.begin(); it != m_workerMotionTracks.end(); ) {
                float elapsed = std::chrono::duration<float>(now - it->lastSeen).count();
                if (elapsed > 2.0f) {
                    it = m_workerMotionTracks.erase(it);
                } else {
                    ++it;
                }
            }

            // Match new motionRegions to existing tracks
            std::vector<bool> regionMatched(motionRegions.size(), false);
            std::vector<bool> trackMatched(m_workerMotionTracks.size(), false);
            
            // Match based on center distance
            for (size_t i = 0; i < motionRegions.size(); ++i) {
                cv::Point2f regionCenter(
                    motionRegions[i].x + motionRegions[i].width / 2.0f,
                    motionRegions[i].y + motionRegions[i].height / 2.0f
                );
                
                int bestTrackIdx = -1;
                float bestDist = 200.0f; // Max center distance in pixels to associate
                
                for (size_t j = 0; j < m_workerMotionTracks.size(); ++j) {
                    if (trackMatched[j]) continue;
                    
                    cv::Point2f trackCenter(
                        m_workerMotionTracks[j].box.x + m_workerMotionTracks[j].box.width / 2.0f,
                        m_workerMotionTracks[j].box.y + m_workerMotionTracks[j].box.height / 2.0f
                    );
                    
                    float dist = cv::norm(regionCenter - trackCenter);
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestTrackIdx = static_cast<int>(j);
                    }
                }
                
                if (bestTrackIdx != -1) {
                    m_workerMotionTracks[bestTrackIdx].box = motionRegions[i];
                    m_workerMotionTracks[bestTrackIdx].lastSeen = now;
                    m_workerMotionTracks[bestTrackIdx].active = true;
                    regionMatched[i] = true;
                    trackMatched[bestTrackIdx] = true;
                }
            }
            
            // Add unmatched regions as new tracks
            for (size_t i = 0; i < motionRegions.size(); ++i) {
                if (regionMatched[i]) continue;
                
                // Only spawn if min area requirement is met
                if (motionRegions[i].area() >= currentSettings.motionMinArea) {
                    WorkerMotionTrack t;
                    t.id = m_nextMotionId++;
                    t.box = motionRegions[i];
                    t.lastSeen = now;
                    t.active = true;
                    m_workerMotionTracks.push_back(t);
                    trackMatched.push_back(false); // Fix out-of-bounds bug by keeping trackMatched size synchronized
                    newMotionSpawned = true;
                }
            }
            
            // Mark tracks not seen in this frame as inactive (but keep them for 2s hold)
            for (size_t j = 0; j < m_workerMotionTracks.size(); ++j) {
                if (!trackMatched[j]) {
                    m_workerMotionTracks[j].active = false;
                }
            }
            
            if (currentSettings.subZoomsEnabled) {
                // Plan 11 Extension: Filter motion tracks for uniqueness (Non-Maximum Suppression)
                // We want to avoid multiple slots showing the same or very close objects.
                std::vector<int> uniqueTrackIndices;
                for (size_t i = 0; i < m_workerMotionTracks.size(); ++i) {
                    bool tooClose = false;
                    for (int uniqueIdx : uniqueTrackIndices) {
                        cv::Rect intersect = m_workerMotionTracks[i].box & m_workerMotionTracks[uniqueIdx].box;
                        if (intersect.area() > 0) {
                            float iou = static_cast<float>(intersect.area()) / 
                                        (m_workerMotionTracks[i].box.area() + m_workerMotionTracks[uniqueIdx].box.area() - intersect.area());
                            // If IoU > 0.3, they are likely the same object
                            if (iou > 0.3f) {
                                tooClose = true;
                                break;
                            }
                        }
                    }
                    if (!tooClose) {
                        uniqueTrackIndices.push_back(static_cast<int>(i));
                    }
                }

                // Assign tracks to the 4 slots of localSubZooms
                std::vector<bool> slotUpdated(4, false);
                
                // Keep tracks that are already assigned AND still considered unique
                for (int i = 0; i < 4; ++i) {
                    if (m_sharedSubZooms[i].active) {
                        int assignedId = m_sharedSubZooms[i].motion_id;
                        auto trackIt = std::find_if(m_workerMotionTracks.begin(), m_workerMotionTracks.end(),
                            [assignedId](const WorkerMotionTrack& t) { return t.id == assignedId; });
                        
                        // Check if this track is still in our unique list
                        bool isStillUnique = false;
                        if (trackIt != m_workerMotionTracks.end()) {
                            int trackIdx = static_cast<int>(std::distance(m_workerMotionTracks.begin(), trackIt));
                            if (std::find(uniqueTrackIndices.begin(), uniqueTrackIndices.end(), trackIdx) != uniqueTrackIndices.end()) {
                                isStillUnique = true;
                            }
                        }

                        if (trackIt != m_workerMotionTracks.end() && isStillUnique) {
                            localSubZooms[i].active = true;
                            localSubZooms[i].motion_id = trackIt->id;
                            localSubZooms[i].box = trackIt->box;
                            localSubZooms[i].isLost = !trackIt->active;
                            
                            // Crop the frame with padding and magnification
                            int pad = currentSettings.subZoomPaddingPx;
                            float mag = std::max(1.0f, currentSettings.subZoomMagnification);
                            
                            int cx = trackIt->box.x + trackIt->box.width / 2;
                            int cy = trackIt->box.y + trackIt->box.height / 2;
                            
                            int paddedW = trackIt->box.width + 2 * pad;
                            int paddedH = trackIt->box.height + 2 * pad;
                            
                            int cropW = std::max(8, static_cast<int>(std::round(paddedW / mag)));
                            int cropH = std::max(8, static_cast<int>(std::round(paddedH / mag)));
                            
                            int x1 = cx - cropW / 2;
                            int y1 = cy - cropH / 2;
                            
                            int x1_clamped = std::max(0, x1);
                            int y1_clamped = std::max(0, y1);
                            int x2_clamped = std::min(trackingFrame.cols, x1 + cropW);
                            int y2_clamped = std::min(trackingFrame.rows, y1 + cropH);
                            
                            if (x2_clamped > x1_clamped && y2_clamped > y1_clamped) {
                                cv::Rect roi(x1_clamped, y1_clamped, x2_clamped - x1_clamped, y2_clamped - y1_clamped);
                                trackingFrame(roi).copyTo(localSubZooms[i].frame);
                            }
                            
                            slotUpdated[i] = true;
                        }
                    }
                }
                
                // Fill empty slots with unassigned unique tracks
                for (int trackIdx : uniqueTrackIndices) {
                    const auto& track = m_workerMotionTracks[trackIdx];
                    bool alreadyAssigned = false;
                    for (int i = 0; i < 4; ++i) {
                        if (slotUpdated[i] && localSubZooms[i].motion_id == track.id) {
                            alreadyAssigned = true;
                            break;
                        }
                    }
                    
                    if (alreadyAssigned) continue;
                    
                    // Find first free slot
                    int freeSlotIdx = -1;
                    for (int i = 0; i < 4; ++i) {
                        if (!slotUpdated[i] && !localSubZooms[i].active) {
                            freeSlotIdx = i;
                            break;
                        }
                    }
                    
                    if (freeSlotIdx != -1) {
                        localSubZooms[freeSlotIdx].active = true;
                        localSubZooms[freeSlotIdx].motion_id = track.id;
                        localSubZooms[freeSlotIdx].box = track.box;
                        localSubZooms[freeSlotIdx].isLost = !track.active;
                        
                        // Crop
                        int pad = currentSettings.subZoomPaddingPx;
                        float mag = std::max(1.0f, currentSettings.subZoomMagnification);
                        
                        int cx = track.box.x + track.box.width / 2;
                        int cy = track.box.y + track.box.height / 2;
                        
                        int paddedW = track.box.width + 2 * pad;
                        int paddedH = track.box.height + 2 * pad;
                        
                        int cropW = std::max(8, static_cast<int>(std::round(paddedW / mag)));
                        int cropH = std::max(8, static_cast<int>(std::round(paddedH / mag)));
                        
                        int x1 = cx - cropW / 2;
                        int y1 = cy - cropH / 2;
                        
                        int x1_clamped = std::max(0, x1);
                        int y1_clamped = std::max(0, y1);
                        int x2_clamped = std::min(trackingFrame.cols, x1 + cropW);
                        int y2_clamped = std::min(trackingFrame.rows, y1 + cropH);
                        
                        if (x2_clamped > x1_clamped && y2_clamped > y1_clamped) {
                            cv::Rect roi(x1_clamped, y1_clamped, x2_clamped - x1_clamped, y2_clamped - y1_clamped);
                            trackingFrame(roi).copyTo(localSubZooms[freeSlotIdx].frame);
                        }
                        
                        slotUpdated[freeSlotIdx] = true;
                    }
                }
            }
        } else {
            m_workerMotionTracks.clear();
        }
        
        // ── Audio: motion alert (rate-limited by cooldown, triggered by new moving entities) ──
        if (currentSettings.motionDetectionEnabled && newMotionSpawned && m_audioEngine.motionCooldownElapsed()) {
            m_audioEngine.playMotionAlert();
            m_audioEngine.recordMotionBeep();
        }

        std::vector<Detection> detections;
        bool hasNewDetections = false;
        int detectorLag = 0;
        
        static bool s_pendingDetections = false;
        static uint64_t s_detectionTriggerFrame = 0;

        if (m_detectorNewResults.load()) {
            std::lock_guard<std::mutex> lock(m_detectorMutex);
            m_detections = m_detectorResults;
            m_detectorNewResults = false;
            s_pendingDetections = true;
            s_detectionTriggerFrame = m_detectorTriggerFrame.load();
        }
        detections = m_detections; // Use latest available detections for HUD rendering

        // ── Tracking & Logging Trigger ──────────────────────────────────
        // Move processing-heavy tracking and disk-I/O logging to dedicated thread
        if (!m_trackingBusy.load()) {
            uint64_t nowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            
            if (s_pendingDetections) {
                hasNewDetections = true;
                detectorLag = static_cast<int>(m_totalFramesProcessed.load() - s_detectionTriggerFrame);
                s_pendingDetections = false;
            }

            std::lock_guard<std::mutex> lock(m_trackingMutex);
            trackingFrame.copyTo(m_trackingFrameCopy);
            m_trackingSettingsCopy   = currentSettings;
            m_trackingDetectionsCopy   = hasNewDetections ? detections : std::vector<Detection>();
            m_trackingDetectorLag      = detectorLag;
            m_trackingSessionMs        = nowMs - m_logSessionStartMs;
            m_trackingMotionTracksCopy = m_workerMotionTracks;
            m_trackingBusy             = true;
            m_trackingCv.notify_one();
        }

        // ── Data-Logging Session Management (Plan 03) ─────────────────────
        if (currentSettings.dataLoggingEnabled && !m_dataLogger->isOpen()) {
            LogFormat fmt = (currentSettings.dataLoggingFormat == 1) ? LogFormat::JSON : LogFormat::CSV;
            if (m_dataLogger->open(currentSettings.dataLoggingOutputDir, fmt)) {
                log(LogLevel::INFO, "Data logging started: " + m_dataLogger->getCurrentPath());
            }
            m_logSessionStartMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        } else if (!currentSettings.dataLoggingEnabled && m_dataLogger->isOpen()) {
            m_dataLogger->close();
            log(LogLevel::INFO, "Data logging stopped.");
        }

        if (currentSettings.enableDetection && !m_detectorBusy.load()) {
            bool runDetector = (frameSkipCounter == 0);
            frameSkipCounter = (frameSkipCounter + 1) % (currentSettings.detectionSkipFrames + 1);

            if (runDetector) {
                std::lock_guard<std::mutex> lock(m_detectorMutex);
                trackingFrame.copyTo(m_detectorFrameCopy);
                m_detectorSettingsCopy = currentSettings;
                m_detectorTriggerFrame = m_totalFramesProcessed.load();
                m_detectorBusy = true;
                m_detectorCv.notify_one();
            }
        }

        // Crop target zoom from rawFrame (or trackingFrame if disabled)
        if (m_sharedLockedTarget.state != TrackingState::SEARCHING && !rawFrame.empty()) {
            cv::Rect roi = m_sharedLockedTarget.box;
            
            // Validate bounding box in trackingFrame space (1280x720)
            const bool bbox_valid = (roi.width > 0 && roi.height > 0 &&
                                      roi.x >= 0 && roi.y >= 0 &&
                                      roi.x + roi.width  <= trackingFrame.cols &&
                                      roi.y + roi.height <= trackingFrame.rows);
            if (bbox_valid) {
                cv::Mat sourceFrame = rawFrame;
                cv::Rect targetRoi = roi;
                
                if (currentSettings.enable4KZoom) {
                    double scaleX = (double)rawFrame.cols / (double)trackingFrame.cols;
                    double scaleY = (double)rawFrame.rows / (double)trackingFrame.rows;
                    
                    targetRoi.x = static_cast<int>(std::round(roi.x * scaleX));
                    targetRoi.y = static_cast<int>(std::round(roi.y * scaleY));
                    targetRoi.width = static_cast<int>(std::round(roi.width * scaleX));
                    targetRoi.height = static_cast<int>(std::round(roi.height * scaleY));
                } else {
                    sourceFrame = trackingFrame;
                }

                // Apply additional digital magnification by shrinking the crop
                // around the target center (values >1.0 zoom in stronger).
                float zoomMag = std::max(1.0f, currentSettings.targetZoomMagnification);
                if (zoomMag > 1.0f) {
                    int centerX = targetRoi.x + targetRoi.width / 2;
                    int centerY = targetRoi.y + targetRoi.height / 2;

                    int zoomW = static_cast<int>(std::round(targetRoi.width / zoomMag));
                    int zoomH = static_cast<int>(std::round(targetRoi.height / zoomMag));

                    zoomW = std::max(8, zoomW);
                    zoomH = std::max(8, zoomH);

                    targetRoi.x = centerX - zoomW / 2;
                    targetRoi.y = centerY - zoomH / 2;
                    targetRoi.width = zoomW;
                    targetRoi.height = zoomH;
                }
                
                // Add 15% padding around the target bounding box
                int pad_w = static_cast<int>(targetRoi.width * 0.15f);
                int pad_h = static_cast<int>(targetRoi.height * 0.15f);
                int x1 = std::max(0, targetRoi.x - pad_w);
                int y1 = std::max(0, targetRoi.y - pad_h);
                int x2 = std::min(sourceFrame.cols, targetRoi.x + targetRoi.width + pad_w);
                int y2 = std::min(sourceFrame.rows, targetRoi.y + targetRoi.height + pad_h);
                
                    if (x2 > x1 && y2 > y1) {
                        sourceFrame(cv::Rect(x1, y1, x2 - x1, y2 - y1)).copyTo(zoomCrop);
                        // If using 4K zoom, apply enhancement to the zoom crop separately
                        if (currentSettings.enable4KZoom && currentSettings.lowLightEnhancement && !zoomCrop.empty()) {
                            ImageUtils::enhanceLowLight(zoomCrop, zoomLab, zoomChannels, zoomClahe, currentSettings.lowLightClipLimit, currentSettings.lowLightDenoiseKernel);
                        }
                    } else {
                        zoomCrop.release();
                    }
            } else {
                zoomCrop.release();
            }
        } else {
            zoomCrop.release();
        }

        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            trackingFrame.copyTo(m_sharedFrame);
            zoomCrop.copyTo(m_sharedZoomFrame);
            m_sharedDetections       = detections;
            // m_sharedTrackedObjects is now updated by trackingWorkerLoop
            m_sharedMotionRegions    = motionRegions;
            m_sharedCameraFps        = currentFps;
            m_sharedCameraWidth      = rawFrame.cols;
            m_sharedCameraHeight     = rawFrame.rows;
            m_sharedTrackingWidth    = trackingFrame.cols;
            m_sharedTrackingHeight   = trackingFrame.rows;
            m_sharedZoomWidth        = zoomCrop.cols;
            m_sharedZoomHeight       = zoomCrop.rows;
            
            // Copy sub zoom data to shared state
            for (int i = 0; i < 4; ++i) {
                m_sharedSubZooms[i].active = localSubZooms[i].active;
                m_sharedSubZooms[i].motion_id = localSubZooms[i].motion_id;
                m_sharedSubZooms[i].box = localSubZooms[i].box;
                m_sharedSubZooms[i].isLost = localSubZooms[i].isLost;
                localSubZooms[i].frame.copyTo(m_sharedSubZooms[i].frame);
            }
            
            m_newDataAvailable       = true;
        }
    }
}

void Application::trackingWorkerLoop() {
    log(LogLevel::INFO, "Tracking thread started");
    while (m_running) {
        cv::Mat trackingFrame;
        SystemSettings settings;
        std::vector<Detection> detections;
        int detectorLag = 0;
        uint64_t sessionMs = 0;
        std::vector<WorkerMotionTrack> motionTracks;

        {
            std::unique_lock<std::mutex> lock(m_trackingMutex);
            m_trackingCv.wait(lock, [this]() {
                return !m_running || m_trackingBusy.load();
            });

            if (!m_running) break;

            m_trackingFrameCopy.copyTo(trackingFrame);
            settings     = m_trackingSettingsCopy;
            detections   = m_trackingDetectionsCopy;
            detectorLag  = m_trackingDetectorLag;
            sessionMs    = m_trackingSessionMs;
            motionTracks = m_trackingMotionTracksCopy;
        }

        if (trackingFrame.empty()) {
            m_trackingBusy = false;
            continue;
        }

        // --- Tracking Logic (moved from workerLoop) ---
        std::vector<TrackedObject> tracked;
        try {
            // --- 1. Handle External Lock Commands ---
            if (m_lockRequested.exchange(false)) {
                m_sharedLockedTarget.track_id = m_requestedLockId.load();
                m_sharedLockedTarget.state = TrackingState::LOCKED;
                m_pixelLockActive = false;
            }
            if (m_releaseLockRequested.exchange(false)) {
                m_sharedLockedTarget.state = TrackingState::SEARCHING;
                m_sharedLockedTarget.track_id = -1;
                m_pixelLockActive = false;
            }
            if (m_pixelLockRequested.exchange(false)) {
                cv::Rect tr; { std::lock_guard<std::mutex> lock(m_dataMutex); tr = m_pixelLockRect; }
                
                // Clamp to frame boundaries
                tr.x = std::max(0, tr.x);
                tr.y = std::max(0, tr.y);
                if (tr.x + tr.width > trackingFrame.cols) tr.width = trackingFrame.cols - tr.x;
                if (tr.y + tr.height > trackingFrame.rows) tr.height = trackingFrame.rows - tr.y;

                if (tr.width > 4 && tr.height > 4) {
                    m_pixelTemplate = trackingFrame(tr).clone();
                    m_pixelLockActive = true; m_pixelVx = 0.0f; m_pixelVy = 0.0f;
                    m_pixelCenterX = static_cast<float>(tr.x + tr.width / 2.0f);
                    m_pixelCenterY = static_cast<float>(tr.y + tr.height / 2.0f);
                    m_pixelLockRect = tr;
                    m_sharedLockedTarget.state = TrackingState::LOCKED;
                    m_sharedLockedTarget.track_id = m_tracker->getNextIdAndIncrement();
                    m_sharedLockedTarget.class_id = -1; m_sharedLockedTarget.className = "Pixel Target";
                    m_sharedLockedTarget.box = tr; m_sharedLockedTarget.confidence = 1.0f;
                    m_sharedLockedTarget.lost_frames = 0; m_sharedLockedTarget.trail.clear();
                    m_sharedLockedTarget.trail.push_back(cv::Point(static_cast<int>(m_pixelCenterX), static_cast<int>(m_pixelCenterY)));
                    
                    // Trigger sound effect
                    m_audioEngine.playLockAcquired();
                    log(LogLevel::INFO, "Pixel target locked via selection: (" + 
                        std::to_string(tr.x) + ", " + std::to_string(tr.y) + ", " + 
                        std::to_string(tr.width) + "x" + std::to_string(tr.height) + ")");
                }
            }
            if (m_pixelLockRectUpdateRequested.exchange(false)) {
                cv::Rect rect; { std::lock_guard<std::mutex> lock(m_dataMutex); rect = m_pixelLockRect; }
                rect.x = std::max(0, rect.x); rect.y = std::max(0, rect.y);
                if (rect.x + rect.width > trackingFrame.cols) rect.width = trackingFrame.cols - rect.x;
                if (rect.y + rect.height > trackingFrame.rows) rect.height = trackingFrame.rows - rect.y;
                if (rect.width > 4 && rect.height > 4) {
                    m_pixelTemplate = trackingFrame(rect).clone();
                    m_pixelLockActive = true; m_pixelVx = 0.0f; m_pixelVy = 0.0f;
                    m_pixelCenterX = static_cast<float>(rect.x + rect.width / 2.0f);
                    m_pixelCenterY = static_cast<float>(rect.y + rect.height / 2.0f);
                    m_pixelLockRect = rect;
                    m_sharedLockedTarget.state = TrackingState::LOCKED;
                    if (m_sharedLockedTarget.track_id < 0) m_sharedLockedTarget.track_id = m_tracker->getNextIdAndIncrement();
                    m_sharedLockedTarget.box = rect; m_sharedLockedTarget.confidence = 1.0f; m_sharedLockedTarget.lost_frames = 0;
                    
                    // Clear trail on manual move/resize to avoid "jump" lines
                    m_sharedLockedTarget.trail.clear();
                    m_sharedLockedTarget.trail.push_back(cv::Point(static_cast<int>(m_pixelCenterX), static_cast<int>(m_pixelCenterY)));
                }
            }

            if (settings.enableTracking) {
                // --- Plan 11: High-Sensitivity Re-acquisition ---
                if (settings.trackerReacquisitionEnabled || settings.trackerUseMotionFallback) {
                    std::vector<TrackedObject> currentTracks = m_tracker->getTrackedObjects(1);
                    for (const auto& track : currentTracks) {
                        if (track.lost_frames > 0 && track.lost_frames < settings.trackerMaxLostFrames) {
                            bool foundInROI = false;

                            // 1. ROI-based Re-Detection
                            if (settings.trackerReacquisitionEnabled) {
                                float zoom = std::max(1.1f, settings.trackerReacquisitionZoom);
                                int roiW = static_cast<int>(track.box.width * zoom);
                                int roiH = static_cast<int>(track.box.height * zoom);
                                int roiX = track.box.x + track.box.width / 2 - roiW / 2;
                                int roiY = track.box.y + track.box.height / 2 - roiH / 2;

                                // Clamp to frame bounds
                                int x1 = std::max(0, roiX);
                                int y1 = std::max(0, roiY);
                                int x2 = std::min(trackingFrame.cols, roiX + roiW);
                                int y2 = std::min(trackingFrame.rows, roiY + roiH);

                                if (x2 - x1 > 10 && y2 - y1 > 10) {
                                    cv::Rect roi(x1, y1, x2 - x1, y2 - y1);
                                    cv::Mat roiFrame = trackingFrame(roi).clone();
                                    
                                    // Run detector on ROI
                                    std::vector<Detection> roiDets = m_detector->detect(roiFrame, settings);
                                    
                                    // Safeguard: Only take the best matching detection in ROI
                                    float bestRoiConf = 0.0f;
                                    Detection bestRoiDet;
                                    bool bestRoiFound = false;

                                    for (auto& d : roiDets) {
                                        if (d.class_id == track.class_id && d.confidence > bestRoiConf) {
                                            bestRoiConf = d.confidence;
                                            bestRoiDet = d;
                                            bestRoiFound = true;
                                        }
                                    }

                                    if (bestRoiFound) {
                                        // Map back to global
                                        bestRoiDet.box.x += x1;
                                        bestRoiDet.box.y += y1;
                                        bestRoiDet.is_recovery = true; // Mark as recovery to prevent spawning new tracks
                                        
                                        detections.push_back(bestRoiDet);
                                        foundInROI = true;
                                    }
                                }
                            }

                            // 2. Motion-Guided Fallback (only if YOLO failed in ROI or is disabled)
                            if (!foundInROI && settings.trackerUseMotionFallback) {
                                for (const auto& mt : motionTracks) {
                                    // Check overlap with predicted box
                                    cv::Rect intersect = track.box & mt.box;
                                    if (intersect.area() > 0) {
                                        float iou = static_cast<float>(intersect.area()) / 
                                                    (track.box.area() + mt.box.area() - intersect.area());
                                        
                                        if (iou > 0.15f || intersect.area() > track.box.area() * 0.5f) {
                                            // Pseudo-detection from motion
                                            Detection d;
                                            d.class_id = track.class_id;
                                            d.className = track.className + " (Motion)";
                                            d.confidence = 0.5f; // Lower confidence for motion fallback
                                            d.box = mt.box;
                                            d.is_recovery = true; // Mark as recovery
                                            detections.push_back(d);
                                            break; 
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // --- Plan 11 Safeguard: Global Deduplication NMS ---
                // If we have mixed real-time (recovery) and lagged (global) detections,
                // the lagged ones might spawn ghost tracks. We perform NMS to suppress them.
                if (!detections.empty()) {
                    std::vector<cv::Rect> b;
                    std::vector<float> c;
                    for (const auto& d : detections) {
                        b.push_back(d.box);
                        c.push_back(d.confidence);
                    }
                    std::vector<int> idx;
                    // Strict NMS to ensure only one box per physical object
                    cv::dnn::NMSBoxes(b, c, 0.05f, 0.3f, idx);
                    
                    std::vector<Detection> uniqueDets;
                    for (int i : idx) uniqueDets.push_back(detections[i]);
                    detections = std::move(uniqueDets);
                }

                // Update tracker
                m_tracker->update(detections, settings, detectorLag);
                
                tracked = m_tracker->getTrackedObjects(settings.trackerMaxTrailLength);

                // --- Face Recognition Integration (Plan 11) ---
                if (settings.faceRecognitionEnabled && m_faceRecognizer) {
                    for (auto& obj : tracked) {
                        // COCO class 0 = person
                        if (obj.class_id == 0 && obj.is_confirmed) {
                            bool runRecognition = false;
                            
                            {
                                std::lock_guard<std::mutex> lock(m_faceMutex);
                                if (m_trackIdToFace.find(obj.track_id) == m_trackIdToFace.end()) {
                                    runRecognition = true;
                                } else {
                                    // Periodic re-check to ensure consistency (every 60 frames)
                                    if (m_totalFramesProcessed.load() % 60 == 0) {
                                        runRecognition = true;
                                    }
                                }
                            }

                            if (runRecognition) {
                                auto faceResults = m_faceRecognizer->process(trackingFrame, obj.box, settings.faceRecognitionThreshold);
                                if (!faceResults.empty()) {
                                    // Take the best match (first result for now)
                                    const auto& fr = faceResults[0];
                                    if (fr.identityId != -1) {
                                        std::lock_guard<std::mutex> lock(m_faceMutex);
                                        m_trackIdToFace[obj.track_id] = {fr.identityId, fr.name};
                                    }
                                }
                            }

                            // Apply cached identity
                            {
                                std::lock_guard<std::mutex> lock(m_faceMutex);
                                auto it = m_trackIdToFace.find(obj.track_id);
                                if (it != m_trackIdToFace.end()) {
                                    obj.face_id = it->second.first;
                                    obj.face_name = it->second.second;
                                }
                            }
                        }
                    }

                    // Clean up cache for dead tracks
                    {
                        std::lock_guard<std::mutex> lock(m_faceMutex);
                        for (auto it = m_trackIdToFace.begin(); it != m_trackIdToFace.end(); ) {
                            bool stillExists = false;
                            for (const auto& obj : tracked) {
                                if (obj.track_id == it->first) { stillExists = true; break; }
                            }
                            if (!stillExists) it = m_trackIdToFace.erase(it);
                            else ++it;
                        }
                    }
                }

                updateTargetHistory(tracked, trackingFrame);

                // Update pixel target using template matching if active
                if (m_pixelLockActive && m_sharedLockedTarget.state != TrackingState::SEARCHING) {
                    if (!m_pixelLockDragging.load()) {
                        cv::Rect lastBox = m_sharedLockedTarget.box;

                        // Predict next position using constant velocity motion model
                        m_pixelCenterX += m_pixelVx;
                        m_pixelCenterY += m_pixelVy;

                        cv::Rect predictedBox(
                            static_cast<int>(std::round(m_pixelCenterX - lastBox.width / 2.0f)),
                            static_cast<int>(std::round(m_pixelCenterY - lastBox.height / 2.0f)),
                            lastBox.width,
                            lastBox.height
                        );

                        int pad = 80; // Larger search window to prevent tracking loss
                        int rectW = predictedBox.width + pad * 2;
                        int rectH = predictedBox.height + pad * 2;
                        int rectX = predictedBox.x - pad;
                        int rectY = predictedBox.y - pad;

                        if (rectW > trackingFrame.cols) rectW = trackingFrame.cols;
                        if (rectH > trackingFrame.rows) rectH = trackingFrame.rows;

                        if (rectX < 0) rectX = 0;
                        if (rectY < 0) rectY = 0;
                        if (rectX + rectW > trackingFrame.cols) rectX = trackingFrame.cols - rectW;
                        if (rectY + rectH > trackingFrame.rows) rectY = trackingFrame.rows - rectH;

                        cv::Rect searchRect(rectX, rectY, rectW, rectH);

                        if (searchRect.width >= m_pixelTemplate.cols && searchRect.height >= m_pixelTemplate.rows) {
                            cv::Mat result;
                            cv::matchTemplate(trackingFrame(searchRect), m_pixelTemplate, result, cv::TM_CCOEFF_NORMED);

                            double minVal, maxVal;
                            cv::Point minLoc, maxLoc;
                            cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

                            if (maxVal > 0.5) {
                                // Sub-pixel peak interpolation (quadratic fit)
                                double dx = 0.0, dy = 0.0;
                                if (maxLoc.x > 0 && maxLoc.x < result.cols - 1) {
                                    float valL = result.at<float>(maxLoc.y, maxLoc.x - 1);
                                    float valR = result.at<float>(maxLoc.y, maxLoc.x + 1);
                                    float valC = static_cast<float>(maxVal);
                                    float denom = valL - 2.0f * valC + valR;
                                    if (std::abs(denom) > 1e-5f) dx = static_cast<double>((valL - valR) / (2.0f * denom));
                                }
                                if (maxLoc.y > 0 && maxLoc.y < result.rows - 1) {
                                    float valT = result.at<float>(maxLoc.y - 1, maxLoc.x);
                                    float valB = result.at<float>(maxLoc.y + 1, maxLoc.x);
                                    float valC = static_cast<float>(maxVal);
                                    float denom = valT - 2.0f * valC + valB;
                                    if (std::abs(denom) > 1e-5f) dy = static_cast<double>((valT - valB) / (2.0f * denom));
                                }

                                double new_center_x = searchRect.x + maxLoc.x + dx + m_pixelTemplate.cols / 2.0;
                                double new_center_y = searchRect.y + maxLoc.y + dy + m_pixelTemplate.rows / 2.0;

                                float alpha = settings.trackerVelocitySmoothing;
                                m_pixelVx = alpha * static_cast<float>(new_center_x - (lastBox.x + lastBox.width / 2.0)) + (1.0f - alpha) * m_pixelVx;
                                m_pixelVy = alpha * static_cast<float>(new_center_y - (lastBox.y + lastBox.height / 2.0)) + (1.0f - alpha) * m_pixelVy;

                                m_pixelCenterX = static_cast<float>(new_center_x);
                                m_pixelCenterY = static_cast<float>(new_center_y);

                                cv::Rect newBox(
                                    static_cast<int>(std::round(m_pixelCenterX - m_pixelTemplate.cols / 2.0f)),
                                    static_cast<int>(std::round(m_pixelCenterY - m_pixelTemplate.rows / 2.0f)),
                                    m_pixelTemplate.cols,
                                    m_pixelTemplate.rows
                                );

                                m_sharedLockedTarget.box = newBox;
                                m_sharedLockedTarget.confidence = static_cast<float>(maxVal);
                                m_sharedLockedTarget.lost_frames = 0;
                                m_sharedLockedTarget.state = TrackingState::LOCKED;

                                cv::Point newCenter(newBox.x + newBox.width / 2, newBox.y + newBox.height / 2);
                                m_sharedLockedTarget.trail.push_back(newCenter);
                                if (m_sharedLockedTarget.trail.size() > static_cast<size_t>(settings.trackerMaxTrailLength)) {
                                    m_sharedLockedTarget.trail.erase(m_sharedLockedTarget.trail.begin());
                                }

                                if (maxVal > 0.85) {
                                    cv::Mat matchedPatch = trackingFrame(newBox);
                                    if (matchedPatch.size() == m_pixelTemplate.size() && matchedPatch.type() == m_pixelTemplate.type()) {
                                        double beta = 0.05;
                                        cv::addWeighted(m_pixelTemplate, 1.0 - beta, matchedPatch, beta, 0.0, m_pixelTemplate);
                                    }
                                }
                            } else {
                                m_sharedLockedTarget.lost_frames++;
                                m_pixelVx *= settings.trackerDeadReckoningDamping;
                                m_pixelVy *= settings.trackerDeadReckoningDamping;
                                m_sharedLockedTarget.box = predictedBox;
                                if (m_sharedLockedTarget.lost_frames > settings.trackerMaxLostFrames) {
                                    m_sharedLockedTarget.state = TrackingState::LOST;
                                    m_pixelLockActive = false;
                                } else {
                                    m_sharedLockedTarget.state = TrackingState::LOST;
                                }
                            }
                        }
                    }
                }

                // Update target lock states for non-pixel objects if pixelLock is inactive
                if (!m_pixelLockActive && m_sharedLockedTarget.state != TrackingState::SEARCHING) {
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

                // ── Audio: target lock state transitions ───────────────────────────
                {
                    const TrackingState curState = m_sharedLockedTarget.state;
                    if (m_prevLockState != TrackingState::LOCKED && curState == TrackingState::LOCKED) {
                        m_audioEngine.playLockAcquired();
                    } else if (m_prevLockState == TrackingState::LOCKED && curState == TrackingState::LOST) {
                        m_audioEngine.playLockLost();
                    }
                    m_prevLockState = curState;
                }

                if (m_pixelLockActive && m_sharedLockedTarget.state != TrackingState::SEARCHING) {
                    TrackedObject pixelObj;
                    pixelObj.track_id = m_sharedLockedTarget.track_id;
                    pixelObj.class_id = -1;
                    pixelObj.className = m_sharedLockedTarget.className;
                    pixelObj.box = m_sharedLockedTarget.box;
                    pixelObj.confidence = m_sharedLockedTarget.confidence;
                    pixelObj.lost_frames = m_sharedLockedTarget.lost_frames;
                    pixelObj.is_active = (m_sharedLockedTarget.state == TrackingState::LOCKED);
                    pixelObj.is_confirmed = true;
                    pixelObj.trail = m_sharedLockedTarget.trail;
                    tracked.push_back(pixelObj);
                }

                m_workerTrackCount.store(static_cast<int>(tracked.size()));

                // Alarm Zone Checks
                auto zones = m_roiManager->getROIs();
                std::unordered_set<int> activeZoneIds;
                for (const auto& z : zones) {
                    if (z.active && z.function == ROIFunction::ALARM) {
                        activeZoneIds.insert(z.id);
                        std::unordered_set<int> currentTracksInZone;
                        for (const auto& obj : tracked) {
                            cv::Point center(obj.box.x + obj.box.width / 2, obj.box.y + obj.box.height / 2);
                            if (z.rect.contains(center)) currentTracksInZone.insert(obj.track_id);
                        }
                        for (int tid : currentTracksInZone) {
                            if (m_activeAlarms[z.id].find(tid) == m_activeAlarms[z.id].end()) {
                                log(LogLevel::WARN, "ALARM: Object #" + std::to_string(tid) + " entered Alarm Zone '" + z.label + "'");
                                m_audioEngine.playAlarmEntry();
                            }
                        }
                        for (int tid : m_activeAlarms[z.id]) {
                            if (currentTracksInZone.find(tid) == currentTracksInZone.end()) {
                                log(LogLevel::INFO, "Object #" + std::to_string(tid) + " left Alarm Zone '" + z.label + "'");
                                m_audioEngine.playAlarmExit();
                            }
                        }
                        m_activeAlarms[z.id] = currentTracksInZone;
                    }
                }
                for (auto it = m_activeAlarms.begin(); it != m_activeAlarms.end(); ) {
                    if (activeZoneIds.find(it->first) == activeZoneIds.end()) it = m_activeAlarms.erase(it);
                    else ++it;
                }

                // ── Data-Logging (Plan 03) ────────────────────────────────────
                if (settings.dataLoggingEnabled && m_dataLogger->isOpen()) {
                    std::vector<TrackedObject> logObjects = tracked;
                    for (const auto& mtrack : motionTracks) {
                        bool overlaps = false;
                        for (const auto& obj : tracked) {
                            if (!obj.is_active || !obj.is_confirmed) continue;
                            cv::Rect inter = mtrack.box & obj.box;
                            if (inter.area() > 0) {
                                double ratio = (double)inter.area() / mtrack.box.area();
                                if (ratio > 0.2) { overlaps = true; break; }
                            }
                        }
                        if (!overlaps) {
                            TrackedObject motionObj;
                            motionObj.track_id = mtrack.id + 10000;
                            motionObj.class_id = -99;
                            motionObj.className = "Motion";
                            motionObj.box = mtrack.box;
                            motionObj.confidence = 1.0f;
                            motionObj.is_active = true;
                            motionObj.is_confirmed = true;
                            logObjects.push_back(motionObj);
                        }
                    }
                    m_dataLogger->logFrame(static_cast<double>(sessionMs), logObjects, 0.0);
                }
            }
        } catch (const std::exception& e) {
            log(LogLevel::ERR, std::string("Async tracking failed: ") + e.what());
        }

        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            m_sharedTrackedObjects = tracked;
            m_sharedTargetHistory = m_internalTargetHistory;
        }

        m_trackingBusy = false;
    }
    log(LogLevel::INFO, "Tracking thread stopping");
}

void Application::detectorWorkerLoop() {
    log(LogLevel::INFO, "Detector thread started");
    while (m_running) {
        cv::Mat frameToProcess;
        SystemSettings settingsToUse;
        
        {
            std::unique_lock<std::mutex> lock(m_detectorMutex);
            m_detectorCv.wait(lock, [this]() {
                return !m_running || m_detectorBusy.load();
            });
            
            if (!m_running) break;
            
            m_detectorFrameCopy.copyTo(frameToProcess);
            settingsToUse = m_detectorSettingsCopy;
        }
        
        if (frameToProcess.empty()) {
            m_detectorBusy = false;
            continue;
        }
        
        std::vector<Detection> results;
        try {
            cv::Mat inputFrame = frameToProcess;
            if (settingsToUse.grayscaleInput) {
                cv::Mat gray;
                cv::cvtColor(frameToProcess, gray, cv::COLOR_BGR2GRAY);
                cv::cvtColor(gray, inputFrame, cv::COLOR_GRAY2BGR);
            }
            results = m_detector->detect(inputFrame, settingsToUse);
            
            if (m_roiManager->hasActiveROI()) {
                m_roiManager->filterDetections(results);
            }
            
            m_workerDetectionCount.store(static_cast<int>(results.size()));
        } catch (const std::exception& e) {
            log(LogLevel::ERR, std::string("Async detection failed: ") + e.what());
        }
        
        {
            std::lock_guard<std::mutex> lock(m_detectorMutex);
            m_detectorResults = std::move(results);
            m_detectorNewResults = true;
            m_detectorBusy = false;
        }
    }
    log(LogLevel::INFO, "Detector thread stopping");
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

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##DataPanelFilter", "Filter by ID, class, or state", m_dataPanelFilter, sizeof(m_dataPanelFilter));
    ImGui::SameLine();
    if (ImGui::Button("Clear##DataPanelFilter")) {
        m_dataPanelFilter[0] = '\0';
    }
    ImGui::Separator();

    auto toLowerCopy = [](const std::string& value) {
        std::string lowered = value;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return lowered;
    };

    const std::string filterText = toLowerCopy(m_dataPanelFilter);

    if (ImGui::BeginTabBar("DataPanelTabs")) {
        // --- TAB 1: Active Tracks ---
        if (ImGui::BeginTabItem("Active Tracks")) {
            std::vector<TrackedObject> visibleObjects;
            visibleObjects.reserve(m_trackedObjects.size());

            for (const auto& obj : m_trackedObjects) {
                if (filterText.empty()) {
                    visibleObjects.push_back(obj);
                    continue;
                }

                std::string stateText;
                if (m_lockedTarget.state != TrackingState::SEARCHING && m_lockedTarget.track_id == obj.track_id) {
                    stateText = "locked";
                } else if (obj.is_confirmed) {
                    stateText = "lock";
                } else if (obj.lost_frames > 0) {
                    stateText = "lost";
                } else {
                    stateText = "init";
                }

                const std::string searchable = toLowerCopy(std::to_string(obj.track_id) + " " + obj.className + " " + stateText);
                if (searchable.find(filterText) != std::string::npos) {
                    visibleObjects.push_back(obj);
                }
            }

            auto stateRank = [&](const TrackedObject& obj) {
                if (m_lockedTarget.state != TrackingState::SEARCHING && m_lockedTarget.track_id == obj.track_id) return 3;
                if (obj.is_confirmed) return 2;
                if (obj.lost_frames > 0) return 1;
                return 0;
            };

            auto compareObjects = [&](const TrackedObject& lhs, const TrackedObject& rhs, int column, bool descending) {
                auto less = [&](const auto& a, const auto& b) {
                    return descending ? b < a : a < b;
                };

                switch (column) {
                    case 0:
                        return less(lhs.track_id, rhs.track_id);
                    case 1:
                        return less(toLowerCopy(lhs.className), toLowerCopy(rhs.className));
                    case 2: {
                        float lhsCx = static_cast<float>(lhs.box.x) + lhs.box.width / 2.0f;
                        float lhsCy = static_cast<float>(lhs.box.y) + lhs.box.height / 2.0f;
                        float rhsCx = static_cast<float>(rhs.box.x) + rhs.box.width / 2.0f;
                        float rhsCy = static_cast<float>(rhs.box.y) + rhs.box.height / 2.0f;
                        if (lhsCx == rhsCx) return less(lhsCy, rhsCy);
                        return less(lhsCx, rhsCx);
                    }
                    case 3:
                        return less(lhs.confidence, rhs.confidence);
                    case 4:
                        return less(stateRank(lhs), stateRank(rhs));
                    default:
                        return less(lhs.track_id, rhs.track_id);
                }
            };

            if (ImGui::BeginTable("Tracks", 5,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_Sortable)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 35.0f);
                ImGui::TableSetupColumn("Class",    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Pos (X,Y)",ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Conf",     ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("State",    ImGuiTableColumnFlags_WidthFixed, 55.0f);
                ImGui::TableHeadersRow();

                std::vector<TrackedObject> sortedObjects = visibleObjects;
                if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& sortSpec = sortSpecs->Specs[0];
                    std::stable_sort(sortedObjects.begin(), sortedObjects.end(), [&](const TrackedObject& lhs, const TrackedObject& rhs) {
                        if (compareObjects(lhs, rhs, sortSpec.ColumnIndex, sortSpec.SortDirection == ImGuiSortDirection_Descending)) {
                            return true;
                        }
                        if (compareObjects(rhs, lhs, sortSpec.ColumnIndex, sortSpec.SortDirection == ImGuiSortDirection_Descending)) {
                            return false;
                        }
                        return lhs.track_id < rhs.track_id;
                    });
                    sortSpecs->SpecsDirty = false;
                }

                for (const auto& obj : sortedObjects) {
                    ImGui::TableNextRow();
                    
                    bool isSelected = (m_lockedTarget.state != TrackingState::SEARCHING && m_lockedTarget.track_id == obj.track_id);
                    
                    ImGui::TableSetColumnIndex(0);
                    char idLabel[32];
                    snprintf(idLabel, sizeof(idLabel), "%03d", obj.track_id);
                    if (ImGui::Selectable(idLabel, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                        if (isSelected) {
                            m_releaseLockRequested.store(true);
                            m_selectedAnalyzerTargetId = -1;
                        } else {
                            m_requestedLockId.store(obj.track_id);
                            m_lockRequested.store(true);
                            m_selectedAnalyzerTargetId = obj.track_id;
                        }
                    }

                    char contextMenuId[64];
                    snprintf(contextMenuId, sizeof(contextMenuId), "ActiveRowContextMenu##%d", obj.track_id);
                    if (ImGui::BeginPopupContextItem(contextMenuId)) {
                        if (ImGui::MenuItem("Export Target Data (JSON/PNG)")) {
                            auto it = std::find_if(m_targetHistory.begin(), m_targetHistory.end(),
                                                   [&](const UniqueTargetRecord& r) { return r.track_id == obj.track_id; });
                            if (it != m_targetHistory.end()) {
                                exportTarget(*it);
                            } else {
                                log(LogLevel::ERR, "Target record not found in history for export.");
                            }
                        }
                        ImGui::EndPopup();
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

            ImGui::EndTabItem();
        }

        // --- TAB 2: Target History ---
        if (ImGui::BeginTabItem("Target History")) {
            std::vector<UniqueTargetRecord> visibleHistory;
            visibleHistory.reserve(m_targetHistory.size());

            for (const auto& record : m_targetHistory) {
                std::string stateText = record.is_currently_active ? "active" : "lost";
                if (filterText.empty()) {
                    visibleHistory.push_back(record);
                    continue;
                }

                const std::string searchable = toLowerCopy(std::to_string(record.track_id) + " " + record.className + " " + stateText);
                if (searchable.find(filterText) != std::string::npos) {
                    visibleHistory.push_back(record);
                }
            }

            auto compareHistory = [&](const UniqueTargetRecord& lhs, const UniqueTargetRecord& rhs, int column, bool descending) {
                auto less = [&](const auto& a, const auto& b) {
                    return descending ? b < a : a < b;
                };

                switch (column) {
                    case 0:
                        return less(lhs.track_id, rhs.track_id);
                    case 1:
                        return less(toLowerCopy(lhs.className), toLowerCopy(rhs.className));
                    case 2: {
                        float lhsCx = static_cast<float>(lhs.last_box.x) + lhs.last_box.width / 2.0f;
                        float lhsCy = static_cast<float>(lhs.last_box.y) + lhs.last_box.height / 2.0f;
                        float rhsCx = static_cast<float>(rhs.last_box.x) + rhs.last_box.width / 2.0f;
                        float rhsCy = static_cast<float>(rhs.last_box.y) + rhs.last_box.height / 2.0f;
                        if (lhsCx == rhsCx) return less(lhsCy, rhsCy);
                        return less(lhsCx, rhsCx);
                    }
                    case 3:
                        return less(lhs.max_confidence, rhs.max_confidence);
                    case 4:
                        return less(lhs.is_currently_active ? 1 : 0, rhs.is_currently_active ? 1 : 0);
                    case 5:
                        return less(lhs.first_seen_timestamp, rhs.first_seen_timestamp);
                    case 6:
                        return less(lhs.last_seen_timestamp, rhs.last_seen_timestamp);
                    default:
                        return less(lhs.track_id, rhs.track_id);
                }
            };

            if (ImGui::BeginTable("HistoryTable", 7,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_Sortable)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 35.0f);
                ImGui::TableSetupColumn("Class",    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Pos (X,Y)",ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Max Conf", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("State",    ImGuiTableColumnFlags_WidthFixed, 55.0f);
                ImGui::TableSetupColumn("Found",    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Lost",     ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                std::vector<UniqueTargetRecord> sortedHistory = visibleHistory;
                if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& sortSpec = sortSpecs->Specs[0];
                    std::stable_sort(sortedHistory.begin(), sortedHistory.end(), [&](const UniqueTargetRecord& lhs, const UniqueTargetRecord& rhs) {
                        if (compareHistory(lhs, rhs, sortSpec.ColumnIndex, sortSpec.SortDirection == ImGuiSortDirection_Descending)) {
                            return true;
                        }
                        if (compareHistory(rhs, lhs, sortSpec.ColumnIndex, sortSpec.SortDirection == ImGuiSortDirection_Descending)) {
                            return false;
                        }
                        return lhs.track_id < rhs.track_id;
                    });
                    sortSpecs->SpecsDirty = false;
                }

                for (const auto& record : sortedHistory) {
                    ImGui::TableNextRow();

                    bool isSelected = (m_selectedAnalyzerTargetId == record.track_id);

                    ImGui::TableSetColumnIndex(0);
                    char idLabel[32];
                    snprintf(idLabel, sizeof(idLabel), "%03d", record.track_id);
                    if (ImGui::Selectable(idLabel, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                        if (isSelected) {
                            m_selectedAnalyzerTargetId = -1;
                        } else {
                            m_selectedAnalyzerTargetId = record.track_id;
                            // Bidirectional sync: if target is active, lock it
                            if (record.is_currently_active) {
                                m_requestedLockId.store(record.track_id);
                                m_lockRequested.store(true);
                            }
                        }
                    }

                    char contextMenuId[64];
                    snprintf(contextMenuId, sizeof(contextMenuId), "HistoryRowContextMenu##%d", record.track_id);
                    if (ImGui::BeginPopupContextItem(contextMenuId)) {
                        if (ImGui::MenuItem("Export Target Data (JSON/PNG)")) {
                            exportTarget(record);
                        }
                        ImGui::EndPopup();
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", record.className.c_str());

                    ImGui::TableSetColumnIndex(2);
                    float cx = static_cast<float>(record.last_box.x) + record.last_box.width  / 2.0f;
                    float cy = static_cast<float>(record.last_box.y) + record.last_box.height / 2.0f;
                    ImGui::Text("%.0f, %.0f", cx, cy);

                    ImGui::TableSetColumnIndex(3);
                    ImVec4 confColor = (record.max_confidence > 0.6f)
                        ? ImVec4(0.0f, 1.0f, 0.4f, 1.0f)
                        : (record.max_confidence > 0.4f ? ImVec4(1.0f, 0.8f, 0.0f, 1.0f)
                                                       : ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    ImGui::TextColored(confColor, "%.2f", record.max_confidence);

                    ImGui::TableSetColumnIndex(4);
                    if (record.is_currently_active)
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "ACTIVE");
                    else
                        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "LOST");

                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%s", record.first_seen_timestamp.c_str());

                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%s", record.is_currently_active ? "-" : record.last_seen_timestamp.c_str());
                }
                ImGui::EndTable();
            }

            ImGui::EndTabItem();
        }

        // --- TAB 3: Export & Logging ---
        if (ImGui::BeginTabItem("Export & Logging")) {
            ImGui::TextDisabled("Session Data Recording");
            ImGui::Spacing();

            if (m_settings.dataLoggingEnabled) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("  STOP MISSION LOGGING  ", ImVec2(-1, 35))) {
                    m_settings.dataLoggingEnabled = false;
                }
                ImGui::PopStyleColor(2);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.5f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.7f, 0.1f, 1.0f));
                if (ImGui::Button("  START MISSION LOGGING  ", ImVec2(-1, 35))) {
                    m_settings.dataLoggingEnabled = true;
                }
                ImGui::PopStyleColor(2);
            }

            ImGui::Separator();
            ImGui::Columns(2, "logsettings", false);
            ImGui::SetColumnWidth(0, 120);

            ImGui::Text("Format:"); ImGui::NextColumn();
            static const char* kFmtNames[] = { "CSV (Table)", "JSON-Lines (Stream)" };
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##logfmt", &m_settings.dataLoggingFormat, kFmtNames, IM_ARRAYSIZE(kFmtNames));
            ImGui::NextColumn();

            ImGui::Text("Frequency:"); ImGui::NextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##logfreq", &m_settings.dataLoggingFreqFrames, 1, 30, "%d frames");
            ImGui::NextColumn();

            ImGui::Text("Directory:"); ImGui::NextColumn();
            static char s_logDirBuf[256] = {0};
            if (s_logDirBuf[0] == '\0' && !m_settings.dataLoggingOutputDir.empty())
                strncpy(s_logDirBuf, m_settings.dataLoggingOutputDir.c_str(), sizeof(s_logDirBuf)-1);
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - 5);
            if (ImGui::InputText("##logdir", s_logDirBuf, sizeof(s_logDirBuf))) {
                m_settings.dataLoggingOutputDir = s_logDirBuf;
            }
            ImGui::SameLine();
            if (ImGui::Button("...##opendir_log", ImVec2(ImGui::GetFrameHeight(), 0))) {
                 log(LogLevel::INFO, "Logging directory set to: " + m_settings.dataLoggingOutputDir);
            }
            ImGui::NextColumn();

            ImGui::Text("Export Dir:"); ImGui::NextColumn();
            static char s_exportDirBuf[256] = {0};
            if (s_exportDirBuf[0] == '\0' && !m_settings.exportOutputDir.empty())
                strncpy(s_exportDirBuf, m_settings.exportOutputDir.c_str(), sizeof(s_exportDirBuf)-1);
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - 5);
            if (ImGui::InputText("##exportdir", s_exportDirBuf, sizeof(s_exportDirBuf))) {
                m_settings.exportOutputDir = s_exportDirBuf;
            }
            ImGui::SameLine();
            if (ImGui::Button("...##opendir_exp", ImVec2(ImGui::GetFrameHeight(), 0))) {
                 log(LogLevel::INFO, "Export directory set to: " + m_settings.exportOutputDir);
            }
            ImGui::NextColumn();

            ImGui::Columns(1);
            ImGui::Separator();

            if (m_dataLogger->isOpen()) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "LIVE PATH: %s", m_dataLogger->getCurrentPath().c_str());
                ImGui::Text("RECORDS: %llu  |  SIZE: %.2f MB", 
                            (unsigned long long)m_dataLogger->getRowsWritten(),
                            (float)m_dataLogger->getBytesWritten() / (1024.0f * 1024.0f));
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "STATUS: IDLE (No active log file)");
            }

            ImGui::Separator();
            if (ImGui::Button("Export Target History (JSON)", ImVec2(-1, 0))) {
                exportTargetHistory();
            }
            if (ImGui::Button("Take Screenshot (PNG)", ImVec2(-1, 0))) {
                m_screenshotRequested = true;
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}


void Application::renderZoomWindow(const cv::Mat& zoomFrame) {
    ImGui::Begin("Target Zoom");

    if (m_lockedTarget.state != TrackingState::SEARCHING) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("UNLOCK TARGET / RELEASE LOCK", ImVec2(-FLT_MIN, 28.0f))) {
            m_releaseLockRequested.store(true);
        }
        ImGui::PopStyleColor(3);
        ImGui::Spacing();
    }

    if (m_lockedTarget.state != TrackingState::SEARCHING && !zoomFrame.empty()) {
        cv::Rect roi = m_lockedTarget.box;

        m_zoomRenderer->updateTexture(zoomFrame);

        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 pos   = ImGui::GetCursorScreenPos();

        float frame_aspect  = static_cast<float>(zoomFrame.cols) / static_cast<float>(zoomFrame.rows);
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

        snprintf(infoText, sizeof(infoText), "CONF: %.2f | SIZE: %dx%d (HD: %dx%d)", 
                 m_lockedTarget.confidence, zoomFrame.cols, zoomFrame.rows, roi.width, roi.height);
        drawList->AddText(ImVec2(draw_x + 8.0f, draw_y + 22.0f), hudColor, infoText);

        snprintf(infoText, sizeof(infoText), "4K ZOOM: %s | MAG: %.1fx",
             (m_settings.enable4KZoom ? "ON" : "OFF"), m_settings.targetZoomMagnification);
        drawList->AddText(ImVec2(draw_x + 8.0f, draw_y + 36.0f), hudColor, infoText);
    } else if (m_lockedTarget.state != TrackingState::SEARCHING) {
        // Target locked but zoom frame is empty (e.g. invalid bounds)
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        const char* text = "INVALID TARGET BOUNDS";
        ImVec2 textSize = ImGui::CalcTextSize(text);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + (avail.x - textSize.x) * 0.5f, pos.y + (avail.y - textSize.y) * 0.5f));
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", text);
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
    ImGui::TextDisabled("v1.10.9");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.0f);
    if (ImGui::Button("Settings...", ImVec2(110, 0))) {
        m_showSettingsWindow = !m_showSettingsWindow;
        savePersistedSettings();
    }
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

            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Capture Resolution");
            ImGui::NextColumn();
            ImGui::Text("%dx%d", m_cameraWidth, m_cameraHeight);
            ImGui::NextColumn();

            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Tracking Resolution");
            ImGui::NextColumn();
            ImGui::Text("%dx%d", m_trackingWidth, m_trackingHeight);
            ImGui::NextColumn();

            const int zoomSourceW = m_settings.enable4KZoom ? m_cameraWidth : m_trackingWidth;
            const int zoomSourceH = m_settings.enable4KZoom ? m_cameraHeight : m_trackingHeight;

            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Zoom Source Resolution");
            ImGui::NextColumn();
            ImGui::Text("%dx%d", zoomSourceW, zoomSourceH);
            ImGui::NextColumn();

            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Zoom Crop Resolution");
            ImGui::NextColumn();
            ImGui::Text("%dx%d", m_zoomWidth, m_zoomHeight);
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

            ImGui::TextDisabled("Camera Resolution & Zoom");
            settingsChanged |= ImGui::Checkbox("Request 4K Camera Resolution", &m_settings.request4KCamera);
            ImGui::SameLine();
            settingsChanged |= ImGui::Checkbox("Enable 4K Target Zoom", &m_settings.enable4KZoom);
            ImGui::SetNextItemWidth(220.0f);
            settingsChanged |= ImGui::SliderFloat("Target Zoom Magnification", &m_settings.targetZoomMagnification, 1.0f, 4.0f, "%.1fx");

            ImGui::Separator();

            ImGui::TextDisabled("Low-Light Enhancement");
            settingsChanged |= ImGui::Checkbox("Enable Low-Light Enhancement", &m_settings.lowLightEnhancement);
            if (m_settings.lowLightEnhancement) {
                ImGui::SetNextItemWidth(140);
                settingsChanged |= ImGui::SliderFloat("Contrast Clip Limit##ll", &m_settings.lowLightClipLimit, 1.0f, 10.0f, "%.1f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140);
                settingsChanged |= ImGui::SliderInt("Noise Filter Kernel##ll", &m_settings.lowLightDenoiseKernel, 0, 9);
            }

            ImGui::Separator();

            // ── Motion Detection (Plan 10) ───────────────────────────────
            ImGui::TextDisabled("Motion Detection");
            settingsChanged |= ImGui::Checkbox("Enable Motion Detection##md", &m_settings.motionDetectionEnabled);
            if (m_settings.motionDetectionEnabled) {
                ImGui::SameLine();
                settingsChanged |= ImGui::Checkbox("Show Overlay##md", &m_settings.motionShowOverlay);
                ImGui::SameLine();
                settingsChanged |= ImGui::Checkbox("Heatmap##md", &m_settings.motionHeatmapOverlay);

        if (m_settings.motionHeatmapOverlay) {
                    ImGui::SetNextItemWidth(140);
                    settingsChanged |= ImGui::SliderFloat("Heatmap Decay##md", &m_settings.motionHeatmapDecay, 0.5f, 0.99f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How fast the heatmap cools down.\n0.90 = medium persistence, 0.99 = high persistence.");

                    ImGui::SetNextItemWidth(140);
                    settingsChanged |= ImGui::SliderFloat("Heatmap Sensitivity##md", &m_settings.motionHeatmapSensitivity, 1.0f, 50.0f, "%.1f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower value = more sensitive to small movements.");

                    ImGui::SetNextItemWidth(140);
                    settingsChanged |= ImGui::SliderFloat("Heatmap Alpha##md", &m_settings.motionHeatmapAlpha, 0.0f, 1.0f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overall transparency of the heatmap overlay.");
                }

                settingsChanged |= ImGui::Checkbox("Enable Sub Zooms##sz", &m_settings.subZoomsEnabled);
                if (m_settings.subZoomsEnabled) {
                    ImGui::SameLine();
                    settingsChanged |= ImGui::Checkbox("Use Separate Windows##sz", &m_settings.subZoomsUseSeparateWindows);
                    
                    ImGui::SetNextItemWidth(220.0f);
                    settingsChanged |= ImGui::SliderInt("Sub Zoom Padding (px)##sz", &m_settings.subZoomPaddingPx, 0, 100);
                    ImGui::SetNextItemWidth(220.0f);
                    settingsChanged |= ImGui::SliderFloat("Sub Zoom Magnification##sz", &m_settings.subZoomMagnification, 1.0f, 4.0f, "%.1fx");
                }

                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::SliderFloat("Sensitivity##md", &m_settings.motionSensitivity, 5.0f, 100.0f, "%.1f")) {
                    settingsChanged = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("MOG2 variance threshold.\nLower = more sensitive (detects subtler movement).\nHigher = only strong motion triggers detection.");

                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::SliderInt("Min. Area (px)##md", &m_settings.motionMinArea, 1, 5000)) {
                    settingsChanged = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Minimum contour area in pixels.\nRegions smaller than this are discarded (removes noise).");

                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderInt("Blur Kernel##md", &m_settings.motionBlurKernel, 1, 21)) {
                    // Force odd
                    if (m_settings.motionBlurKernel > 1 && m_settings.motionBlurKernel % 2 == 0)
                        m_settings.motionBlurKernel += 1;
                    settingsChanged = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Gaussian pre-blur kernel size (must be odd, 1 = disabled).\nHigher values suppress camera sensor noise.");

                ImGui::SameLine();
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderFloat("Fill Alpha##md", &m_settings.motionOverlayAlpha, 0.0f, 1.0f, "%.2f")) {
                    settingsChanged = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Transparency of the motion region fill.\n0 = invisible fill (outline only), 1 = fully opaque.");

                // Color picker for motion overlay
                if (ImGui::ColorEdit4("Overlay Color##md", m_motionOverlayColorF,
                                       ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                    m_settings.motionOverlayColor = IM_COL32(
                        static_cast<int>(m_motionOverlayColorF[0] * 255.0f),
                        static_cast<int>(m_motionOverlayColorF[1] * 255.0f),
                        static_cast<int>(m_motionOverlayColorF[2] * 255.0f),
                        static_cast<int>(m_motionOverlayColorF[3] * 255.0f));
                    settingsChanged = true;
                }

                settingsChanged |= ImGui::Checkbox("Shadow Detection##md", &m_settings.motionDetectShadows);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Enables MOG2 shadow detection.\nReduces ghost detections from cast shadows.\nCosts ~15%% extra CPU.");

                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::SliderInt("Learning Rate##md", &m_settings.motionLearningRate, -1, 100)) {
                    settingsChanged = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("-1 = automatic (recommended).\n0 = frozen background model.\n100 = very fast adaptation (may miss slow movements).");

                if (ImGui::Button("Reset Background##md")) {
                    m_motionDetector.reset();
                    log(LogLevel::INFO, "Motion detector background model reset.");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Clears the MOG2 background model.\nUse after moving the camera or major scene changes.");
            }

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
                "VDO.Ninja Viewer (open browser)",
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
                                // VDO.Ninja quick viewer — open in default browser and prefill the viewer URL
                                const char* vdoUrl = "https://vdo.ninja/v24/?view=sKXtRAj";
                                strncpy(m_cameraInputBuf, vdoUrl, sizeof(m_cameraInputBuf));
    #ifdef __APPLE__
                                // open default browser on macOS
                                std::string cmd = std::string("open \"") + vdoUrl + "\"";
                                system(cmd.c_str());
    #endif
                            } else if (i == 7) {
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
                m_cameraAddress = newAddr;
                savePersistedSettings();
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
                    applyStandardPreset();
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

            if (m_settings.filterByPriorityClasses && m_detector) {
                ImGui::Indent(10.0f);
                
                // Search field for filtering class list
                static char classSearchQuery[64] = "";
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##class_search", "Search classes...", classSearchQuery, sizeof(classSearchQuery));
                
                std::string searchLower = classSearchQuery;
                std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), [](unsigned char c){ return std::tolower(c); });

                ImGui::Spacing();
                
                // Quick preset actions
                if (ImGui::Button("Select All")) {
                    for (int i = 0; i < m_detector->numClasses(); ++i) {
                        m_settings.priorityClasses.insert(i);
                    }
                    settingsChanged = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear All")) {
                    m_settings.priorityClasses.clear();
                    settingsChanged = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Presets...")) {
                    ImGui::OpenPopup("class_presets_popup");
                }

                if (ImGui::BeginPopup("class_presets_popup")) {
                    if (ImGui::Selectable("Traffic & Pedestrians")) {
                        m_settings.priorityClasses = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11};
                        settingsChanged = true;
                    }
                    if (ImGui::Selectable("Common (25 classes)")) {
                        m_settings.priorityClasses = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 15, 16, 24, 25, 26, 28, 39, 41, 56, 62, 63, 67, 74};
                        settingsChanged = true;
                    }
                    if (ImGui::Selectable("Animals")) {
                        m_settings.priorityClasses = {14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
                        settingsChanged = true;
                    }
                    if (ImGui::Selectable("Indoor & Electronics")) {
                        m_settings.priorityClasses = {24, 25, 26, 27, 28, 39, 40, 41, 42, 43, 44, 45, 56, 57, 58, 59, 60, 62, 63, 64, 65, 66, 67, 73, 74, 75, 76, 77, 78, 79};
                        settingsChanged = true;
                    }
                    ImGui::EndPopup();
                }

                // Render checkboxes in a 4-column table layout
                const auto& classes = m_detector->getClasses();
                const int numCols = 4;
                if (ImGui::BeginTable("ClassChecklistGrid", numCols,
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg,
                                      ImVec2(0, 180.0f))) {
                    
                    for (int i = 0; i < numCols; ++i) {
                        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
                    }

                    int visibleCount = 0;
                    for (size_t i = 0; i < classes.size(); ++i) {
                        std::string classNameLower = classes[i];
                        std::transform(classNameLower.begin(), classNameLower.end(), classNameLower.begin(), [](unsigned char c){ return std::tolower(c); });
                        
                        if (!searchLower.empty() && classNameLower.find(searchLower) == std::string::npos) {
                            continue;
                        }

                        if (visibleCount % numCols == 0) {
                            ImGui::TableNextRow();
                        }
                        ImGui::TableSetColumnIndex(visibleCount % numCols);

                        bool isChecked = (m_settings.priorityClasses.find(static_cast<int>(i)) != m_settings.priorityClasses.end());
                        char label[128];
                        snprintf(label, sizeof(label), "%s##cls_%zu", classes[i].c_str(), i);
                        if (ImGui::Checkbox(label, &isChecked)) {
                            if (isChecked) {
                                m_settings.priorityClasses.insert(static_cast<int>(i));
                            } else {
                                m_settings.priorityClasses.erase(static_cast<int>(i));
                            }
                            settingsChanged = true;
                        }
                        visibleCount++;
                    }
                    
                    if (visibleCount == 0) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("No matching classes.");
                    }
                    
                    ImGui::EndTable();
                }
                
                ImGui::Unindent(10.0f);
            } else if (!m_settings.filterByPriorityClasses) {
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
        savePersistedSettings();
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// UI: Settings Window (floating, dockable)
// -----------------------------------------------------------------------

void Application::renderSettingsWindow() {
    if (!m_showSettingsWindow) return;

    ImGui::SetNextWindowSize(ImVec2(480, 560), ImGuiCond_FirstUseEver);
    bool prevState = m_showSettingsWindow;
    if (!ImGui::Begin("Settings", &m_showSettingsWindow)) {
        ImGui::End();
        if (prevState != m_showSettingsWindow) savePersistedSettings();
        return;
    }
    if (prevState != m_showSettingsWindow) savePersistedSettings();

    bool changed = false;
    bool audioChanged = false;
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.5f, 1.0f), "APPLICATION SETTINGS");
    ImGui::Separator();

    if (ImGui::BeginTabBar("SettingsTabBar")) {
        // --- TAB 1: Display & HUD ---
        if (ImGui::BeginTabItem("Display & HUD")) {
            ImGui::Spacing();
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
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "HUD Elements Toggle");
            changed |= ImGui::Checkbox("Crosshair##s",           &m_settings.showCrosshair);
            changed |= ImGui::Checkbox("Tactical Overlay##s",    &m_settings.showTacticalOverlay);
            changed |= ImGui::Checkbox("Corner Brackets##s",     &m_settings.showCornerBrackets);
            changed |= ImGui::Checkbox("Status Windows##s",      &m_settings.showStatusWindows);
            changed |= ImGui::Checkbox("Show Track IDs##s",      &m_settings.showTrackIDs);
            changed |= ImGui::Checkbox("Show Confidence##s",     &m_settings.showConfidence);
            changed |= ImGui::Checkbox("Show Trails##s",         &m_settings.showTrails);
            changed |= ImGui::Checkbox("Fading Trail Alpha##s",  &m_settings.showTrailFade);

            ImGui::EndTabItem();
        }

        // --- TAB 2: Camera & Zoom ---
        if (ImGui::BeginTabItem("Camera & Zoom")) {
            ImGui::Spacing();
            changed |= ImGui::Checkbox("Request 4K camera resolution##s", &m_settings.request4KCamera);
            changed |= ImGui::Checkbox("Enable 4K target zoom##s", &m_settings.enable4KZoom);
            changed |= ImGui::SliderFloat("Target zoom magnification##s", &m_settings.targetZoomMagnification, 1.0f, 4.0f, "%.1fx");
            ImGui::Separator();
            ImGui::TextDisabled("Sub Zooms");
            changed |= ImGui::Checkbox("Enable Sub Zooms##szs", &m_settings.subZoomsEnabled);
            if (m_settings.subZoomsEnabled) {
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Use Separate Windows##szs", &m_settings.subZoomsUseSeparateWindows);
            }
            ImGui::Separator();
            changed |= ImGui::Checkbox("Enable Low-Light Enhancement##s", &m_settings.lowLightEnhancement);
            if (m_settings.lowLightEnhancement) {
                changed |= ImGui::SliderFloat("Contrast Clip Limit##s_ll", &m_settings.lowLightClipLimit, 1.0f, 10.0f, "%.1f");
                changed |= ImGui::SliderInt("Noise Filter Kernel##s_ll", &m_settings.lowLightDenoiseKernel, 0, 9);
            }

            ImGui::EndTabItem();
        }

        // --- TAB 3: Detection & Tracking ---
        if (ImGui::BeginTabItem("Detection & Tracking")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Detector Settings");
            changed |= ImGui::SliderFloat("Confidence##ds",  &m_settings.detectorConfThreshold,  0.01f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Score##ds",       &m_settings.detectorScoreThreshold, 0.01f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("NMS##ds",         &m_settings.detectorNmsThreshold,   0.01f, 1.0f, "%.3f");
            changed |= ImGui::SliderInt("Skip Frames##ds",   &m_settings.detectionSkipFrames, 0, 10);
            changed |= ImGui::Checkbox("Grayscale Input##ds",&m_settings.grayscaleInput);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Remote GPU Offloading (LAN)");
            changed |= ImGui::Checkbox("Enable Remote Inference##rem", &m_settings.remoteInferenceEnabled);
            
            ImGui::BeginDisabled(!m_settings.remoteInferenceEnabled);
            
            static char ipBuf[64];
            static bool ipBufInit = false;
            if (!ipBufInit) {
                strncpy(ipBuf, m_settings.remoteInferenceIp.c_str(), sizeof(ipBuf));
                ipBufInit = true;
            }
            if (ImGui::InputText("Server IP##rem", ipBuf, sizeof(ipBuf))) {
                m_settings.remoteInferenceIp = ipBuf;
                changed = true;
            }
            
            int port = m_settings.remoteInferencePort;
            if (ImGui::InputInt("Server Port##rem", &port)) {
                if (port > 0 && port < 65536) {
                    m_settings.remoteInferencePort = port;
                    changed = true;
                }
            }
            
            ImGui::EndDisabled();

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Tracker Settings");
            changed |= ImGui::SliderFloat("Min Match Score##ts", &m_settings.trackerMinMatchScore, 0.01f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Max Center Dist##ts", &m_settings.trackerMaxCenterDistPx, 10.0f, 800.0f, "%.0f");
            changed |= ImGui::SliderInt("Max Lost Frames##ts",   &m_settings.trackerMaxLostFrames,  1, 120);
            changed |= ImGui::SliderInt("Trail Length##ts",      &m_settings.trackerMaxTrailLength, 5, 200);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Re-acquisition & Recovery (Plan 11)");
            changed |= ImGui::Checkbox("High-Sensitivity ROI Scans##ra", &m_settings.trackerReacquisitionEnabled);
            if (m_settings.trackerReacquisitionEnabled) {
                changed |= ImGui::SliderFloat("Scan ROI Zoom##ra", &m_settings.trackerReacquisitionZoom, 1.1f, 4.0f, "%.1fx");
            }
            changed |= ImGui::Checkbox("Motion-Guided Fallback##ra", &m_settings.trackerUseMotionFallback);
            changed |= ImGui::SliderFloat("Lost Match Radius Mult##ra", &m_settings.trackerReacquisitionMaxDist, 1.0f, 3.0f, "%.1fx");

            ImGui::EndTabItem();
        }

        // --- TAB 4: Audio Alerts ---
        if (ImGui::BeginTabItem("Audio Alerts")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "ACOUSTIC ALERT SYSTEM");
            ImGui::Separator();

            audioChanged |= ImGui::Checkbox("Master Enable##aud", &m_settings.audioEnabled);
            ImGui::BeginDisabled(!m_settings.audioEnabled);

            audioChanged |= ImGui::SliderFloat("Master Volume##aud", &m_settings.audioMasterVolume, 0.0f, 1.0f, "%.2f");
            if (ImGui::Button("TEST ALL##aud")) {
                AudioEngine::Config previewCfg;
                previewCfg.masterEnabled       = m_settings.audioEnabled;
                previewCfg.masterVolume        = m_settings.audioMasterVolume;
                previewCfg.motionEnabled       = m_settings.audioMotionEnabled;
                previewCfg.motionFreqHz        = m_settings.audioMotionFreqHz;
                previewCfg.motionDurationMs    = m_settings.audioMotionDurationMs;
                previewCfg.motionCooldownSec   = m_settings.audioMotionCooldownSec;
                previewCfg.alarmEntryEnabled   = m_settings.audioAlarmEntryEnabled;
                previewCfg.alarmEntryFreqHz    = m_settings.audioAlarmEntryFreqHz;
                previewCfg.alarmEntryDurMs     = m_settings.audioAlarmEntryDurMs;
                previewCfg.alarmExitEnabled    = m_settings.audioAlarmExitEnabled;
                previewCfg.alarmExitFreqHz     = m_settings.audioAlarmExitFreqHz;
                previewCfg.alarmExitDurMs      = m_settings.audioAlarmExitDurMs;
                previewCfg.lockAcquiredEnabled = m_settings.audioLockAcquiredEnabled;
                previewCfg.lockAcquiredFreqHz  = m_settings.audioLockAcquiredFreqHz;
                previewCfg.lockAcquiredDurMs   = m_settings.audioLockAcquiredDurMs;
                previewCfg.lockLostEnabled     = m_settings.audioLockLostEnabled;
                previewCfg.lockLostFreqHz      = m_settings.audioLockLostFreqHz;
                previewCfg.lockLostDurMs       = m_settings.audioLockLostDurMs;
                m_audioEngine.applyConfig(previewCfg);
                m_audioEngine.playMotionAlert();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(plays motion tone)");

            ImGui::Spacing();

            if (ImGui::CollapsingHeader("Motion Alert##aud_coll", ImGuiTreeNodeFlags_DefaultOpen)) {
                audioChanged |= ImGui::Checkbox("Enable##audm", &m_settings.audioMotionEnabled);
                ImGui::BeginDisabled(!m_settings.audioMotionEnabled);
                audioChanged |= ImGui::SliderFloat("Frequency (Hz)##audm",  &m_settings.audioMotionFreqHz,     100.0f, 4000.0f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Duration (ms)##audm",   &m_settings.audioMotionDurationMs,  20.0f,  500.0f, "%.0f ms");
                audioChanged |= ImGui::SliderFloat("Cooldown (s)##audm",    &m_settings.audioMotionCooldownSec,  0.1f,   10.0f, "%.1f s");
                if (ImGui::Button("TEST##audm")) {
                    m_audioEngine.playMotionAlert();
                }
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Alarm Zone — Entry##aud_coll")) {
                audioChanged |= ImGui::Checkbox("Enable##aude", &m_settings.audioAlarmEntryEnabled);
                ImGui::BeginDisabled(!m_settings.audioAlarmEntryEnabled);
                audioChanged |= ImGui::SliderFloat("Frequency (Hz)##aude", &m_settings.audioAlarmEntryFreqHz, 100.0f, 4000.0f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Duration (ms)##aude",  &m_settings.audioAlarmEntryDurMs,   20.0f,  500.0f, "%.0f ms");
                if (ImGui::Button("TEST##aude")) {
                    m_audioEngine.playAlarmEntry();
                }
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Alarm Zone — Exit##aud_coll")) {
                audioChanged |= ImGui::Checkbox("Enable##audx", &m_settings.audioAlarmExitEnabled);
                ImGui::BeginDisabled(!m_settings.audioAlarmExitEnabled);
                audioChanged |= ImGui::SliderFloat("Frequency (Hz)##audx", &m_settings.audioAlarmExitFreqHz, 100.0f, 4000.0f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Duration (ms)##audx",  &m_settings.audioAlarmExitDurMs,   20.0f,  500.0f, "%.0f ms");
                if (ImGui::Button("TEST##audx")) {
                    m_audioEngine.playAlarmExit();
                }
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Target Lock — Acquired##aud_coll")) {
                audioChanged |= ImGui::Checkbox("Enable##audla", &m_settings.audioLockAcquiredEnabled);
                ImGui::BeginDisabled(!m_settings.audioLockAcquiredEnabled);
                audioChanged |= ImGui::SliderFloat("Frequency (Hz)##audla", &m_settings.audioLockAcquiredFreqHz, 100.0f, 4000.0f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Duration (ms)##audla",  &m_settings.audioLockAcquiredDurMs,   20.0f,  500.0f, "%.0f ms");
                if (ImGui::Button("TEST##audla")) {
                    m_audioEngine.playLockAcquired();
                }
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Target Lock — Lost##aud_coll")) {
                audioChanged |= ImGui::Checkbox("Enable##audll", &m_settings.audioLockLostEnabled);
                ImGui::BeginDisabled(!m_settings.audioLockLostEnabled);
                audioChanged |= ImGui::SliderFloat("Frequency (Hz)##audll", &m_settings.audioLockLostFreqHz, 100.0f, 4000.0f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Duration (ms)##audll",  &m_settings.audioLockLostDurMs,   20.0f,  500.0f, "%.0f ms");
                if (ImGui::Button("TEST##audll")) {
                    m_audioEngine.playLockLost();
                }
                ImGui::EndDisabled();
            }

            ImGui::EndDisabled(); // master enable

            ImGui::EndTabItem();
        }

        // --- TAB 5: System & Admin ---
        if (ImGui::BeginTabItem("System & Admin")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Logging Settings");
            const char* levelNames[] = {"VERBOSE","INFO","WARN","ERROR"};
            int lvl = m_settings.logLevel;
            if (ImGui::Combo("Log Level##ls", &lvl, levelNames, 4)) {
                m_settings.logLevel = lvl;
                changed = true;
            }
            changed |= ImGui::Checkbox("Log to File (not yet implemented)", &m_settings.logToFile);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1.0f), "Danger Zone");
            static bool confirmQuit = false;
            ImGui::Checkbox("Enable Admin Actions##s_admin", &confirmQuit);
            if (confirmQuit) {
                if (ImGui::Button("Reset All Settings##s_reset", ImVec2(-1, 0))) {
                    applyStandardPreset();
                    changed = true;
                    log(LogLevel::WARN, "All settings reset to defaults");
                }
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
                if (ImGui::Button("Quit Application##s_quit", ImVec2(-1, 0)))
                    glfwSetWindowShouldClose(m_window, true);
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("(Admin actions locked)");
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(-1, 0))) {
        m_showSettingsWindow = false;
        savePersistedSettings();
    }

    if (changed || audioChanged) {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_sharedSettings = m_settings;
        savePersistedSettings();
    }

    if (audioChanged) {
        AudioEngine::Config audioCfg;
        audioCfg.masterEnabled       = m_settings.audioEnabled;
        audioCfg.masterVolume        = m_settings.audioMasterVolume;
        audioCfg.motionEnabled       = m_settings.audioMotionEnabled;
        audioCfg.motionFreqHz        = m_settings.audioMotionFreqHz;
        audioCfg.motionDurationMs    = m_settings.audioMotionDurationMs;
        audioCfg.motionCooldownSec   = m_settings.audioMotionCooldownSec;
        audioCfg.alarmEntryEnabled   = m_settings.audioAlarmEntryEnabled;
        audioCfg.alarmEntryFreqHz    = m_settings.audioAlarmEntryFreqHz;
        audioCfg.alarmEntryDurMs     = m_settings.audioAlarmEntryDurMs;
        audioCfg.alarmExitEnabled    = m_settings.audioAlarmExitEnabled;
        audioCfg.alarmExitFreqHz     = m_settings.audioAlarmExitFreqHz;
        audioCfg.alarmExitDurMs      = m_settings.audioAlarmExitDurMs;
        audioCfg.lockAcquiredEnabled = m_settings.audioLockAcquiredEnabled;
        audioCfg.lockAcquiredFreqHz  = m_settings.audioLockAcquiredFreqHz;
        audioCfg.lockAcquiredDurMs   = m_settings.audioLockAcquiredDurMs;
        audioCfg.lockLostEnabled     = m_settings.audioLockLostEnabled;
        audioCfg.lockLostFreqHz      = m_settings.audioLockLostFreqHz;
        audioCfg.lockLostDurMs       = m_settings.audioLockLostDurMs;
        m_audioEngine.applyConfig(audioCfg);
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// Main Loop
// -----------------------------------------------------------------------

void Application::run() {
    cv::Mat currentFrame;
    cv::Mat currentZoomFrame;
    cv::Mat currentHeatmap;
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

        // ── High-Speed Texture Update (Direct from Capture) ──────────
        {
            std::lock_guard<std::mutex> lock(m_captureMutex);
            if (m_captureNewFrameDisplay) {
                m_captureFrame.copyTo(currentFrame);
                m_captureNewFrameDisplay = false;
                m_renderer->updateTexture(currentFrame);
            }
        }

        // Consume shared data
        std::vector<cv::Rect> currentMotionRegions;
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            if (m_newDataAvailable) {
                // Note: We keep currentFrame from capture for high-speed background,
                // but we still need the other shared data (zoom, heatmap, detections).
                m_sharedZoomFrame.copyTo(currentZoomFrame);
                m_sharedHeatmapFrame.copyTo(currentHeatmap);
                m_detections         = m_sharedDetections;
                m_trackedObjects     = m_sharedTrackedObjects;
                m_targetHistory      = m_sharedTargetHistory;
                currentMotionRegions = m_sharedMotionRegions;
                m_lockedTarget       = m_sharedLockedTarget;
                m_cameraFps          = m_sharedCameraFps;
                m_cameraWidth        = m_sharedCameraWidth.load();
                m_cameraHeight       = m_sharedCameraHeight.load();
                m_trackingWidth      = m_sharedTrackingWidth.load();
                m_trackingHeight     = m_sharedTrackingHeight.load();
                m_zoomWidth          = m_sharedZoomWidth.load();
                m_zoomHeight         = m_sharedZoomHeight.load();
                
                if (!currentHeatmap.empty()) {
                    m_heatmapRenderer->updateTexture(currentHeatmap);
                }
                
                // Copy sub zooms local data
                for (int i = 0; i < 4; ++i) {
                    m_subZooms[i].active = m_sharedSubZooms[i].active;
                    m_subZooms[i].motion_id = m_sharedSubZooms[i].motion_id;
                    m_subZooms[i].box = m_sharedSubZooms[i].box;
                    m_subZooms[i].isLost = m_sharedSubZooms[i].isLost;
                    m_sharedSubZooms[i].frame.copyTo(m_subZooms[i].frame);
                }
                
                m_newDataAvailable   = false;
            }
        }

        // Update sub zoom OpenGL textures
        if (m_settings.subZoomsEnabled) {
            for (int i = 0; i < 4; ++i) {
                if (m_subZooms[i].active && !m_subZooms[i].frame.empty()) {
                    m_subZoomRenderers[i]->updateTexture(m_subZooms[i].frame);
                }
            }
        }

        // Texture generation/updates for target history
        auto updateGLTexture = [](const cv::Mat& img, uint32_t& tex_id, int& tex_version, int current_version) {
            if (img.empty()) return;
            if (tex_id == 0 || current_version > tex_version) {
                if (tex_id == 0) {
                    glGenTextures(1, &tex_id);
                }
                glBindTexture(GL_TEXTURE_2D, tex_id);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, img.step / img.elemSize());

                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, img.cols, img.rows, 0, GL_BGR, GL_UNSIGNED_BYTE, img.data);

                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

                tex_version = current_version;
            }
        };

        for (const auto& record : m_targetHistory) {
            auto& texInfo = m_targetTextures[record.track_id];
            updateGLTexture(record.snapshot_first.image, texInfo.texture_id_first, texInfo.texture_version_first, record.cropped_image_first_version);
            updateGLTexture(record.snapshot_mid.image, texInfo.texture_id_mid, texInfo.texture_version_mid, record.cropped_image_mid_version);
            updateGLTexture(record.snapshot_last.image, texInfo.texture_id_last, texInfo.texture_version_last, record.cropped_image_last_version);
            
            // Sync periodic snapshot textures
            if (texInfo.gallery_version != record.gallery_version || texInfo.texture_ids_periodic.size() != record.periodic_snapshots.size()) {
                // If decimation happened or size changed, clear everything to ensure correct mapping
                for (uint32_t tid : texInfo.texture_ids_periodic) {
                    if (tid != 0) glDeleteTextures(1, &tid);
                }
                texInfo.texture_ids_periodic.clear();
                texInfo.texture_ids_periodic.resize(record.periodic_snapshots.size(), 0);
                texInfo.texture_versions_periodic.clear();
                texInfo.texture_versions_periodic.resize(record.periodic_snapshots.size(), -1);
                
                // Adjust UI selection index for decimation
                auto& uiState = m_analyzerUiStates[record.track_id];
                if (uiState.selected_snapshot_idx != -1) {
                    if (uiState.selected_snapshot_idx == 0) {
                        // Discovery is kept at index 0
                    } else if (uiState.selected_snapshot_idx % 2 == 0) {
                        uiState.selected_snapshot_idx /= 2;
                    } else {
                        uiState.selected_snapshot_idx = -1; // Image was dropped
                    }
                }

                texInfo.gallery_version = record.gallery_version;
            }
            for (size_t i = 0; i < record.periodic_snapshots.size(); ++i) {
                updateGLTexture(record.periodic_snapshots[i].image, texInfo.texture_ids_periodic[i], texInfo.texture_versions_periodic[i], 1);
            }

            texInfo.max_confidence = record.max_confidence;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save Profile (Settings)")) {
                    savePersistedSettings();
                    log(LogLevel::INFO, "Settings saved to disk");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit", "Alt+F4")) {
                    glfwSetWindowShouldClose(m_window, true);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Settings Window", "C", &m_showSettingsWindow)) savePersistedSettings();
                if (ImGui::MenuItem("Dev Console", "F12", &m_showDevConsole)) savePersistedSettings();
                if (ImGui::MenuItem("Target Analyzer", nullptr, &m_showTargetAnalyzer)) savePersistedSettings();
                ImGui::Separator();
                if (ImGui::MenuItem("Data Panel", nullptr, &m_showDataPanel)) savePersistedSettings();
                if (ImGui::MenuItem("Zoom Window", nullptr, &m_showZoomWindow)) savePersistedSettings();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Tools")) {
                if (ImGui::MenuItem("Take Screenshot", "PrintScreen")) {
                    m_screenshotRequested = true;
                }
                if (ImGui::MenuItem("Export Entire History (JSON)")) {
                    exportTargetHistory();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Open Export Directory")) {
                    std::string dir = m_settings.exportOutputDir.empty() ? "." : m_settings.exportOutputDir;
                    #ifdef _WIN32
                    std::string cmd = "explorer " + dir;
                    #elif __APPLE__
                    std::string cmd = "open " + dir;
                    #else
                    std::string cmd = "xdg-open " + dir;
                    #endif
                    system(cmd.c_str());
                    log(LogLevel::INFO, "Opening directory: " + dir);
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
                    log(LogLevel::INFO, "Settings saved to disk");
                }
                if (ImGui::MenuItem("Standard")) {
                    applyStandardPreset();
                }
                if (ImGui::BeginMenu("Presets")) {
                    if (ImGui::MenuItem("Performance")) applyPresetPerformance();
                    if (ImGui::MenuItem("Balanced"))    applyPresetBalanced();
                    if (ImGui::MenuItem("Precision"))    applyPresetPrecision();
                    if (ImGui::MenuItem("Low Light"))    applyPresetLowLight();
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("Send Feedback")) {
                    m_showFeedbackWindow = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if (m_showFeedbackWindow) {
            ImGui::OpenPopup("Feedback");
        }
        if (ImGui::BeginPopupModal("Feedback", &m_showFeedbackWindow, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Tell us what failed, what you expected, and how to reproduce it.");
            ImGui::Spacing();
            ImGui::InputTextMultiline("##feedback", m_feedbackBuf, IM_ARRAYSIZE(m_feedbackBuf), ImVec2(520, 220));
            if (!m_feedbackStatus.empty()) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", m_feedbackStatus.c_str());
            }
            const bool hasContent = std::strlen(m_feedbackBuf) > 0;
            ImGui::BeginDisabled(!hasContent);
            if (ImGui::Button("Submit")) {
                if (saveFeedback(m_feedbackBuf)) {
                    m_showFeedbackWindow = false;
                    m_feedbackStatus.clear();
                    memset(m_feedbackBuf, 0, sizeof(m_feedbackBuf));
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                m_showFeedbackWindow = false;
                m_feedbackStatus.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

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

            if (m_settings.motionDetectionEnabled && m_settings.motionHeatmapOverlay && m_heatmapRenderer->getTextureID() != 0) {
                drawList->AddImage(reinterpret_cast<void*>(static_cast<intptr_t>(m_heatmapRenderer->getTextureID())),
                                   ImVec2(view.pos_x, view.pos_y),
                                   ImVec2(view.pos_x + view.target_w, view.pos_y + view.target_h));
            }

            // ── ROI & Target Interaction: drag-to-draw/move/resize ───────────
            int hoveredZoneId = -1;
            ROIEditState hoveredAction = ROIEditState::NONE;

            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_None)) {
                ImVec2 mpos = ImGui::GetMousePos();
                cv::Point mVideo(
                    static_cast<int>((mpos.x - view.pos_x) / view.scale),
                    static_cast<int>((mpos.y - view.pos_y) / view.scale));

                auto zones = m_roiManager->getROIs();
                double tol = std::max(4.0, 8.0 / view.scale);

                if (m_editState == ROIEditState::NONE) {
                    // Check corners first
                    if (m_lockedTarget.state != TrackingState::SEARCHING && m_lockedTarget.className == "Pixel Target") {
                        cv::Rect r = m_lockedTarget.box;
                        cv::Point tl(r.x, r.y);
                        cv::Point tr(r.x + r.width, r.y);
                        cv::Point bl(r.x, r.y + r.height);
                        cv::Point br(r.x + r.width, r.y + r.height);

                        if (std::hypot(mVideo.x - tl.x, mVideo.y - tl.y) <= tol) {
                            hoveredZoneId = 999; hoveredAction = ROIEditState::RESIZING_TL;
                        } else if (std::hypot(mVideo.x - tr.x, mVideo.y - tr.y) <= tol) {
                            hoveredZoneId = 999; hoveredAction = ROIEditState::RESIZING_TR;
                        } else if (std::hypot(mVideo.x - bl.x, mVideo.y - bl.y) <= tol) {
                            hoveredZoneId = 999; hoveredAction = ROIEditState::RESIZING_BL;
                        } else if (std::hypot(mVideo.x - br.x, mVideo.y - br.y) <= tol) {
                            hoveredZoneId = 999; hoveredAction = ROIEditState::RESIZING_BR;
                        }
                    }

                    if (hoveredZoneId == -1) {
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
                    }

                    // Check edges if no corner hovered
                    if (hoveredZoneId == -1) {
                        if (m_lockedTarget.state != TrackingState::SEARCHING && m_lockedTarget.className == "Pixel Target") {
                            cv::Rect r = m_lockedTarget.box;
                            if (std::abs(mVideo.x - r.x) <= tol && mVideo.y >= r.y && mVideo.y <= r.y + r.height) {
                                hoveredZoneId = 999; hoveredAction = ROIEditState::RESIZING_L;
                            } else if (std::abs(mVideo.x - (r.x + r.width)) <= tol && mVideo.y >= r.y && mVideo.y <= r.y + r.height) {
                                hoveredZoneId = 999; hoveredAction = ROIEditState::RESIZING_R;
                            } else if (std::abs(mVideo.y - r.y) <= tol && mVideo.x >= r.x && mVideo.x <= r.x + r.width) {
                                hoveredZoneId = 999; hoveredAction = ROIEditState::RESIZING_T;
                            } else if (std::abs(mVideo.y - (r.y + r.height)) <= tol && mVideo.x >= r.x && mVideo.x <= r.x + r.width) {
                                hoveredZoneId = 999; hoveredAction = ROIEditState::RESIZING_B;
                            }
                        }
                    }

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
                        if (m_lockedTarget.state != TrackingState::SEARCHING && m_lockedTarget.className == "Pixel Target") {
                            cv::Rect r = m_lockedTarget.box;
                            if (mVideo.x > r.x && mVideo.x < r.x + r.width &&
                                mVideo.y > r.y && mVideo.y < r.y + r.height) {
                                hoveredZoneId = 999; hoveredAction = ROIEditState::MOVING;
                            }
                        }
                    }

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
                        if (m_editZoneId == 999) {
                            {
                                std::lock_guard<std::mutex> lock(m_dataMutex);
                                if (m_pixelLockRect.width > 0 && m_pixelLockRect.height > 0) {
                                    m_editDragStartRect = m_pixelLockRect;
                                } else {
                                    m_editDragStartRect = m_lockedTarget.box;
                                }
                            }
                            m_pixelLockDragging.store(true);
                        } else {
                            for (const auto& z : zones) {
                                if (z.id == m_editZoneId) {
                                    m_editDragStartRect = z.rect;
                                    break;
                                }
                            }
                        }
                    } else {
                        // Check if we clicked an object box to lock it (unified logic)
                        bool lockedOnObj = false;
                        for (const auto& obj : m_trackedObjects) {
                            if (obj.box.contains(mVideo)) {
                                m_requestedLockId.store(obj.track_id);
                                m_lockRequested.store(true);
                                m_selectedAnalyzerTargetId = obj.track_id;
                                log(LogLevel::INFO, "Target lock requested: " + obj.className +
                                   " TrackID=" + std::to_string(obj.track_id));
                                lockedOnObj = true;
                                break;
                            }
                        }

                        if (!lockedOnObj) {
                            // Start drawing NEW ROI or Pixel Target
                            m_editState = ROIEditState::DRAWING;
                            m_editDragStartMouse = mVideo;
                            m_roiManager->beginDrag(mVideo);
                        }
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
                        if (m_editZoneId == 999) {
                            // Clamp newRect to frame boundaries
                            newRect.x = std::clamp(newRect.x, 0, cols - 4);
                            newRect.y = std::clamp(newRect.y, 0, rows - 4);
                            newRect.width = std::clamp(newRect.width, 4, cols - newRect.x);
                            newRect.height = std::clamp(newRect.height, 4, rows - newRect.y);

                            std::lock_guard<std::mutex> lock(m_dataMutex);
                            m_pixelLockRect = newRect;
                            m_sharedLockedTarget.box = newRect;
                            m_lockedTarget.box = newRect;
                        } else {
                            m_roiManager->updateRect(m_editZoneId, newRect);
                        }
                    }
                }

                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    if (m_editState == ROIEditState::DRAWING) {
                        int dx = std::abs(mVideo.x - m_editDragStartMouse.x);
                        int dy = std::abs(mVideo.y - m_editDragStartMouse.y);

                        if (dx < 5 && dy < 5) {
                            // Minimal drag -> Pixel point lock (template match)
                            m_roiManager->cancelDrag();
                            std::lock_guard<std::mutex> lock(m_dataMutex);
                            m_pixelLockPoint = m_editDragStartMouse;
                            // Set a default 60x60 rect for point lock
                            int ts = 60;
                            m_pixelLockRect = cv::Rect(m_editDragStartMouse.x - ts/2, m_editDragStartMouse.y - ts/2, ts, ts);
                            m_pixelLockRequested.store(true);
                        } else {
                            // Significant drag -> Create target
                            if (m_roiEditMode) {
                                // Create ROI Zone
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
                            } else {
                                // Create Pixel Target
                                cv::Rect drawnRect = m_roiManager->getDragRect();
                                m_roiManager->cancelDrag();
                                if (drawnRect.width > 4 && drawnRect.height > 4) {
                                    std::lock_guard<std::mutex> lock(m_dataMutex);
                                    m_pixelLockRect = drawnRect;
                                    m_pixelLockRequested.store(true);
                                }
                            }
                        }
                    } else if (m_editZoneId == 999) {
                        m_pixelLockDragging.store(false);
                        {
                            std::lock_guard<std::mutex> lock(m_dataMutex);
                            m_pixelLockRect = m_lockedTarget.box;
                            m_pixelLockRectUpdateRequested.store(true);
                        }
                        log(LogLevel::INFO, "Pixel target template updated via manual drag/resize to: (" +
                            std::to_string(m_lockedTarget.box.x) + ", " + std::to_string(m_lockedTarget.box.y) + ", " +
                            std::to_string(m_lockedTarget.box.width) + "x" + std::to_string(m_lockedTarget.box.height) + ")");
                    }
                    m_editState = ROIEditState::NONE;
                    m_editZoneId = -1;
                }

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    if (m_editState == ROIEditState::DRAWING && m_roiManager->isDragging()) {
                        m_roiManager->cancelDrag();
                    } else if (m_editZoneId == 999) {
                        m_pixelLockDragging.store(false);
                        // Revert the box to original before dragging
                        std::lock_guard<std::mutex> lock(m_dataMutex);
                        m_pixelLockRect = m_editDragStartRect;
                        m_sharedLockedTarget.box = m_editDragStartRect;
                        m_lockedTarget.box = m_editDragStartRect;
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
                    bool isHoveredZone = (hoveredZoneId == z.id);
                    bool isEditingZone = (m_editZoneId == z.id);

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

                    // Render corner handles (little squares) when hovered or editing
                    if (isHoveredZone || isEditingZone) {
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

            // Draw pixel target handles
            if (m_lockedTarget.state != TrackingState::SEARCHING && m_lockedTarget.className == "Pixel Target") {
                bool isHovered = (hoveredZoneId == 999);
                bool isEditing = (m_editZoneId == 999);
                ImU32 col = (isHovered || isEditing) ? IM_COL32(255, 255, 0, 255) : IM_COL32(255, 50, 50, 255);
                
                // Only draw the box outline here if we are actively interacting with it.
                // Otherwise, the HUD::drawTrackedObject will handle the standard box rendering.
                float borderThickness = (isHovered || isEditing) ? 2.5f : 0.0f;

                ImVec2 rMin(view.pos_x + m_lockedTarget.box.x * view.scale,
                            view.pos_y + m_lockedTarget.box.y * view.scale);
                ImVec2 rMax(view.pos_x + (m_lockedTarget.box.x + m_lockedTarget.box.width)  * view.scale,
                            view.pos_y + (m_lockedTarget.box.y + m_lockedTarget.box.height) * view.scale);

                ImVec2 tl(rMin.x, rMin.y);
                ImVec2 tr(rMax.x, rMin.y);
                ImVec2 bl(rMin.x, rMax.y);
                ImVec2 br(rMax.x, rMax.y);

                if (borderThickness > 0.0f) {
                    drawList->AddRect(rMin, rMax, col, 0.0f, 0, borderThickness);
                }
                
                drawList->AddRectFilled(rMin, rMax, IM_COL32(255, 50, 50, 25)); // Faint red fill
                
                if (isHovered || isEditing) {
                    drawList->AddRectFilled(ImVec2(tl.x - 3, tl.y - 3), ImVec2(tl.x + 3, tl.y + 3), col);
                    drawList->AddRectFilled(ImVec2(tr.x - 3, tr.y - 3), ImVec2(tr.x + 3, tr.y + 3), col);
                    drawList->AddRectFilled(ImVec2(bl.x - 3, bl.y - 3), ImVec2(bl.x + 3, bl.y + 3), col);
                    drawList->AddRectFilled(ImVec2(br.x - 3, br.y - 3), ImVec2(br.x + 3, br.y + 3), col);
                }

                char targetLbl[64];
                snprintf(targetLbl, sizeof(targetLbl), "[PIXEL TARGET] ID:%d", m_lockedTarget.track_id);
                drawList->AddText(ImVec2(rMin.x + 4, rMin.y + 3), col, targetLbl);
            }

            // ── Regular HUD and target locking ────────────────────────
            // HUD and target locking now handled via contextual drag in Camera View block.

            // ── Motion Detection Overlay (Plan 10) ─────────────────────
            // Drawn before tracked objects so motion regions appear behind
            // tracking bounding boxes (lower z-order on same draw list).
            if (m_settings.motionDetectionEnabled && m_settings.motionShowOverlay) {
                m_hud->drawMotionOverlay(drawList, currentMotionRegions, view, m_settings);
            }

            m_hud->render(drawList, static_cast<int>(avail.x), static_cast<int>(avail.y),
                          m_cameraFps, m_trackedObjects, m_lockedTarget, view, m_settings);

            if (m_settings.subZoomsEnabled && !m_settings.subZoomsUseSeparateWindows) {
                // Draw inserts at edges
                float insert_w = 120.0f;
                float insert_h = 120.0f;
                float margin = 20.0f;
                
                // Scale inserts if viewport is small
                float scaleFactor = std::min(1.0f, view.target_w / 640.0f);
                insert_w *= scaleFactor;
                insert_h *= scaleFactor;
                margin *= scaleFactor;
                
                ImU32 outlineColorActive = IM_COL32(0, 200, 100, 220); // Green
                ImU32 outlineColorHolding = IM_COL32(255, 120, 0, 220); // Orange
                ImU32 lineColorActive = IM_COL32(0, 200, 100, 150);
                ImU32 lineColorHolding = IM_COL32(255, 120, 0, 100);
                
                if (m_settings.hudColor != 0) {
                    outlineColorActive = ApplyBrightnessLocal(m_settings.hudColor, m_settings.hudBrightness);
                    lineColorActive = ApplyBrightnessLocal(m_settings.hudColor, m_settings.hudBrightness * 0.7f);
                }
                
                // Setup obstacle list for collision avoidance
                struct CollisionBox {
                    float x1, y1, x2, y2;
                };
                std::vector<CollisionBox> obstacles;
                
                // 1. Tracked objects
                for (const auto& obj : m_trackedObjects) {
                    float ox1 = view.pos_x + obj.box.x * view.scale;
                    float oy1 = view.pos_y + obj.box.y * view.scale;
                    float ox2 = ox1 + obj.box.width * view.scale;
                    float oy2 = oy1 + obj.box.height * view.scale;
                    float pad = 12.0f;
                    obstacles.push_back({ox1 - pad, oy1 - pad, ox2 + pad, oy2 + pad});
                }
                // 2. Locked target (if not in m_trackedObjects)
                if (m_lockedTarget.state != TrackingState::SEARCHING) {
                    float ox1 = view.pos_x + m_lockedTarget.box.x * view.scale;
                    float oy1 = view.pos_y + m_lockedTarget.box.y * view.scale;
                    float ox2 = ox1 + m_lockedTarget.box.width * view.scale;
                    float oy2 = oy1 + m_lockedTarget.box.height * view.scale;
                    float pad = 12.0f;
                    obstacles.push_back({ox1 - pad, oy1 - pad, ox2 + pad, oy2 + pad});
                }
                // 3. HUD elements
                obstacles.push_back({
                    view.pos_x + margin - 5.0f,
                    view.pos_y + margin - 5.0f,
                    view.pos_x + margin + 180.0f * scaleFactor + 5.0f,
                    view.pos_y + margin + 100.0f * scaleFactor + 5.0f
                });
                obstacles.push_back({
                    view.pos_x + margin - 5.0f,
                    view.pos_y + view.target_h - 60.0f * scaleFactor - margin - 5.0f,
                    view.pos_x + margin + 180.0f * scaleFactor + 5.0f,
                    view.pos_y + view.target_h - margin + 5.0f
                });

                std::vector<CollisionBox> placedSubzooms;

                float min_x = view.pos_x + margin;
                float max_x = view.pos_x + view.target_w - insert_w - margin;
                float min_y = view.pos_y + margin;
                float max_y = view.pos_y + view.target_h - insert_h - margin;

                auto getClosestPointOnRect = [](ImVec2 rMin, ImVec2 rMax, ImVec2 p) -> ImVec2 {
                    return ImVec2(
                        std::clamp(p.x, rMin.x, rMax.x),
                        std::clamp(p.y, rMin.y, rMax.y)
                    );
                };

                for (int i = 0; i < 4; ++i) {
                    if (m_subZooms[i].active) {
                        ImVec2 pos;
                        if (max_x < min_x || max_y < min_y) {
                            // Small viewport fallback
                            if (i == 0) pos = ImVec2(view.pos_x + margin, view.pos_y + margin + 110.0f * scaleFactor);
                            else if (i == 1) pos = ImVec2(view.pos_x + view.target_w - insert_w - margin, view.pos_y + margin);
                            else if (i == 2) pos = ImVec2(view.pos_x + margin, view.pos_y + view.target_h - insert_h - margin - 70.0f * scaleFactor);
                            else pos = ImVec2(view.pos_x + view.target_w - insert_w - margin, view.pos_y + view.target_h - insert_h - margin);
                        } else {
                            // Find best position using sliding candidates from default corners
                            ImVec2 defaultPos;
                            if (i == 0) defaultPos = ImVec2(min_x, min_y);
                            else if (i == 1) defaultPos = ImVec2(max_x, min_y);
                            else if (i == 2) defaultPos = ImVec2(min_x, max_y);
                            else defaultPos = ImVec2(max_x, max_y);

                            std::vector<ImVec2> candidates;
                            candidates.push_back(defaultPos);

                            float step = 25.0f;
                            if (i == 0) {
                                for (float y = min_y + step; y <= max_y; y += step) candidates.push_back(ImVec2(min_x, y));
                                for (float x = min_x + step; x <= max_x; x += step) candidates.push_back(ImVec2(x, min_y));
                            } else if (i == 1) {
                                for (float y = min_y + step; y <= max_y; y += step) candidates.push_back(ImVec2(max_x, y));
                                for (float x = max_x - step; x >= min_x; x -= step) candidates.push_back(ImVec2(x, min_y));
                            } else if (i == 2) {
                                for (float y = max_y - step; y >= min_y; y -= step) candidates.push_back(ImVec2(min_x, y));
                                for (float x = min_x + step; x <= max_x; x += step) candidates.push_back(ImVec2(x, max_y));
                            } else if (i == 3) {
                                for (float y = max_y - step; y >= min_y; y -= step) candidates.push_back(ImVec2(max_x, y));
                                for (float x = max_x - step; x >= min_x; x -= step) candidates.push_back(ImVec2(x, max_y));
                            }

                            ImVec2 bestPos = defaultPos;
                            float minOverlap = -1.0f;

                            for (const auto& cand : candidates) {
                                float cx1 = cand.x;
                                float cy1 = cand.y;
                                float cx2 = cx1 + insert_w;
                                float cy2 = cy1 + insert_h;
                                
                                float totalOverlap = 0.0f;
                                for (const auto& obs : obstacles) {
                                    float ox1 = std::max(cx1, obs.x1);
                                    float oy1 = std::max(cy1, obs.y1);
                                    float ox2 = std::min(cx2, obs.x2);
                                    float oy2 = std::min(cy2, obs.y2);
                                    if (ox2 > ox1 && oy2 > oy1) {
                                        totalOverlap += (ox2 - ox1) * (oy2 - oy1);
                                    }
                                }
                                
                                for (const auto& pz : placedSubzooms) {
                                    float ox1 = std::max(cx1, pz.x1);
                                    float oy1 = std::max(cy1, pz.y1);
                                    float ox2 = std::min(cx2, pz.x2);
                                    float oy2 = std::min(cy2, pz.y2);
                                    if (ox2 > ox1 && oy2 > oy1) {
                                        totalOverlap += (ox2 - ox1) * (oy2 - oy1);
                                    }
                                }
                                
                                if (totalOverlap == 0.0f) {
                                    bestPos = cand;
                                    minOverlap = 0.0f;
                                    break;
                                }
                                
                                if (minOverlap < 0.0f || totalOverlap < minOverlap) {
                                    minOverlap = totalOverlap;
                                    bestPos = cand;
                                }
                            }

                            pos = bestPos;
                            placedSubzooms.push_back({pos.x, pos.y, pos.x + insert_w, pos.y + insert_h});
                        }
                        
                        ImVec2 mCenter(
                            view.pos_x + (m_subZooms[i].box.x + m_subZooms[i].box.width / 2.0f) * view.scale,
                            view.pos_y + (m_subZooms[i].box.y + m_subZooms[i].box.height / 2.0f) * view.scale
                        );
                        
                        ImVec2 startPt = getClosestPointOnRect(pos, ImVec2(pos.x + insert_w, pos.y + insert_h), mCenter);
                        ImU32 lineCol = m_subZooms[i].isLost ? lineColorHolding : lineColorActive;
                        ImU32 borderCol = m_subZooms[i].isLost ? outlineColorHolding : outlineColorActive;
                        
                        // Draw leader line first
                        if (m_subZooms[i].isLost) {
                            float dx = mCenter.x - startPt.x;
                            float dy = mCenter.y - startPt.y;
                            float len = std::hypot(dx, dy);
                            if (len > 1.0f) {
                                dx /= len; dy /= len;
                                float dist = 0.0f;
                                float dashLen = 6.0f;
                                float gapLen = 4.0f;
                                while (dist < len) {
                                    float nextDist = std::min(len, dist + dashLen);
                                    drawList->AddLine(
                                        ImVec2(startPt.x + dx * dist, startPt.y + dy * dist),
                                        ImVec2(startPt.x + dx * nextDist, startPt.y + dy * nextDist),
                                        lineCol, 1.0f
                                    );
                                    dist += dashLen + gapLen;
                                }
                            }
                        } else {
                            drawList->AddLine(startPt, mCenter, lineCol, 1.0f);
                        }
                        
                        // Draw target marker consisting of outline box and outline circle
                        ImVec2 targetMin(
                            view.pos_x + m_subZooms[i].box.x * view.scale,
                            view.pos_y + m_subZooms[i].box.y * view.scale
                        );
                        ImVec2 targetMax(
                            targetMin.x + m_subZooms[i].box.width * view.scale,
                            targetMin.y + m_subZooms[i].box.height * view.scale
                        );
                        drawList->AddRect(targetMin, targetMax, borderCol, 0.0f, 0, 2.5f);

                        float markerRadius = std::min(40.0f * scaleFactor, std::min(targetMax.x - targetMin.x, targetMax.y - targetMin.y) * 0.4f);
                        if (markerRadius < 4.0f) markerRadius = 4.0f;
                        drawList->AddCircle(mCenter, markerRadius, borderCol, 16, 2.5f);
                        
                        if (m_subZoomRenderers[i]->getTextureID() != 0) {
                            drawList->AddImage(
                                reinterpret_cast<void*>(static_cast<intptr_t>(m_subZoomRenderers[i]->getTextureID())),
                                pos, ImVec2(pos.x + insert_w, pos.y + insert_h)
                            );
                        }
                        
                        drawList->AddRect(pos, ImVec2(pos.x + insert_w, pos.y + insert_h), borderCol, 0.0f, 0, 2.5f);
                        
                        char slotName[32];
                        snprintf(slotName, sizeof(slotName), "M-%02d", m_subZooms[i].motion_id);
                        
                        drawList->AddRectFilled(
                            ImVec2(pos.x + 2, pos.y + 2),
                            ImVec2(pos.x + 40 * scaleFactor, pos.y + 16 * scaleFactor),
                            IM_COL32(0, 0, 0, 180)
                        );
                        drawList->AddText(
                            ImVec2(pos.x + 4, pos.y + 2),
                            borderCol,
                            slotName
                        );
                        
                        if (m_subZooms[i].isLost) {
                            // Plan 11 UI improvement: Move HOLD to corner, remove darkening
                            const char* holdText = "HOLD";
                            ImVec2 textSize = ImGui::CalcTextSize(holdText);
                            float holdX = pos.x + insert_w - textSize.x - 4.0f;
                            float holdY = pos.y + 2.0f;

                            drawList->AddRectFilled(
                                ImVec2(holdX - 2.0f, holdY),
                                ImVec2(pos.x + insert_w - 2.0f, holdY + 16 * scaleFactor),
                                IM_COL32(0, 0, 0, 180)
                            );
                            drawList->AddText(
                                ImVec2(holdX, holdY),
                                outlineColorHolding,
                                holdText
                            );
                        }
                    }
                }
            }
        }
        ImGui::End();

        // Render 4 separate sub zoom windows if enabled
        if (m_settings.subZoomsEnabled && m_settings.subZoomsUseSeparateWindows) {
            for (int i = 0; i < 4; ++i) {
                if (m_subZooms[i].active) {
                    char windowName[64];
                    snprintf(windowName, sizeof(windowName), "Sub Zoom %d (M-%02d)", i + 1, m_subZooms[i].motion_id);
                    
                    ImGui::SetNextWindowSize(ImVec2(180, 220), ImGuiCond_FirstUseEver);
                    ImGui::Begin(windowName);
                    
                    if (m_subZoomRenderers[i]->getTextureID() != 0) {
                        ImVec2 avail = ImGui::GetContentRegionAvail();
                        float size = std::min(avail.x, avail.y - 30.0f);
                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        
                        ImDrawList* wDrawList = ImGui::GetWindowDrawList();
                        wDrawList->AddImage(
                            reinterpret_cast<void*>(static_cast<intptr_t>(m_subZoomRenderers[i]->getTextureID())),
                            pos, ImVec2(pos.x + size, pos.y + size)
                        );
                        
                        ImU32 borderCol = m_subZooms[i].isLost ? IM_COL32(255, 120, 0, 220) : IM_COL32(0, 200, 100, 220);
                        if (m_settings.hudColor != 0) {
                            borderCol = m_subZooms[i].isLost ? IM_COL32(255, 120, 0, 220) : ApplyBrightnessLocal(m_settings.hudColor, m_settings.hudBrightness);
                        }
                        
                        wDrawList->AddRect(pos, ImVec2(pos.x + size, pos.y + size), borderCol, 0.0f, 0, 2.5f);
                        
                        ImGui::Dummy(ImVec2(size, size));
                        
                        if (m_subZooms[i].isLost) {
                            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "STATUS: HOLDING");
                        } else {
                            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "STATUS: TRACKING");
                        }
                        ImGui::Text("Box: %d,%d %dx%d", m_subZooms[i].box.x, m_subZooms[i].box.y, m_subZooms[i].box.width, m_subZooms[i].box.height);
                    } else {
                        ImGui::Text("No Frame Available");
                    }
                    
                    ImGui::End();
                }
            }
        }

        // ── 2. Data Panel ────────────────────────────────────────────────
        if (m_showDataPanel) renderDataPanel();

        // ── 3. Zoom Window ───────────────────────────────────────────────
        if (m_showZoomWindow) renderZoomWindow(currentZoomFrame);

        // ── 4. Dev Console ───────────────────────────────────────────────
        if (m_showDevConsole) renderDevConsole();

        // ── 5. Settings Window (floating) ────────────────────────────────
        if (m_showSettingsWindow) renderSettingsWindow();

        // ── 6. Target Analyzer Window (dockable/floating) ────────────────
        if (m_showTargetAnalyzer) renderTargetAnalyzer();

        // Keyboard Shortcuts
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
            m_settings.dataLoggingEnabled = !m_settings.dataLoggingEnabled;
            log(LogLevel::INFO, m_settings.dataLoggingEnabled ? "Data logging STARTED via shortcut" : "Data logging STOPPED via shortcut");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PrintScreen)) {
            m_screenshotRequested = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_C)) {
            m_showSettingsWindow = !m_showSettingsWindow; // Calibration/Settings
            savePersistedSettings();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
            m_showDevConsole = !m_showDevConsole;
            savePersistedSettings();
        }

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

if (m_screenshotRequested.exchange(false)) {
    takeScreenshot();
}

glfwSwapBuffers(m_window);
}
}

bool Application::saveFeedback(const std::string& feedback) {
    const auto now = std::chrono::system_clock::now();
    const auto in_time_t = std::chrono::system_clock::to_time_t(now);

    std::filesystem::path feedbackDir = std::filesystem::path(PROJECT_HORUS_SOURCE_DIR) / "feedback";

    std::error_code ec;
    std::filesystem::create_directories(feedbackDir, ec);
    if (ec) {
        m_feedbackStatus = "Feedback konnte nicht gespeichert werden: Zielordner ist nicht verfügbar.";
        log(LogLevel::ERR, m_feedbackStatus);
        return false;
    }

    std::stringstream filename;
    filename << "feedback_" << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S") << ".json";
    const std::filesystem::path feedbackPath = feedbackDir / filename.str();

    std::ofstream file(feedbackPath);
    if (!file.is_open()) {
        m_feedbackStatus = "Feedback konnte nicht gespeichert werden: Datei lässt sich nicht öffnen.";
        log(LogLevel::ERR, m_feedbackStatus + " (" + feedbackPath.string() + ")");
        return false;
    }

    file << "{\n";
    file << "  \"timestamp\": \"" << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S") << "\",\n";
    file << "  \"feedback\": \"" << EscapeJsonString(feedback) << "\"\n";
    file << "}\n";

    if (!file.good()) {
        m_feedbackStatus = "Feedback konnte nicht vollständig geschrieben werden.";
        log(LogLevel::ERR, m_feedbackStatus + " (" + feedbackPath.string() + ")");
        return false;
    }

    m_feedbackStatus = "Feedback gespeichert.";
    log(LogLevel::INFO, "Feedback saved to " + feedbackPath.string());
    return true;
}

void Application::updateTargetHistory(const std::vector<TrackedObject>& activeTracks, const cv::Mat& currentFrame) {
    if (currentFrame.empty()) return;

    int manualCaptureId = m_manualCaptureTargetId.exchange(-1);

    auto getTimestamp = []() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        struct tm buf;
        #ifdef _WIN32
        localtime_s(&buf, &in_time_t);
        #else
        localtime_r(&in_time_t, &buf);
        #endif
        char timeStr[64];
        std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &buf);
        return std::string(timeStr);
    };

    std::string timestamp = getTimestamp();

    // Mark all existing history records as inactive first, but keep track of previously active IDs
    std::vector<int> previouslyActiveIds;
    for (auto& record : m_internalTargetHistory) {
        if (record.is_currently_active) {
            previouslyActiveIds.push_back(record.track_id);
        }
        record.is_currently_active = false;
    }

    for (const auto& obj : activeTracks) {
        // Find if already in history
        auto it = std::find_if(m_internalTargetHistory.begin(), m_internalTargetHistory.end(),
                               [&](const UniqueTargetRecord& r) { return r.track_id == obj.track_id; });

        if (it == m_internalTargetHistory.end()) {
            // New target!
            UniqueTargetRecord record;
            record.track_id = obj.track_id;
            record.class_id = obj.class_id;
            record.className = obj.className;
            record.max_confidence = obj.confidence;
            record.first_seen_timestamp = timestamp;
            record.last_seen_timestamp = timestamp;
            record.first_box = obj.box;
            record.last_box = obj.box;
            record.trail = obj.trail;
            record.is_currently_active = true;

            // Face Recognition (Plan 11)
            record.face_id   = obj.face_id;
            record.face_name = obj.face_name;

            // Crop image with expanded padding (50% on each side)
            float paddingFactor = 0.5f;
            cv::Rect roi = obj.box;
            int padW = static_cast<int>(roi.width * paddingFactor);
            int padH = static_cast<int>(roi.height * paddingFactor);
            roi.x -= padW;
            roi.y -= padH;
            roi.width += 2 * padW;
            roi.height += 2 * padH;

            // Clamp to frame boundaries
            roi.x = std::max(0, roi.x);
            roi.y = std::max(0, roi.y);
            if (roi.x + roi.width > currentFrame.cols) roi.width = currentFrame.cols - roi.x;
            if (roi.y + roi.height > currentFrame.rows) roi.height = currentFrame.rows - roi.y;

            if (roi.width > 0 && roi.height > 0) {
                cv::Mat crop = currentFrame(roi).clone();
                
                TargetSnapshot snap;
                snap.image = crop;
                snap.timestamp = timestamp;
                snap.box = obj.box;
                snap.confidence = obj.confidence;

                record.snapshot_first = snap;
                record.snapshot_mid = snap;
                record.snapshot_last = snap;

                // Add the very first snapshot
                record.periodic_snapshots.push_back(snap);
            }
            record.last_snapshot_time = std::chrono::steady_clock::now();
            record.cropped_image_first_version = 1;
            record.cropped_image_mid_version = 1;
            record.cropped_image_last_version = 1;

            m_internalTargetHistory.push_back(record);
        } else {
            // Existing target
            it->last_seen_timestamp = timestamp;
            it->last_box = obj.box;
            it->trail = obj.trail;
            it->is_currently_active = true;

            // Face Recognition (Plan 11)
            if (obj.face_id != -1) {
                it->face_id   = obj.face_id;
                it->face_name = obj.face_name;
            }

            if (obj.confidence > it->max_confidence) {
                it->max_confidence = obj.confidence;
            }

            // Check if adaptive interval has elapsed since last snapshot
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - it->last_snapshot_time).count() >= it->current_snapshot_interval_ms) {
                float paddingFactor = 0.5f;
                cv::Rect roi = obj.box;
                int padW = static_cast<int>(roi.width * paddingFactor);
                int padH = static_cast<int>(roi.height * paddingFactor);
                roi.x -= padW;
                roi.y -= padH;
                roi.width += 2 * padW;
                roi.height += 2 * padH;

                roi.x = std::max(0, roi.x);
                roi.y = std::max(0, roi.y);
                if (roi.x + roi.width > currentFrame.cols) roi.width = currentFrame.cols - roi.x;
                if (roi.y + roi.height > currentFrame.rows) roi.height = currentFrame.rows - roi.y;
                if (roi.width > 0 && roi.height > 0) {
                    TargetSnapshot snap;
                    snap.image = currentFrame(roi).clone();
                    snap.timestamp = timestamp;
                    snap.box = obj.box;
                    snap.confidence = obj.confidence;
                    it->periodic_snapshots.push_back(snap);
                    it->last_snapshot_time = now;

                    // Adaptive Decimation (Military-Grade)
                    // Keep max 24 images per target. If full, drop every 2nd image and double the interval.
                    const size_t MAX_GALLERY_SIZE = 24;
                    if (it->periodic_snapshots.size() > MAX_GALLERY_SIZE) {
                        std::vector<TargetSnapshot> decimated;
                        decimated.reserve(MAX_GALLERY_SIZE);
                        
                        // Always keep Discovery (0) and decimate the rest
                        decimated.push_back(it->periodic_snapshots[0]);
                        for (size_t i = 2; i < it->periodic_snapshots.size(); i += 2) {
                            decimated.push_back(it->periodic_snapshots[i]);
                        }
                        
                        it->periodic_snapshots = std::move(decimated);
                        it->current_snapshot_interval_ms *= 2;
                        it->gallery_version++;
                        
                        // Adjust manual selection index if it exists
                        if (it->snapshot_mid_manual_idx != -1) {
                            if (it->snapshot_mid_manual_idx == 0) {
                                // Discovery is kept
                            } else if (it->snapshot_mid_manual_idx % 2 == 0) {
                                it->snapshot_mid_manual_idx = it->snapshot_mid_manual_idx / 2;
                            } else {
                                it->snapshot_mid_manual_idx = -1; // Image was deleted, reset to auto-best
                            }
                        }
                        
                        log(LogLevel::INFO, "Decimated gallery for Target ID " + std::to_string(it->track_id) + ". New interval: " + std::to_string(it->current_snapshot_interval_ms) + "ms");
                    }

                    it->snapshot_last = snap;
                    it->cropped_image_last_version++;

                    if (it->snapshot_mid_manual_idx == -1) {
                        int midIdx = it->periodic_snapshots.size() / 2;
                        it->snapshot_mid = it->periodic_snapshots[midIdx];
                        it->cropped_image_mid_version++;
                    }
                }
            }

            // Or if manual capture was explicitly requested for this target ID
            if (manualCaptureId == obj.track_id) {
                float paddingFactor = 0.5f;
                cv::Rect roi = obj.box;
                int padW = static_cast<int>(roi.width * paddingFactor);
                int padH = static_cast<int>(roi.height * paddingFactor);
                roi.x -= padW;
                roi.y -= padH;
                roi.width += 2 * padW;
                roi.height += 2 * padH;

                roi.x = std::max(0, roi.x);
                roi.y = std::max(0, roi.y);
                if (roi.x + roi.width > currentFrame.cols) roi.width = currentFrame.cols - roi.x;
                if (roi.y + roi.height > currentFrame.rows) roi.height = currentFrame.rows - roi.y;
                if (roi.width > 0 && roi.height > 0) {
                    TargetSnapshot snap;
                    snap.image = currentFrame(roi).clone();
                    snap.timestamp = timestamp;
                    snap.box = obj.box;
                    snap.confidence = obj.confidence;
                    
                    it->periodic_snapshots.push_back(snap);
                    it->last_snapshot_time = now;

                    if (it->snapshot_mid_manual_idx == -1) {
                        it->snapshot_mid = snap;
                        it->cropped_image_mid_version++;
                    }
                    log(LogLevel::INFO, "Manual snapshot captured and added to gallery for Target ID " + std::to_string(obj.track_id));
                }
            }
        }
    }

    // Check for targets that transitioned to lost (archived)
    for (auto& record : m_internalTargetHistory) {
        if (!record.is_currently_active && std::find(previouslyActiveIds.begin(), previouslyActiveIds.end(), record.track_id) != previouslyActiveIds.end()) {
            // Target was just lost! Finalize visual chronology from periodic_snapshots
            if (!record.periodic_snapshots.empty()) {
                record.snapshot_first = record.periodic_snapshots.front();
                record.cropped_image_first_version++;

                if (record.snapshot_mid_manual_idx != -1 && record.snapshot_mid_manual_idx < (int)record.periodic_snapshots.size()) {
                    record.snapshot_mid = record.periodic_snapshots[record.snapshot_mid_manual_idx];
                } else {
                    int midIdx = record.periodic_snapshots.size() / 2;
                    record.snapshot_mid = record.periodic_snapshots[midIdx];
                }
                record.cropped_image_mid_version++;

                record.snapshot_last = record.periodic_snapshots.back();
                record.cropped_image_last_version++;

                log(LogLevel::INFO, "Finalized visual chronology for Target ID " + std::to_string(record.track_id) + ". Gallery preserved (" + std::to_string(record.periodic_snapshots.size()) + " images).");
            }
        }
    }
}

bool Application::exportTarget(const UniqueTargetRecord& record) {
    std::string baseDir = m_settings.exportOutputDir;
    if (baseDir.empty()) {
        baseDir = ".";
    }

    std::error_code ec;
    std::filesystem::create_directories(baseDir, ec);

    std::string idStr = std::to_string(record.track_id);
    while (idStr.length() < 3) idStr = "0" + idStr;

    std::string jsonPath = baseDir + "/target_" + idStr + "_details.json";
    std::string imgPathFirst = baseDir + "/target_" + idStr + "_discovery.png";
    std::string imgPathMid   = baseDir + "/target_" + idStr + "_midtrack.png";
    std::string imgPathLast  = baseDir + "/target_" + idStr + "_lastseen.png";

    // Write JSON file
    std::ofstream file(jsonPath);
    if (!file.is_open()) {
        log(LogLevel::ERR, "Failed to open file for target export: " + jsonPath);
        return false;
    }

    file << "{\n";
    file << "  \"track_id\": " << record.track_id << ",\n";
    file << "  \"class_id\": " << record.class_id << ",\n";
    file << "  \"className\": \"" << record.className << "\",\n";
    file << "  \"max_confidence\": " << record.max_confidence << ",\n";
    file << "  \"first_seen\": \"" << record.first_seen_timestamp << "\",\n";
    file << "  \"last_seen\": \"" << record.last_seen_timestamp << "\",\n";
    file << "  \"first_box\": {\"x\": " << record.first_box.x << ", \"y\": " << record.first_box.y 
         << ", \"w\": " << record.first_box.width << ", \"h\": " << record.first_box.height << "},\n";
    file << "  \"last_box\": {\"x\": " << record.last_box.x << ", \"y\": " << record.last_box.y 
         << ", \"w\": " << record.last_box.width << ", \"h\": " << record.last_box.height << "},\n";
    file << "  \"trail\": [\n";
    for (size_t i = 0; i < record.trail.size(); ++i) {
        file << "    {\"x\": " << record.trail[i].x << ", \"y\": " << record.trail[i].y << "}";
        if (i + 1 < record.trail.size()) file << ",";
        file << "\n";
    }
    file << "  ]\n";
    file << "}\n";
    file.close();

    // Write image keyframes
    auto writeKeyframe = [&](const std::string& path, const cv::Mat& img, const char* label) {
        if (!img.empty()) {
            if (cv::imwrite(path, img)) {
                log(LogLevel::INFO, "Exported " + std::string(label) + " target image to " + path);
            } else {
                log(LogLevel::ERR, "Failed to write " + std::string(label) + " target image: " + path);
            }
        }
    };

    writeKeyframe(imgPathFirst, record.snapshot_first.image, "discovery");
    writeKeyframe(imgPathMid, record.snapshot_mid.image, "midtrack");
    writeKeyframe(imgPathLast, record.snapshot_last.image, "lastseen");

    log(LogLevel::INFO, "Exported target details to " + jsonPath);
    return true;
}

bool Application::exportTargetHistory() {
    std::string baseDir = m_settings.exportOutputDir;
    if (baseDir.empty()) {
        baseDir = ".";
    }

    std::error_code ec;
    std::filesystem::create_directories(baseDir, ec);

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    struct tm buf;
    #ifdef _WIN32
    localtime_s(&buf, &in_time_t);
    #else
    localtime_r(&in_time_t, &buf);
    #endif
    char timebuf[64];
    std::strftime(timebuf, sizeof(timebuf), "%Y%m%d_%H%M%S", &buf);

    std::string jsonPath = baseDir + "/horus_history_" + timebuf + ".json";

    std::ofstream file(jsonPath);
    if (!file.is_open()) {
        log(LogLevel::ERR, "Failed to open file for history export: " + jsonPath);
        return false;
    }

    file << "{\n";
    file << "  \"export_time\": \"" << timebuf << "\",\n";
    file << "  \"targets\": [\n";
    for (size_t i = 0; i < m_targetHistory.size(); ++i) {
        const auto& record = m_targetHistory[i];
        file << "    {\n";
        file << "      \"track_id\": " << record.track_id << ",\n";
        file << "      \"class_id\": " << record.class_id << ",\n";
        file << "      \"className\": \"" << record.className << "\",\n";
        file << "      \"max_confidence\": " << record.max_confidence << ",\n";
        file << "      \"first_seen\": \"" << record.first_seen_timestamp << "\",\n";
        file << "      \"last_seen\": \"" << record.last_seen_timestamp << "\",\n";
        file << "      \"trail_len\": " << record.trail.size() << "\n";
        file << "    }";
        if (i + 1 < m_targetHistory.size()) file << ",";
        file << "\n";
    }
    file << "  ]\n";
    file << "}\n";
    file.close();

    log(LogLevel::INFO, "Exported full target history (" + std::to_string(m_targetHistory.size()) + " entries) to " + jsonPath);
    return true;
}

void Application::takeScreenshot() {
    int display_w, display_h;
    glfwGetFramebufferSize(m_window, &display_w, &display_h);

    // Read pixels from back buffer
    std::vector<uint8_t> pixels(3 * display_w * display_h);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, display_w, display_h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    // Flip vertically (OpenGL 0,0 is bottom-left, OpenCV is top-left)
    cv::Mat flipped(display_h, display_w, CV_8UC3, pixels.data());
    cv::Mat screenshot;
    cv::flip(flipped, screenshot, 0);
    cv::cvtColor(screenshot, screenshot, cv::COLOR_RGB2BGR);

    std::string baseDir = m_settings.exportOutputDir;
    if (baseDir.empty()) baseDir = ".";

    std::error_code ec;
    std::filesystem::create_directories(baseDir, ec);

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    struct tm buf;
    #ifdef _WIN32
    localtime_s(&buf, &in_time_t);
    #else
    localtime_r(&in_time_t, &buf);
    #endif
    char timebuf[64];
    std::strftime(timebuf, sizeof(timebuf), "%Y%m%d_%H%M%S", &buf);

    std::string path = baseDir + "/horus_screenshot_" + timebuf + ".png";
    if (cv::imwrite(path, screenshot)) {
        log(LogLevel::INFO, "Screenshot saved to " + path);
    } else {
        log(LogLevel::ERR, "Failed to save screenshot: " + path);
    }
}

void Application::renderTargetAnalyzer() {
    ImGui::Begin("Target Analyzer");

    if (m_selectedAnalyzerTargetId == -1) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No target selected for analysis.");
        ImGui::Text("Click a target in the video feed or target list to analyze it.");
        ImGui::End();
        return;
    }

    // Find target in history
    int targetIdx = -1;
    for (int i = 0; i < (int)m_targetHistory.size(); ++i) {
        if (m_targetHistory[i].track_id == m_selectedAnalyzerTargetId) {
            targetIdx = i;
            break;
        }
    }

    if (targetIdx == -1) {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Target ID %d not found in system history.", m_selectedAnalyzerTargetId);
        if (ImGui::Button("Clear Selection")) {
            m_selectedAnalyzerTargetId = -1;
        }
        ImGui::End();
        return;
    }

    auto& selectedRecord = m_targetHistory[targetIdx];
    auto& uiState = m_analyzerUiStates[selectedRecord.track_id];

    auto updateSharedRecord = [&](const std::function<void(UniqueTargetRecord&)>& func) {
        func(selectedRecord); // Update local copy in m_targetHistory immediately
        std::lock_guard<std::mutex> lock(m_dataMutex);
        auto it = std::find_if(m_sharedTargetHistory.begin(), m_sharedTargetHistory.end(),
                               [&](const UniqueTargetRecord& r) { return r.track_id == selectedRecord.track_id; });
        if (it != m_sharedTargetHistory.end()) {
            func(*it);
        }
    };

    // Manage active gallery snapshot selection
    static int lastSelectedTargetId = -1;
    static int activeSnapshotIdx = 1; // 0 = Discovery, 1 = Mid-Track (Best), 2 = Last Seen
    if (m_selectedAnalyzerTargetId != lastSelectedTargetId) {
        lastSelectedTargetId = m_selectedAnalyzerTargetId;
        activeSnapshotIdx = 1; // Default to Mid-Track (Best image first)
    }

    // Render target detail card header
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.5f, 1.0f), "TARGET REPORT: ID %03d", selectedRecord.track_id);
    ImGui::SameLine();
    ImGui::TextDisabled("| Class: %s (ID: %d)", selectedRecord.className.c_str(), selectedRecord.class_id);
    ImGui::Separator();

    // Visual Chronology Gallery
    ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.9f, 1.0f), "VISUAL ANALYZER");
    ImGui::SameLine();
    
    // Toggle Gallery Button
    if (ImGui::SmallButton(uiState.show_full_gallery ? "[ VIEW: SINGLE ]" : "[ VIEW: GALLERY ]")) {
        uiState.show_full_gallery = !uiState.show_full_gallery;
    }

    ImGui::Spacing();

    if (uiState.show_full_gallery) {
        // --- GALLERY VIEW (GRID) ---
        ImGui::TextDisabled("Select any image to set it as the 'Best Representative' snapshot.");
        ImGui::Spacing();

        float windowVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        auto& texInfo = m_targetTextures[selectedRecord.track_id];
        
        int cols = 3;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float imgWidth = (ImGui::GetContentRegionAvail().x - (cols - 1) * spacing) / cols;

        for (int i = 0; i < (int)selectedRecord.periodic_snapshots.size(); ++i) {
            uint32_t texID = 0;
            if (i < (int)texInfo.texture_ids_periodic.size()) {
                texID = texInfo.texture_ids_periodic[i];
            }

            if (texID != 0) {
                ImGui::PushID(i);
                
                bool isHighlighted = (uiState.selected_snapshot_idx == i);
                bool isActualBest  = (selectedRecord.snapshot_mid_manual_idx == i);

                if (isActualBest) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.0f, 1.0f)); // Green for BEST
                } else if (isHighlighted) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.4f, 0.8f, 1.0f)); // Blue for Highlight
                }

                if (ImGui::ImageButton("##gallery_item", reinterpret_cast<void*>(static_cast<intptr_t>(texID)), ImVec2(imgWidth, imgWidth * 0.75f))) {
                    uiState.selected_snapshot_idx = i;
                }

                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Set as Best Representative")) {
                        updateSharedRecord([&](UniqueTargetRecord& r) {
                            r.snapshot_mid_manual_idx = i;
                            if (i >= 0 && i < (int)r.periodic_snapshots.size()) {
                                r.snapshot_mid = r.periodic_snapshots[i];
                                r.cropped_image_mid_version++;
                            }
                        });
                        log(LogLevel::INFO, "Manually updated Best Representative for Target " + std::to_string(selectedRecord.track_id));
                    }
                    if (ImGui::MenuItem("Export Snapshot...")) {
                        const auto& snap = selectedRecord.periodic_snapshots[i];
                        if (!snap.image.empty()) {
                            std::string baseDir = "exports";
                            std::error_code ec;
                            std::filesystem::create_directories(baseDir, ec);
                            
                            std::string timestampClean = snap.timestamp;
                            std::replace(timestampClean.begin(), timestampClean.end(), ':', '-');
                            std::replace(timestampClean.begin(), timestampClean.end(), ' ', '_');

                            std::string path = baseDir + "/target_" + std::to_string(selectedRecord.track_id) + 
                                              "_snap_" + std::to_string(i) + "_" + timestampClean + ".jpg";

                            if (cv::imwrite(path, snap.image)) {
                                log(LogLevel::INFO, "Exported snapshot to " + path);
                            } else {
                                log(LogLevel::ERR, "Failed to export snapshot to " + path);
                            }
                        }
                    }
                    if (ImGui::MenuItem("Open Large View")) {
                        uiState.show_large_view_modal = true;
                        uiState.large_view_snapshot_idx = i;
                        uiState.large_view_tex_id = texID;
                    }
                    ImGui::EndPopup();
                }

                if (isActualBest || isHighlighted) {
                    ImGui::PopStyleColor();
                    // Draw selection border
                    ImU32 borderColor = isActualBest ? IM_COL32(0, 255, 100, 255) : IM_COL32(0, 150, 255, 255);
                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), borderColor, 0.0f, 0, 2.0f);
                }

                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Snapshot %d", i + 1);
                    ImGui::Text("Time: %s", selectedRecord.periodic_snapshots[i].timestamp.c_str());
                    ImGui::Text("Confidence: %.2f", selectedRecord.periodic_snapshots[i].confidence);
                    if (isActualBest) ImGui::TextColored(ImVec4(0, 1, 0, 1), "CURRENT BEST REPRESENTATIVE");
                    if (isHighlighted) ImGui::TextColored(ImVec4(0, 0.6f, 1, 1), "SELECTED");
                    ImGui::EndTooltip();
                }

                ImGui::PopID();

                if ((i + 1) % cols != 0) ImGui::SameLine();
            }
        }
        ImGui::NewLine();
        ImGui::Spacing();
    } else {
        // --- SINGLE VIEW (DETAILED) ---
        // Resolve active snapshot textures and label
        TargetSnapshot activeSnap;
        uint32_t activeTexID = 0;
        const char* snapLabel = "";
        ImVec4 badgeColor;

        uint32_t texFirst = 0, texMid = 0, texLast = 0;
        auto texIt = m_targetTextures.find(selectedRecord.track_id);
        if (texIt != m_targetTextures.end()) {
            texFirst = texIt->second.texture_id_first;
            texMid = texIt->second.texture_id_mid;
            texLast = texIt->second.texture_id_last;
        }

        if (activeSnapshotIdx == 0) {
            activeSnap = selectedRecord.snapshot_first;
            activeTexID = texFirst;
            snapLabel = "DISCOVERY (FIRST DETECTED)";
            badgeColor = ImVec4(0.0f, 0.7f, 1.0f, 1.0f); // Cyan
        } else if (activeSnapshotIdx == 1) {
            activeSnap = selectedRecord.snapshot_mid;
            activeTexID = texMid;
            snapLabel = (selectedRecord.snapshot_mid_manual_idx != -1) ? "MANUAL SELECTION (BEST)" : "AUTO SELECTION (BEST)";
            badgeColor = ImVec4(0.0f, 1.0f, 0.5f, 1.0f); // Green
        } else {
            activeSnap = selectedRecord.snapshot_last;
            activeTexID = texLast;
            snapLabel = "LAST SEEN (SIGNAL END)";
            badgeColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red
        }

        // Gallery navigation controls
        if (ImGui::Button("< Prev")) {
            activeSnapshotIdx = (activeSnapshotIdx - 1 + 3) % 3;
        }
        ImGui::SameLine();

        // Quick select buttons (tabs)
        auto drawTabButton = [&](const char* name, int idx) {
            bool selected = (activeSnapshotIdx == idx);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            }
            if (ImGui::Button(name)) {
                activeSnapshotIdx = idx;
            }
            if (selected) {
                ImGui::PopStyleColor();
            }
            ImGui::SameLine();
        };

        drawTabButton("Discovery", 0);
        drawTabButton("Best", 1);
        drawTabButton("Last", 2);

        if (ImGui::Button("Next >")) {
            activeSnapshotIdx = (activeSnapshotIdx + 1) % 3;
        }

        ImGui::Spacing();

        // Render active keyframe image
        if (activeTexID != 0 && !activeSnap.image.empty()) {
            float imgAspect = static_cast<float>(activeSnap.image.cols) / activeSnap.image.rows;
            float displayW = ImGui::GetContentRegionAvail().x * 0.9f;
            float displayH = displayW / imgAspect;

            // Display image centered in window
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - displayW) * 0.5f);
            ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(activeTexID)), ImVec2(displayW, displayH));
            ImGui::Spacing();
        } else {
            ImGui::TextDisabled("No visual crop available for this milestone.");
        }

        ImGui::Separator();

        // Snapshot Milestone Metadata
        ImGui::TextColored(badgeColor, "[MILESTONE: %s]", snapLabel);
        ImGui::Spacing();

        ImGui::Columns(2, "snapshot_metadata", false);
        ImGui::SetColumnWidth(0, 110.0f);

        ImGui::Text("Capture Time:");
        ImGui::NextColumn();
        if (!activeSnap.timestamp.empty()) {
            ImGui::Text("%s", activeSnap.timestamp.c_str());
        } else {
            ImGui::TextDisabled("[N/A]");
        }
        ImGui::NextColumn();

        ImGui::Text("Location Box:");
        ImGui::NextColumn();
        if (activeSnap.box.width > 0 && activeSnap.box.height > 0) {
            ImGui::Text("[%d, %d, %d, %d]", activeSnap.box.x, activeSnap.box.y, activeSnap.box.width, activeSnap.box.height);
            ImGui::TextDisabled("Area: %d px", activeSnap.box.width * activeSnap.box.height);
        } else {
            ImGui::TextDisabled("[N/A]");
        }
        ImGui::NextColumn();

        ImGui::Text("Confidence:");
        ImGui::NextColumn();
        if (activeSnap.confidence > 0.0f) {
            ImGui::Text("%.2f", activeSnap.confidence);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, badgeColor);
            ImGui::ProgressBar(activeSnap.confidence, ImVec2(100, 12), "");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("[N/A]");
        }
        ImGui::Columns(1);
    }

    ImGui::Separator();
    ImGui::Spacing();

    // General Target Lifetime Metadata
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Target Lifetime Metadata:");
    
    ImGui::Columns(2, "target_lifetime_metadata", false);
    ImGui::SetColumnWidth(0, 110.0f);

    ImGui::Text("Target ID:");
    ImGui::NextColumn();
    ImGui::Text("%d", selectedRecord.track_id);
    ImGui::NextColumn();

    ImGui::Text("Class Type:");
    ImGui::NextColumn();
    ImGui::Text("%s (ID: %d)", selectedRecord.className.c_str(), selectedRecord.class_id);
    ImGui::NextColumn();

    ImGui::Text("Current Status:");
    ImGui::NextColumn();
    if (selectedRecord.is_currently_active) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "ACTIVE / IN VIEW");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "LOST / ARCHIVED");
    }
    ImGui::NextColumn();

    ImGui::Text("First Seen:");
    ImGui::NextColumn();
    ImGui::Text("%s", selectedRecord.first_seen_timestamp.c_str());
    ImGui::NextColumn();

    ImGui::Text("Last Seen:");
    ImGui::NextColumn();
    ImGui::Text("%s", selectedRecord.last_seen_timestamp.c_str());
    ImGui::Columns(1);

    ImGui::Separator();

    if (selectedRecord.is_currently_active) {
        if (ImGui::Button("Update Visual Crop (Manual)", ImVec2(-FLT_MIN, 0))) {
            m_manualCaptureTargetId = selectedRecord.track_id;
        }
        ImGui::Spacing();
    }

    if (ImGui::Button("Export Target Details", ImVec2(-FLT_MIN, 0))) {
        exportTarget(selectedRecord);
    }

    if (ImGui::Button("Deselect", ImVec2(-FLT_MIN, 0))) {
        m_selectedAnalyzerTargetId = -1;
    }

    // Large View Modal (Plan 07 Extension)
    if (uiState.show_large_view_modal) {
        ImGui::OpenPopup("Gallery - Large View");
    }

    ImGui::SetNextWindowSize(ImVec2(800, 650), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal("Gallery - Large View", &uiState.show_large_view_modal)) {
        int idx = uiState.large_view_snapshot_idx;
        if (idx >= 0 && idx < (int)selectedRecord.periodic_snapshots.size()) {
            const auto& snap = selectedRecord.periodic_snapshots[idx];
            if (!snap.image.empty() && uiState.large_view_tex_id != 0) {
                float availW = ImGui::GetContentRegionAvail().x;
                float availH = ImGui::GetContentRegionAvail().y - 45; // Space for status and button
                
                float aspect = (float)snap.image.rows / (float)snap.image.cols;
                float imgW = availW;
                float imgH = imgW * aspect;
                if (imgH > availH) {
                    imgH = availH;
                    imgW = imgH / aspect;
                }

                ImGui::SetCursorPosX((availW - imgW) * 0.5f);
                ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(uiState.large_view_tex_id)), ImVec2(imgW, imgH));
                
                ImGui::TextDisabled("Snapshot %d | Time: %s | Confidence: %.2f", idx + 1, snap.timestamp.c_str(), snap.confidence);
            }
        }

        ImGui::Separator();
        if (ImGui::Button("CLOSE", ImVec2(-FLT_MIN, 0))) {
            uiState.show_large_view_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void Application::cleanup() {
    m_running = false;

    // Wake up threads to let them terminate
    {
        std::lock_guard<std::mutex> lk(m_detectorMutex);
        m_detectorCv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(m_trackingMutex);
        m_trackingCv.notify_all();
    }

    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_detectorThread.joinable()) m_detectorThread.join();
    if (m_trackingThread.joinable()) m_trackingThread.join();
    if (m_workerThread.joinable()) m_workerThread.join();

    curl_global_cleanup();

    for (int i = 0; i < 4; ++i) {
        m_subZoomRenderers[i].reset();
    }

    // Delete target history textures
    for (auto& [id, texInfo] : m_targetTextures) {
        if (texInfo.texture_id_first != 0) {
            glDeleteTextures(1, &texInfo.texture_id_first);
        }
        if (texInfo.texture_id_mid != 0) {
            glDeleteTextures(1, &texInfo.texture_id_mid);
        }
        if (texInfo.texture_id_last != 0) {
            glDeleteTextures(1, &texInfo.texture_id_last);
        }
        for (uint32_t tid : texInfo.texture_ids_periodic) {
            if (tid != 0) glDeleteTextures(1, &tid);
        }
    }
    m_targetTextures.clear();

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
