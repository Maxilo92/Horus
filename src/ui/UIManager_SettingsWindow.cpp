#include "UIManager.hpp"
#include "UIManager_internal.hpp"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// renderSettingsWindow
// Source reference: Application.cpp lines 3095-3396
// API substitutions:
//   m_sharedSettings = m_settings → pushSettingsToBlackboard()
//   log(...)                      → m_log(...)
//   glfwSetWindowShouldClose      → unchanged (m_window)
// ─────────────────────────────────────────────────────────────────────────────

namespace {
AudioEngine::Config MakeAudioConfig(const SystemSettings& s) {
    AudioEngine::Config c;
    c.masterEnabled       = s.audioEnabled;
    c.masterVolume        = s.audioMasterVolume;
    c.motionEnabled       = s.audioMotionEnabled;
    c.motionFreqHz        = s.audioMotionFreqHz;
    c.motionDurationMs    = s.audioMotionDurationMs;
    c.motionCooldownSec   = s.audioMotionCooldownSec;
    c.alarmEntryEnabled   = s.audioAlarmEntryEnabled;
    c.alarmEntryFreqHz    = s.audioAlarmEntryFreqHz;
    c.alarmEntryDurMs     = s.audioAlarmEntryDurMs;
    c.alarmExitEnabled    = s.audioAlarmExitEnabled;
    c.alarmExitFreqHz     = s.audioAlarmExitFreqHz;
    c.alarmExitDurMs      = s.audioAlarmExitDurMs;
    c.lockAcquiredEnabled = s.audioLockAcquiredEnabled;
    c.lockAcquiredFreqHz  = s.audioLockAcquiredFreqHz;
    c.lockAcquiredDurMs   = s.audioLockAcquiredDurMs;
    c.lockLostEnabled            = s.audioLockLostEnabled;
    c.lockLostFreqHz             = s.audioLockLostFreqHz;
    c.lockLostDurMs              = s.audioLockLostDurMs;
    c.lockPulseEnabled           = s.audioLockPulseEnabled;
    c.lockPulseFreqHz            = s.audioLockPulseFreqHz;
    c.lockPulseDurMs             = s.audioLockPulseDurMs;
    c.lockPulseMinIntervalMs     = s.audioLockPulseMinIntervalMs;
    c.lockPulseMaxIntervalMs     = s.audioLockPulseMaxIntervalMs;
    c.lockPulseSolutionThresh    = s.audioLockPulseSolutionThresh;
    c.lockPulseSolutionFreqHz    = s.audioLockPulseSolutionFreqHz;
    c.lockPulseSolutionDurMs     = s.audioLockPulseSolutionDurMs;
    return c;
}
} // namespace

