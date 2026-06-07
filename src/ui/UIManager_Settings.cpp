#include "UIManager.hpp"
#include "UIManager_internal.hpp"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cstring>
#include <opencv2/opencv.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// File-local helpers
// ─────────────────────────────────────────────────────────────────────────────

static SystemSettings MakeStandardSettings() {
    SystemSettings s;
    s.hudColor    = kDefaultHudColor;
    s.targetColor = kDefaultTargetColor;
    return s;
}

static std::string GetDefaultSettingsPath() {
    const char* home = std::getenv("HOME");
    std::filesystem::path base = home ? home : ".";
    base /= ".tactileviewer";
    return (base / "settings.ini").string();
}

static bool ParseBoolSetting(const std::string& v) {
    std::string lv = v;
    std::transform(lv.begin(), lv.end(), lv.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return lv == "1" || lv == "true" || lv == "yes" || lv == "on";
}

static std::set<int> ParsePriorityClasses(const std::string& v) {
    std::set<int> out;
    std::stringstream ss(v);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        try { out.insert(std::stoi(tok)); } catch(...) {}
    }
    return out;
}

static std::string SerializePriorityClasses(const std::set<int>& classes) {
    std::ostringstream os;
    bool first = true;
    for (int id : classes) {
        if (!first) os << ',';
        os << id;
        first = false;
    }
    return os.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// syncColorEditorsFromSettings
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::syncColorEditorsFromSettings() {
    ImU32 hud    = m_settings.hudColor          ? m_settings.hudColor    : kDefaultHudColor;
    ImU32 target = m_settings.targetColor       ? m_settings.targetColor : kDefaultTargetColor;
    ImU32 motion = m_settings.motionOverlayColor? m_settings.motionOverlayColor
                                                : kDefaultMotionColor;
    ImU32ToFloat4(hud,    m_hudColorF);
    ImU32ToFloat4(target, m_targetColorF);
    ImU32ToFloat4(motion, m_motionOverlayColorF);
    m_settings.hudColor           = hud;
    m_settings.targetColor        = target;
    m_settings.motionOverlayColor = motion;
}

// ─────────────────────────────────────────────────────────────────────────────
// savePersistedSettings
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::savePersistedSettings() const {
    std::filesystem::path p(m_settingsPath.empty() ? GetDefaultSettingsPath() : m_settingsPath);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);

    std::ofstream out(p);
    if (!out) {
        std::cerr << "[WARN] Could not save settings to " << p.string() << std::endl;
        return;
    }

    out << "# Tactileviewer persistent settings\n";
    out << "cameraAddress="       << (m_cameraAddress.empty() ? "1" : m_cameraAddress) << '\n';
    out << "showSettingsWindow="  << (m_showSettingsWindow ? 1 : 0) << '\n';
    out << "showDevConsole="      << (m_showDevConsole ? 1 : 0) << '\n';
    out << "showDataPanel="       << (m_showDataPanel ? 1 : 0) << '\n';
    out << "showZoomWindow="      << (m_showZoomWindow ? 1 : 0) << '\n';
    out << "showTargetAnalyzer="  << (m_showTargetAnalyzer ? 1 : 0) << '\n';

    const auto& s = m_settings;
    out << "detectorModel="       << s.detectorModel << '\n';
    out << "remoteInferenceEnabled=" << (s.remoteInferenceEnabled ? 1 : 0) << '\n';
    out << "remoteInferenceIp="      << s.remoteInferenceIp << '\n';
    out << "remoteInferencePort="    << s.remoteInferencePort << '\n';
    out << "detectorConfThreshold="  << s.detectorConfThreshold << '\n';
    out << "detectorScoreThreshold=" << s.detectorScoreThreshold << '\n';
    out << "detectorNmsThreshold="   << s.detectorNmsThreshold << '\n';
    out << "filterByPriorityClasses="<< (s.filterByPriorityClasses ? 1 : 0) << '\n';
    out << "priorityClasses="        << SerializePriorityClasses(s.priorityClasses) << '\n';
    out << "trackerMaxLostFrames="   << s.trackerMaxLostFrames << '\n';
    out << "trackerMinMatchIOU="     << s.trackerMinMatchIOU << '\n';
    out << "trackerMaxTrailLength="  << s.trackerMaxTrailLength << '\n';
    out << "showTrails="             << (s.showTrails ? 1 : 0) << '\n';
    out << "trackerMinMatchScore="   << s.trackerMinMatchScore << '\n';
    out << "trackerMaxCenterDistPx=" << s.trackerMaxCenterDistPx << '\n';
    out << "trackerReacquisitionEnabled="  << (s.trackerReacquisitionEnabled ? 1 : 0) << '\n';
    out << "trackerReacquisitionZoom="     << s.trackerReacquisitionZoom << '\n';
    out << "trackerUseMotionFallback="     << (s.trackerUseMotionFallback ? 1 : 0) << '\n';
    out << "trackerReacquisitionMaxDist="  << s.trackerReacquisitionMaxDist << '\n';
    out << "trackerConfirmFrames="         << s.trackerConfirmFrames << '\n';
    out << "trackerVelocitySmoothing="     << s.trackerVelocitySmoothing << '\n';
    out << "trackerDeadReckoningDamping="  << s.trackerDeadReckoningDamping << '\n';
    out << "showTacticalOverlay="  << (s.showTacticalOverlay ? 1 : 0) << '\n';
    out << "showCrosshair="        << (s.showCrosshair ? 1 : 0) << '\n';
    out << "showCornerBrackets="   << (s.showCornerBrackets ? 1 : 0) << '\n';
    out << "showStatusWindows="    << (s.showStatusWindows ? 1 : 0) << '\n';
    out << "showDetections="       << (s.showDetections ? 1 : 0) << '\n';
    out << "showTrackIDs="         << (s.showTrackIDs ? 1 : 0) << '\n';
    out << "showConfidence="       << (s.showConfidence ? 1 : 0) << '\n';
    out << "showTrailFade="        << (s.showTrailFade ? 1 : 0) << '\n';
    out << "hudBrightness="        << s.hudBrightness << '\n';
    out << "crosshairScale="       << s.crosshairScale << '\n';
    out << "boxLineWidth="         << s.boxLineWidth << '\n';
    out << "trailLineWidth="       << s.trailLineWidth << '\n';
    out << "hudColor="             << s.hudColor << '\n';
    out << "targetColor="          << s.targetColor << '\n';
    out << "enableDetection="      << (s.enableDetection ? 1 : 0) << '\n';
    out << "enableTracking="       << (s.enableTracking ? 1 : 0) << '\n';
    out << "detectionSkipFrames="  << s.detectionSkipFrames << '\n';
    out << "grayscaleInput="       << (s.grayscaleInput ? 1 : 0) << '\n';
    out << "logLevel="             << s.logLevel << '\n';
    out << "logToFile="            << (s.logToFile ? 1 : 0) << '\n';
    out << "dataLoggingEnabled="   << (s.dataLoggingEnabled ? 1 : 0) << '\n';
    out << "dataLoggingFormat="    << s.dataLoggingFormat << '\n';
    out << "dataLoggingFreqFrames="<< s.dataLoggingFreqFrames << '\n';
    out << "dataLoggingOutputDir=" << s.dataLoggingOutputDir << '\n';
    out << "exportOutputDir="      << s.exportOutputDir << '\n';
    out << "showROIOverlay="       << (s.showROIOverlay ? 1 : 0) << '\n';
    out << "request4KCamera="      << (s.request4KCamera ? 1 : 0) << '\n';
    out << "enable4KZoom="         << (s.enable4KZoom ? 1 : 0) << '\n';
    out << "targetZoomMagnification=" << s.targetZoomMagnification << '\n';
    out << "lowLightEnhancement="  << (s.lowLightEnhancement ? 1 : 0) << '\n';
    out << "lowLightClipLimit="    << s.lowLightClipLimit << '\n';
    out << "lowLightDenoiseKernel="<< s.lowLightDenoiseKernel << '\n';
    out << "motionDetectionEnabled="  << (s.motionDetectionEnabled ? 1 : 0) << '\n';
    out << "motionShowOverlay="    << (s.motionShowOverlay ? 1 : 0) << '\n';
    out << "motionHeatmapOverlay=" << (s.motionHeatmapOverlay ? 1 : 0) << '\n';
    out << "motionHeatmapDecay="   << s.motionHeatmapDecay << '\n';
    out << "motionHeatmapSensitivity=" << s.motionHeatmapSensitivity << '\n';
    out << "motionHeatmapAlpha="   << s.motionHeatmapAlpha << '\n';
    out << "motionSensitivity="    << s.motionSensitivity << '\n';
    out << "motionMinArea="        << s.motionMinArea << '\n';
    out << "motionBlurKernel="     << s.motionBlurKernel << '\n';
    out << "motionOverlayAlpha="   << s.motionOverlayAlpha << '\n';
    out << "motionOverlayColor="   << s.motionOverlayColor << '\n';
    out << "motionDetectShadows="  << (s.motionDetectShadows ? 1 : 0) << '\n';
    out << "motionLearningRate="   << s.motionLearningRate << '\n';
    out << "motionTrackHoldDuration=" << s.motionTrackHoldDuration << '\n';
    out << "subZoomsEnabled="         << (s.subZoomsEnabled ? 1 : 0) << '\n';
    out << "subZoomsUseSeparateWindows=" << (s.subZoomsUseSeparateWindows ? 1 : 0) << '\n';
    out << "subZoomPaddingPx="     << s.subZoomPaddingPx << '\n';
    out << "subZoomMagnification=" << s.subZoomMagnification << '\n';
    out << "faceRecognitionEnabled="   << (s.faceRecognitionEnabled ? 1 : 0) << '\n';
    out << "faceRecognitionThreshold=" << s.faceRecognitionThreshold << '\n';
    out << "debugShowRawDetections="   << (s.debugShowRawDetections ? 1 : 0) << '\n';
    out << "debugShowKalmanVectors="    << (s.debugShowKalmanVectors ? 1 : 0) << '\n';
    out << "debugFreezeVision="         << (s.debugFreezeVision ? 1 : 0) << '\n';
    out << "debugPerformanceGraphs="    << (s.debugPerformanceGraphs ? 1 : 0) << '\n';

    out << "audioEnabled="         << (s.audioEnabled ? 1 : 0) << '\n';
    out << "audioMasterVolume="    << s.audioMasterVolume << '\n';
    out << "audioMotionEnabled="   << (s.audioMotionEnabled ? 1 : 0) << '\n';
    out << "audioMotionFreqHz="    << s.audioMotionFreqHz << '\n';
    out << "audioMotionDurationMs="<< s.audioMotionDurationMs << '\n';
    out << "audioMotionCooldownSec="<< s.audioMotionCooldownSec << '\n';
    out << "audioAlarmEntryEnabled="   << (s.audioAlarmEntryEnabled ? 1 : 0) << '\n';
    out << "audioAlarmEntryFreqHz="    << s.audioAlarmEntryFreqHz << '\n';
    out << "audioAlarmEntryDurMs="     << s.audioAlarmEntryDurMs << '\n';
    out << "audioAlarmExitEnabled="    << (s.audioAlarmExitEnabled ? 1 : 0) << '\n';
    out << "audioAlarmExitFreqHz="     << s.audioAlarmExitFreqHz << '\n';
    out << "audioAlarmExitDurMs="      << s.audioAlarmExitDurMs << '\n';
    out << "audioLockAcquiredEnabled=" << (s.audioLockAcquiredEnabled ? 1 : 0) << '\n';
    out << "audioLockAcquiredFreqHz="  << s.audioLockAcquiredFreqHz << '\n';
    out << "audioLockAcquiredDurMs="   << s.audioLockAcquiredDurMs << '\n';
    out << "audioLockLostEnabled="     << (s.audioLockLostEnabled ? 1 : 0) << '\n';
    out << "audioLockLostFreqHz="      << s.audioLockLostFreqHz << '\n';
    out << "audioLockLostDurMs="       << s.audioLockLostDurMs << '\n';
    out << "audioLockPulseEnabled="          << (s.audioLockPulseEnabled ? 1 : 0) << '\n';
    out << "audioLockPulseFreqHz="           << s.audioLockPulseFreqHz << '\n';
    out << "audioLockPulseDurMs="            << s.audioLockPulseDurMs << '\n';
    out << "audioLockPulseMinIntervalMs="    << s.audioLockPulseMinIntervalMs << '\n';
    out << "audioLockPulseMaxIntervalMs="    << s.audioLockPulseMaxIntervalMs << '\n';
    out << "audioLockPulseSolutionThresh="   << s.audioLockPulseSolutionThresh << '\n';
    out << "audioLockPulseSolutionFreqHz="   << s.audioLockPulseSolutionFreqHz << '\n';
    out << "audioLockPulseSolutionDurMs="    << s.audioLockPulseSolutionDurMs << '\n';

    // ── ROI zones ─────────────────────────────────────────────────────────
    auto zones = m_roiManager.getROIs();
    out << "roi_count=" << zones.size() << '\n';
    for (int i = 0; i < static_cast<int>(zones.size()); ++i) {
        const auto& z = zones[i];
        out << "roi_" << i << "_rect="     << z.rect.x << ',' << z.rect.y << ','
                                           << z.rect.width << ',' << z.rect.height << '\n';
        out << "roi_" << i << "_label="    << z.label << '\n';
        out << "roi_" << i << "_active="   << (z.active ? 1 : 0) << '\n';
        out << "roi_" << i << "_function=" << static_cast<int>(z.function) << '\n';
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// loadPersistedSettings
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::loadPersistedSettings(const std::string& path) {
    if (!path.empty()) const_cast<std::string&>(m_settingsPath) = path;

    m_settings = MakeStandardSettings();
    syncColorEditorsFromSettings();

    std::filesystem::path p(m_settingsPath.empty() ? GetDefaultSettingsPath() : m_settingsPath);
    std::ifstream in(p);
    if (!in) {
        if (m_cameraAddress.empty()) m_cameraAddress = "1";
        std::filesystem::create_directories(p.parent_path());
        savePersistedSettings();
        return;
    }

    // Accumulate ROI data for deferred zone construction
    struct PendingROI { cv::Rect rect; std::string label; bool active = true; ROIFunction func = ROIFunction::DETECTION; };
    int roiCount = 0;
    std::vector<PendingROI> pendingROIs;

    auto& s = m_settings;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto sep = line.find('=');
        if (sep == std::string::npos) continue;
        const std::string key = line.substr(0, sep);
        const std::string val = line.substr(sep + 1);
        try {
            if      (key == "cameraAddress")             m_cameraAddress                 = val;
            else if (key == "detectorModel")             s.detectorModel                 = std::stoi(val);
            else if (key == "showSettingsWindow")         m_showSettingsWindow            = ParseBoolSetting(val);
            else if (key == "showDevConsole")             m_showDevConsole                = ParseBoolSetting(val);
            else if (key == "showDataPanel")              m_showDataPanel                 = ParseBoolSetting(val);
            else if (key == "showZoomWindow")             m_showZoomWindow                = ParseBoolSetting(val);
            else if (key == "showTargetAnalyzer")         m_showTargetAnalyzer            = ParseBoolSetting(val);
            else if (key == "remoteInferenceEnabled")     s.remoteInferenceEnabled        = ParseBoolSetting(val);
            else if (key == "remoteInferenceIp")          s.remoteInferenceIp             = val;
            else if (key == "remoteInferencePort")        s.remoteInferencePort           = std::stoi(val);
            else if (key == "detectorConfThreshold")      s.detectorConfThreshold         = std::stof(val);
            else if (key == "detectorScoreThreshold")     s.detectorScoreThreshold        = std::stof(val);
            else if (key == "detectorNmsThreshold")       s.detectorNmsThreshold          = std::stof(val);
            else if (key == "filterByPriorityClasses")    s.filterByPriorityClasses       = ParseBoolSetting(val);
            else if (key == "priorityClasses")            s.priorityClasses               = ParsePriorityClasses(val);
            else if (key == "trackerMaxLostFrames")       s.trackerMaxLostFrames          = std::stoi(val);
            else if (key == "trackerMinMatchIOU")         s.trackerMinMatchIOU            = std::stof(val);
            else if (key == "trackerMaxTrailLength")      s.trackerMaxTrailLength         = std::stoi(val);
            else if (key == "showTrails")                 s.showTrails                    = ParseBoolSetting(val);
            else if (key == "trackerMinMatchScore")       s.trackerMinMatchScore          = std::stof(val);
            else if (key == "trackerMaxCenterDistPx")     s.trackerMaxCenterDistPx        = std::stof(val);
            else if (key == "trackerReacquisitionEnabled")s.trackerReacquisitionEnabled   = ParseBoolSetting(val);
            else if (key == "trackerReacquisitionZoom")   s.trackerReacquisitionZoom      = std::stof(val);
            else if (key == "trackerUseMotionFallback")   s.trackerUseMotionFallback      = ParseBoolSetting(val);
            else if (key == "trackerReacquisitionMaxDist")s.trackerReacquisitionMaxDist   = std::stof(val);
            else if (key == "trackerConfirmFrames")       s.trackerConfirmFrames          = std::stoi(val);
            else if (key == "trackerVelocitySmoothing")   s.trackerVelocitySmoothing      = std::stof(val);
            else if (key == "trackerDeadReckoningDamping")s.trackerDeadReckoningDamping   = std::stof(val);
            else if (key == "showTacticalOverlay")        s.showTacticalOverlay           = ParseBoolSetting(val);
            else if (key == "showCrosshair")              s.showCrosshair                 = ParseBoolSetting(val);
            else if (key == "showCornerBrackets")         s.showCornerBrackets            = ParseBoolSetting(val);
            else if (key == "showStatusWindows")          s.showStatusWindows             = ParseBoolSetting(val);
            else if (key == "showDetections")             s.showDetections                = ParseBoolSetting(val);
            else if (key == "showTrackIDs")               s.showTrackIDs                  = ParseBoolSetting(val);
            else if (key == "showConfidence")             s.showConfidence                = ParseBoolSetting(val);
            else if (key == "showTrailFade")              s.showTrailFade                 = ParseBoolSetting(val);
            else if (key == "hudBrightness")              s.hudBrightness                 = std::stof(val);
            else if (key == "crosshairScale")             s.crosshairScale                = std::stof(val);
            else if (key == "boxLineWidth")               s.boxLineWidth                  = std::stof(val);
            else if (key == "trailLineWidth")             s.trailLineWidth                = std::stof(val);
            else if (key == "hudColor")                   s.hudColor    = static_cast<uint32_t>(std::stoul(val));
            else if (key == "targetColor")                s.targetColor = static_cast<uint32_t>(std::stoul(val));
            else if (key == "enableDetection")            s.enableDetection               = ParseBoolSetting(val);
            else if (key == "enableTracking")             s.enableTracking                = ParseBoolSetting(val);
            else if (key == "detectionSkipFrames")        s.detectionSkipFrames           = std::stoi(val);
            else if (key == "grayscaleInput")             s.grayscaleInput                = ParseBoolSetting(val);
            else if (key == "logLevel")                   s.logLevel                      = std::stoi(val);
            else if (key == "logToFile")                  s.logToFile                     = ParseBoolSetting(val);
            else if (key == "dataLoggingEnabled")         s.dataLoggingEnabled            = ParseBoolSetting(val);
            else if (key == "dataLoggingFormat")          s.dataLoggingFormat             = std::stoi(val);
            else if (key == "dataLoggingFreqFrames")      s.dataLoggingFreqFrames         = std::stoi(val);
            else if (key == "dataLoggingOutputDir")       s.dataLoggingOutputDir          = val;
            else if (key == "exportOutputDir")            s.exportOutputDir               = val;
            else if (key == "showROIOverlay")             s.showROIOverlay                = ParseBoolSetting(val);
            else if (key == "request4KCamera")            s.request4KCamera               = ParseBoolSetting(val);
            else if (key == "enable4KZoom")               s.enable4KZoom                  = ParseBoolSetting(val);
            else if (key == "targetZoomMagnification")    s.targetZoomMagnification       = std::stof(val);
            else if (key == "lowLightEnhancement")        s.lowLightEnhancement           = ParseBoolSetting(val);
            else if (key == "lowLightClipLimit")          s.lowLightClipLimit             = std::stof(val);
            else if (key == "lowLightDenoiseKernel")      s.lowLightDenoiseKernel         = std::stoi(val);
            else if (key == "motionDetectionEnabled")     s.motionDetectionEnabled        = ParseBoolSetting(val);
            else if (key == "motionShowOverlay")          s.motionShowOverlay             = ParseBoolSetting(val);
            else if (key == "motionHeatmapOverlay")       s.motionHeatmapOverlay          = ParseBoolSetting(val);
            else if (key == "motionHeatmapDecay")         s.motionHeatmapDecay            = std::stof(val);
            else if (key == "motionHeatmapSensitivity")   s.motionHeatmapSensitivity      = std::stof(val);
            else if (key == "motionHeatmapAlpha")         s.motionHeatmapAlpha            = std::stof(val);
            else if (key == "motionSensitivity")          s.motionSensitivity             = std::stof(val);
            else if (key == "motionMinArea")              s.motionMinArea                 = std::stoi(val);
            else if (key == "motionBlurKernel")           s.motionBlurKernel              = std::stoi(val);
            else if (key == "motionOverlayAlpha")         s.motionOverlayAlpha            = std::stof(val);
            else if (key == "motionOverlayColor")         s.motionOverlayColor = static_cast<uint32_t>(std::stoul(val));
            else if (key == "motionDetectShadows")        s.motionDetectShadows           = ParseBoolSetting(val);
            else if (key == "motionLearningRate")         s.motionLearningRate            = std::stoi(val);
            else if (key == "motionTrackHoldDuration")    s.motionTrackHoldDuration       = std::stof(val);
            else if (key == "subZoomsEnabled")            s.subZoomsEnabled               = ParseBoolSetting(val);
            else if (key == "subZoomsUseSeparateWindows") s.subZoomsUseSeparateWindows    = ParseBoolSetting(val);
            else if (key == "subZoomPaddingPx")           s.subZoomPaddingPx              = std::stoi(val);
            else if (key == "subZoomMagnification")       s.subZoomMagnification          = std::stof(val);
            else if (key == "faceRecognitionEnabled")     s.faceRecognitionEnabled        = ParseBoolSetting(val);
            else if (key == "faceRecognitionThreshold")   s.faceRecognitionThreshold      = std::stof(val);
            else if (key == "debugShowRawDetections")     s.debugShowRawDetections        = ParseBoolSetting(val);
            else if (key == "debugShowKalmanVectors")      s.debugShowKalmanVectors         = ParseBoolSetting(val);
            else if (key == "debugFreezeVision")           s.debugFreezeVision              = ParseBoolSetting(val);
            else if (key == "debugPerformanceGraphs")      s.debugPerformanceGraphs         = ParseBoolSetting(val);
            else if (key == "audioEnabled")               s.audioEnabled                  = ParseBoolSetting(val);
            else if (key == "audioMasterVolume")          s.audioMasterVolume             = std::stof(val);
            else if (key == "audioMotionEnabled")         s.audioMotionEnabled            = ParseBoolSetting(val);
            else if (key == "audioMotionFreqHz")          s.audioMotionFreqHz             = std::stof(val);
            else if (key == "audioMotionDurationMs")      s.audioMotionDurationMs         = std::stof(val);
            else if (key == "audioMotionCooldownSec")     s.audioMotionCooldownSec        = std::stof(val);
            else if (key == "audioAlarmEntryEnabled")     s.audioAlarmEntryEnabled        = ParseBoolSetting(val);
            else if (key == "audioAlarmEntryFreqHz")      s.audioAlarmEntryFreqHz         = std::stof(val);
            else if (key == "audioAlarmEntryDurMs")       s.audioAlarmEntryDurMs          = std::stof(val);
            else if (key == "audioAlarmExitEnabled")      s.audioAlarmExitEnabled         = ParseBoolSetting(val);
            else if (key == "audioAlarmExitFreqHz")       s.audioAlarmExitFreqHz          = std::stof(val);
            else if (key == "audioAlarmExitDurMs")        s.audioAlarmExitDurMs           = std::stof(val);
            else if (key == "audioLockAcquiredEnabled")   s.audioLockAcquiredEnabled      = ParseBoolSetting(val);
            else if (key == "audioLockAcquiredFreqHz")    s.audioLockAcquiredFreqHz       = std::stof(val);
            else if (key == "audioLockAcquiredDurMs")     s.audioLockAcquiredDurMs        = std::stof(val);
            else if (key == "audioLockLostEnabled")          s.audioLockLostEnabled             = ParseBoolSetting(val);
            else if (key == "audioLockLostFreqHz")           s.audioLockLostFreqHz              = std::stof(val);
            else if (key == "audioLockLostDurMs")            s.audioLockLostDurMs               = std::stof(val);
            else if (key == "audioLockPulseEnabled")         s.audioLockPulseEnabled            = ParseBoolSetting(val);
            else if (key == "audioLockPulseFreqHz")          s.audioLockPulseFreqHz             = std::stof(val);
            else if (key == "audioLockPulseDurMs")           s.audioLockPulseDurMs              = std::stof(val);
            else if (key == "audioLockPulseMinIntervalMs")   s.audioLockPulseMinIntervalMs      = std::stof(val);
            else if (key == "audioLockPulseMaxIntervalMs")   s.audioLockPulseMaxIntervalMs      = std::stof(val);
            else if (key == "audioLockPulseSolutionThresh")  s.audioLockPulseSolutionThresh     = std::stof(val);
            else if (key == "audioLockPulseSolutionFreqHz")  s.audioLockPulseSolutionFreqHz     = std::stof(val);
            else if (key == "audioLockPulseSolutionDurMs")   s.audioLockPulseSolutionDurMs      = std::stof(val);
            else if (key == "roi_count") {
                roiCount = std::stoi(val);
                pendingROIs.resize(static_cast<size_t>(roiCount));
            } else if (key.substr(0, 4) == "roi_") {
                // roi_N_rect / roi_N_label / roi_N_active / roi_N_function
                auto us1 = key.find('_', 4);
                if (us1 != std::string::npos) {
                    int idx = std::stoi(key.substr(4, us1 - 4));
                    std::string field = key.substr(us1 + 1);
                    if (idx >= 0 && idx < static_cast<int>(pendingROIs.size())) {
                        if (field == "rect") {
                            std::stringstream ss(val); std::string tok;
                            int x=0,y=0,w=0,h=0;
                            if (std::getline(ss,tok,',')) x=std::stoi(tok);
                            if (std::getline(ss,tok,',')) y=std::stoi(tok);
                            if (std::getline(ss,tok,',')) w=std::stoi(tok);
                            if (std::getline(ss,tok,',')) h=std::stoi(tok);
                            pendingROIs[idx].rect = cv::Rect(x,y,w,h);
                        } else if (field == "label")    pendingROIs[idx].label  = val;
                        else if (field == "active")     pendingROIs[idx].active = ParseBoolSetting(val);
                        else if (field == "function")   pendingROIs[idx].func   = static_cast<ROIFunction>(std::stoi(val));
                    }
                }
            }
        } catch (...) {
            std::cerr << "[WARN] Ignoring invalid settings entry: " << key << std::endl;
        }
    }
    if (m_cameraAddress.empty()) m_cameraAddress = "1";
    syncColorEditorsFromSettings();

    // Restore ROI zones
    if (!pendingROIs.empty()) {
        m_roiManager.clearAll();
        for (const auto& pr : pendingROIs) {
            int id = m_roiManager.addROI(pr.rect, pr.label);
            if (id >= 0) {
                m_roiManager.setFunction(id, pr.func);
                if (!pr.active) m_roiManager.toggleROI(id);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Presets
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::applyStandardPreset() {
    m_settings = MakeStandardSettings();
    syncColorEditorsFromSettings();
    pushSettingsToBlackboard();
    savePersistedSettings();
    m_log(LogLevel::INFO, "Settings preset applied: Standard");
}

void UIManager::applyPresetPerformance() {
    m_settings = MakeStandardSettings();
    m_settings.request4KCamera    = false;
    m_settings.enable4KZoom       = false;
    m_settings.grayscaleInput     = true;
    m_settings.detectionSkipFrames= 2;
    m_settings.lowLightEnhancement= false;
    m_settings.showTrails         = false;
    syncColorEditorsFromSettings();
    pushSettingsToBlackboard();
    savePersistedSettings();
    m_log(LogLevel::INFO, "Settings preset applied: Performance");
}

void UIManager::applyPresetBalanced() {
    m_settings = MakeStandardSettings();
    m_settings.request4KCamera     = true;
    m_settings.enable4KZoom        = true;
    m_settings.grayscaleInput      = false;
    m_settings.detectionSkipFrames = 0;
    m_settings.lowLightEnhancement = false;
    m_settings.showTrails          = true;
    m_settings.showStatusWindows   = true;
    syncColorEditorsFromSettings();
    pushSettingsToBlackboard();
    savePersistedSettings();
    m_log(LogLevel::INFO, "Settings preset applied: Balanced");
}

void UIManager::applyPresetPrecision() {
    m_settings = MakeStandardSettings();
    m_settings.request4KCamera         = true;
    m_settings.enable4KZoom            = true;
    m_settings.detectorConfThreshold   = 0.10f;
    m_settings.detectorScoreThreshold  = 0.10f;
    m_settings.detectorNmsThreshold    = 0.35f;
    m_settings.detectionSkipFrames     = 0;
    m_settings.lowLightEnhancement     = true;
    m_settings.lowLightClipLimit       = 3.5f;
    m_settings.lowLightDenoiseKernel   = 3;
    m_settings.showTrails              = true;
    syncColorEditorsFromSettings();
    pushSettingsToBlackboard();
    savePersistedSettings();
    m_log(LogLevel::INFO, "Settings preset applied: Precision");
}

void UIManager::applyPresetLowLight() {
    m_settings = MakeStandardSettings();
    m_settings.request4KCamera       = true;
    m_settings.enable4KZoom          = true;
    m_settings.lowLightEnhancement   = true;
    m_settings.lowLightClipLimit     = 4.5f;
    m_settings.lowLightDenoiseKernel = 5;
    m_settings.grayscaleInput        = false;
    m_settings.detectionSkipFrames   = 0;
    m_settings.showTrails            = true;
    syncColorEditorsFromSettings();
    pushSettingsToBlackboard();
    savePersistedSettings();
    m_log(LogLevel::INFO, "Settings preset applied: Low Light");
}