void UIManager::renderSettingsWindow() {
    if (!m_showSettingsWindow) return;

    ImGui::SetNextWindowSize(ImVec2(480, 560), ImGuiCond_FirstUseEver);
    bool prevState = m_showSettingsWindow;
    if (!ImGui::Begin("Settings", &m_showSettingsWindow)) {
        ImGui::End();
        if (prevState != m_showSettingsWindow) savePersistedSettings();
        return;
    }
    if (prevState != m_showSettingsWindow) savePersistedSettings();

    bool changed      = false;
    bool audioChanged = false;

    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.5f, 1.0f), "APPLICATION SETTINGS");
    ImGui::Separator();

    if (ImGui::BeginTabBar("SettingsTabBar")) {

        // ── Display & HUD ────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Display & HUD")) {
            ImGui::Spacing();
            changed |= ImGui::SliderFloat("HUD Brightness##s",
                &m_settings.hudBrightness, 0.2f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Crosshair Scale##s",
                &m_settings.crosshairScale, 0.25f, 3.0f, "%.2f");
            changed |= ImGui::SliderFloat("Box Line Width##s",
                &m_settings.boxLineWidth, 0.5f, 5.0f, "%.1f");
            changed |= ImGui::SliderFloat("Trail Line Width##s",
                &m_settings.trailLineWidth, 0.5f, 5.0f, "%.1f");
            if (ImGui::ColorEdit4("HUD Color##s", m_hudColorF,
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
            changed |= ImGui::Checkbox("Crosshair##s",          &m_settings.showCrosshair);
            changed |= ImGui::Checkbox("Tactical Overlay##s",   &m_settings.showTacticalOverlay);
            changed |= ImGui::Checkbox("Corner Brackets##s",    &m_settings.showCornerBrackets);
            changed |= ImGui::Checkbox("Status Windows##s",     &m_settings.showStatusWindows);
            changed |= ImGui::Checkbox("Show Track IDs##s",     &m_settings.showTrackIDs);
            changed |= ImGui::Checkbox("Show Confidence##s",    &m_settings.showConfidence);
            changed |= ImGui::Checkbox("Show Trails##s",        &m_settings.showTrails);
            changed |= ImGui::Checkbox("Fading Trail Alpha##s", &m_settings.showTrailFade);
            ImGui::EndTabItem();
        }

        // ── Camera & Zoom ────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Camera & Zoom")) {
            ImGui::Spacing();
            changed |= ImGui::Checkbox("Request 4K camera resolution##s", &m_settings.request4KCamera);
            changed |= ImGui::Checkbox("Enable 4K target zoom##s",        &m_settings.enable4KZoom);
            changed |= ImGui::SliderFloat("Target zoom magnification##s",
                &m_settings.targetZoomMagnification, 1.0f, 4.0f, "%.1fx");
            ImGui::Separator();
            ImGui::TextDisabled("Sub Zooms");
            changed |= ImGui::Checkbox("Enable Sub Zooms##szs", &m_settings.subZoomsEnabled);
            if (m_settings.subZoomsEnabled) {
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Use Separate Windows##szs",
                    &m_settings.subZoomsUseSeparateWindows);
            }
            ImGui::Separator();
            changed |= ImGui::Checkbox("Enable Low-Light Enhancement##s",
                &m_settings.lowLightEnhancement);
            if (m_settings.lowLightEnhancement) {
                changed |= ImGui::SliderFloat("Contrast Clip Limit##s_ll",
                    &m_settings.lowLightClipLimit, 1.0f, 10.0f, "%.1f");
                changed |= ImGui::SliderInt("Noise Filter Kernel##s_ll",
                    &m_settings.lowLightDenoiseKernel, 0, 9);
            }
            ImGui::EndTabItem();
        }

        // ── Detection & Tracking ─────────────────────────────────────────────
        if (ImGui::BeginTabItem("Detection & Tracking")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Detection Model");
            {
                const char* modelNames[] = {"YOLOv8s — Precision (default)", "YOLOv8n — Speed"};
                int modelSel = m_settings.detectorModel;
                if (ImGui::Combo("Model##dm", &modelSel, modelNames, 2)) {
                    m_settings.detectorModel = modelSel;
                    changed = true;
                    m_blackboard.requestModelSwitch(modelSel);
                    m_log(LogLevel::INFO, modelSel == 0 ? "Switching model → yolov8s" : "Switching model → yolov8n");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(hot-swap, no restart required)");
            }
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Detector Settings");
            changed |= ImGui::SliderFloat("Confidence##ds", &m_settings.detectorConfThreshold,  0.01f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Score##ds",      &m_settings.detectorScoreThreshold, 0.01f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("NMS##ds",        &m_settings.detectorNmsThreshold,   0.01f, 1.0f, "%.3f");
            changed |= ImGui::SliderInt("Skip Frames##ds",  &m_settings.detectionSkipFrames, 0, 10);
            changed |= ImGui::Checkbox("Grayscale Input##ds", &m_settings.grayscaleInput);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Remote GPU Offloading (LAN)");
            changed |= ImGui::Checkbox("Enable Remote Inference##rem",
                &m_settings.remoteInferenceEnabled);

            ImGui::BeginDisabled(!m_settings.remoteInferenceEnabled);
            static char s_ipBuf[64] = {};
            static bool s_ipBufInit = false;
            if (!s_ipBufInit) {
                strncpy(s_ipBuf, m_settings.remoteInferenceIp.c_str(), sizeof(s_ipBuf));
                s_ipBufInit = true;
            }
            if (ImGui::InputText("Server IP##rem", s_ipBuf, sizeof(s_ipBuf))) {
                m_settings.remoteInferenceIp = s_ipBuf;
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
            changed |= ImGui::SliderFloat("Min Match Score##ts",
                &m_settings.trackerMinMatchScore,   0.01f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Max Center Dist##ts",
                &m_settings.trackerMaxCenterDistPx, 10.0f, 800.0f, "%.0f");
            changed |= ImGui::SliderInt("Max Lost Frames##ts",
                &m_settings.trackerMaxLostFrames, 1, 120);
            changed |= ImGui::SliderInt("Trail Length##ts",
                &m_settings.trackerMaxTrailLength, 5, 200);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f),
                "Re-acquisition & Recovery");
            changed |= ImGui::Checkbox("High-Sensitivity ROI Scans##ra",
                &m_settings.trackerReacquisitionEnabled);
            if (m_settings.trackerReacquisitionEnabled) {
                changed |= ImGui::SliderFloat("Scan ROI Zoom##ra",
                    &m_settings.trackerReacquisitionZoom, 1.1f, 4.0f, "%.1fx");
            }
            changed |= ImGui::Checkbox("Motion-Guided Fallback##ra",
                &m_settings.trackerUseMotionFallback);
            changed |= ImGui::SliderFloat("Lost Match Radius Mult##ra",
                &m_settings.trackerReacquisitionMaxDist, 1.0f, 3.0f, "%.1fx");

            ImGui::EndTabItem();
        }

        // ── Motion Detection ──────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Motion Detection")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "MOTION ANALYSIS ENGINE");
            ImGui::Separator();

            changed |= ImGui::Checkbox("Enable Motion Detection##s_md", &m_settings.motionDetectionEnabled);
            ImGui::BeginDisabled(!m_settings.motionDetectionEnabled);
            
            changed |= ImGui::Checkbox("Show Motion Regions##s_md", &m_settings.motionShowOverlay);
            changed |= ImGui::SliderFloat("Detection Sensitivity##s_md", &m_settings.motionSensitivity, 5.0f, 100.0f, "%.1f");
            changed |= ImGui::SliderInt("Min Object Area (px)##s_md", &m_settings.motionMinArea, 1, 5000);
            changed |= ImGui::SliderFloat("Track Hold Duration (s)##s_md", &m_settings.motionTrackHoldDuration, 0.1f, 10.0f, "%.1f");
            
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Heatmap Overlay (Optical Flow)");
            changed |= ImGui::Checkbox("Enable Heatmap##s_md", &m_settings.motionHeatmapOverlay);
            if (m_settings.motionHeatmapOverlay) {
                changed |= ImGui::SliderFloat("Heatmap Decay##s_md", &m_settings.motionHeatmapDecay, 0.5f, 0.99f, "%.2f");
                changed |= ImGui::SliderFloat("Heatmap Alpha##s_md", &m_settings.motionHeatmapAlpha, 0.0f, 1.0f, "%.2f");
                changed |= ImGui::SliderFloat("Velocity Sensitivity##s_md", &m_settings.motionHeatmapSensitivity, 1.0f, 50.0f, "%.1f");
            }
            
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }

        // ── Audio Alerts ─────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Audio Alerts")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "ACOUSTIC ALERT SYSTEM");
            ImGui::Separator();

            audioChanged |= ImGui::Checkbox("Master Enable##aud", &m_settings.audioEnabled);
            ImGui::BeginDisabled(!m_settings.audioEnabled);

            audioChanged |= ImGui::SliderFloat("Master Volume##aud",
                &m_settings.audioMasterVolume, 0.0f, 1.0f, "%.2f");

            if (ImGui::Button("TEST ALL##aud")) {
                m_audioEngine.applyConfig(MakeAudioConfig(m_settings));
                m_audioEngine.playMotionAlert();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(plays motion tone)");

            ImGui::Spacing();

            if (ImGui::CollapsingHeader("Motion Alert##aud_coll",
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                audioChanged |= ImGui::Checkbox("Enable##audm", &m_settings.audioMotionEnabled);
                ImGui::BeginDisabled(!m_settings.audioMotionEnabled);
                audioChanged |= ImGui::SliderFloat("Frequency (Hz)##audm",
                    &m_settings.audioMotionFreqHz,     100.f, 4000.f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Duration (ms)##audm",
                    &m_settings.audioMotionDurationMs,  20.f,  500.f, "%.0f ms");
                audioChanged |= ImGui::SliderFloat("Cooldown (s)##audm",
                    &m_settings.audioMotionCooldownSec, 0.1f,   10.f, "%.1f s");
                if (ImGui::Button("TEST##audm")) m_audioEngine.playMotionAlert();
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Alarm Zone — Entry##aud_coll")) {
                audioChanged |= ImGui::Checkbox("Enable##aude", &m_settings.audioAlarmEntryEnabled);
                ImGui::BeginDisabled(!m_settings.audioAlarmEntryEnabled);
                audioChanged |= ImGui::SliderFloat("Frequency (Hz)##aude",
                    &m_settings.audioAlarmEntryFreqHz, 100.f, 4000.f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Duration (ms)##aude",
                    &m_settings.audioAlarmEntryDurMs,   20.f,  500.f, "%.0f ms");
                if (ImGui::Button("TEST##aude")) m_audioEngine.playAlarmEntry();
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Alarm Zone — Exit##aud_coll")) {
                audioChanged |= ImGui::Checkbox("Enable##audx", &m_settings.audioAlarmExitEnabled);
                ImGui::BeginDisabled(!m_settings.audioAlarmExitEnabled);
                audioChanged |= ImGui::SliderFloat("Frequency (Hz)##audx",
                    &m_settings.audioAlarmExitFreqHz, 100.f, 4000.f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Duration (ms)##audx",
                    &m_settings.audioAlarmExitDurMs,   20.f,  500.f, "%.0f ms");
                if (ImGui::Button("TEST##audx")) m_audioEngine.playAlarmExit();
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Target Lock — Acquired##aud_coll")) {
                audioChanged |= ImGui::Checkbox("Enable##audla",
                    &m_settings.audioLockAcquiredEnabled);
                ImGui::BeginDisabled(!m_settings.audioLockAcquiredEnabled);
                audioChanged |= ImGui::SliderFloat("Frequency (Hz)##audla",
                    &m_settings.audioLockAcquiredFreqHz, 100.f, 4000.f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Duration (ms)##audla",
                    &m_settings.audioLockAcquiredDurMs,   20.f,  500.f, "%.0f ms");
                if (ImGui::Button("TEST##audla")) m_audioEngine.playLockAcquired();
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Target Lock — Lost##aud_coll")) {
                audioChanged |= ImGui::Checkbox("Enable##audll",
                    &m_settings.audioLockLostEnabled);
                ImGui::BeginDisabled(!m_settings.audioLockLostEnabled);
                audioChanged |= ImGui::SliderFloat("Frequency (Hz)##audll",
                    &m_settings.audioLockLostFreqHz, 100.f, 4000.f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Duration (ms)##audll",
                    &m_settings.audioLockLostDurMs,   20.f,  500.f, "%.0f ms");
                if (ImGui::Button("TEST##audll")) m_audioEngine.playLockLost();
                ImGui::EndDisabled();
            }

            if (ImGui::CollapsingHeader("Lock Pulse — Continuous Targeting##aud_coll",
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextDisabled("Beep accelerates as lock confidence rises.");
                ImGui::TextDisabled("Solution tone fires once at full lock.");
                ImGui::Spacing();
                audioChanged |= ImGui::Checkbox("Enable##audlp", &m_settings.audioLockPulseEnabled);
                ImGui::BeginDisabled(!m_settings.audioLockPulseEnabled);
                audioChanged |= ImGui::SliderFloat("Pulse Frequency (Hz)##audlp",
                    &m_settings.audioLockPulseFreqHz, 100.f, 4000.f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Pulse Duration (ms)##audlp",
                    &m_settings.audioLockPulseDurMs,  10.f,  200.f, "%.0f ms");
                audioChanged |= ImGui::SliderFloat("Min Interval — fast (ms)##audlp",
                    &m_settings.audioLockPulseMinIntervalMs, 50.f, 500.f, "%.0f ms");
                audioChanged |= ImGui::SliderFloat("Max Interval — slow (ms)##audlp",
                    &m_settings.audioLockPulseMaxIntervalMs, 200.f, 2000.f, "%.0f ms");
                ImGui::Separator();
                ImGui::TextDisabled("Solution Tone");
                audioChanged |= ImGui::SliderFloat("Solution Threshold##audlp",
                    &m_settings.audioLockPulseSolutionThresh, 0.5f, 1.0f, "%.2f");
                audioChanged |= ImGui::SliderFloat("Solution Frequency (Hz)##audlp",
                    &m_settings.audioLockPulseSolutionFreqHz, 100.f, 4000.f, "%.0f Hz");
                audioChanged |= ImGui::SliderFloat("Solution Duration (ms)##audlp",
                    &m_settings.audioLockPulseSolutionDurMs, 100.f, 1000.f, "%.0f ms");
                ImGui::Spacing();
                if (ImGui::Button("TEST Full Sequence  (3 s)##audlp")) {
                    m_audioEngine.applyConfig(MakeAudioConfig(m_settings));
                    m_audioEngine.startLockPulseTest();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("slow  →  fast  →  solution tone");
                ImGui::EndDisabled();
            }

            ImGui::EndDisabled(); // master

            ImGui::EndTabItem();
        }

        // ── System & Admin ───────────────────────────────────────────────────
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
            static bool s_confirmQuit = false;
            ImGui::Checkbox("Enable Admin Actions##s_admin", &s_confirmQuit);
            if (s_confirmQuit) {
                if (ImGui::Button("Reset All Settings##s_reset", ImVec2(-1, 0))) {
                    applyStandardPreset();
                    changed = true;
                    m_log(LogLevel::WARN, "All settings reset to defaults");
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
        pushSettingsToBlackboard();
        savePersistedSettings();
    }

    if (audioChanged)
        m_audioEngine.applyConfig(MakeAudioConfig(m_settings));

    ImGui::End();
}
