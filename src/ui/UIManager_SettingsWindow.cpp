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

        // ── AI Dossier ───────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("AI Dossier")) {
            ImGui::Spacing();
            changed |= ImGui::Checkbox("Enable AI Dossiers##s", &m_settings.aiDossierEnabled);
            ImGui::Separator();
            
            ImGui::Text("OpenRouter API Key:");
            char keyBuf[128];
            strncpy(keyBuf, m_settings.aiOpenRouterKey.c_str(), sizeof(keyBuf));
            if (ImGui::InputText("##aiKey", keyBuf, sizeof(keyBuf), ImGuiInputTextFlags_Password)) {
                m_settings.aiOpenRouterKey = keyBuf;
                changed = true;
            }
            
            ImGui::Text("VLM Model:");
            char modelBuf[128];
            strncpy(modelBuf, m_settings.aiVlmModel.c_str(), sizeof(modelBuf));
            if (ImGui::InputText("##aiModel", modelBuf, sizeof(modelBuf))) {
                m_settings.aiVlmModel = modelBuf;
                changed = true;
            }
            ImGui::TextDisabled("Default: google/gemini-flash-1.5");
            
            ImGui::Separator();
            changed |= ImGui::SliderFloat("ReID Threshold##s", &m_settings.aiReidThreshold, 0.5f, 0.95f, "%.2f");
            changed |= ImGui::SliderInt("Req / Min##s", &m_settings.aiRequestLimitPerMin, 1, 20);
            changed |= ImGui::SliderFloat("Stability (sec)##s", &m_settings.aiStabilitySec, 1.0f, 10.0f, "%.1f");
            
            ImGui::Separator();
            if (ImGui::Checkbox("Show Dossier Panel##s",   &m_showDossierPanel))   changed = true;
            if (ImGui::Checkbox("Show Dossier Archive##s", &m_showDossierArchive)) changed = true;

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

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f),
                "Target Priority (automatic)");
            changed |= ImGui::Checkbox("Auto Target Priority##prio",
                &m_settings.targetPriorityEnabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Bewertet pro Frame, welches Ziel für den Operator am wichtigsten ist\n(Alarmzone, Klasse, Tempo, Nähe, Annäherung, Neuheit).");
            if (m_settings.targetPriorityEnabled) {
                changed |= ImGui::Checkbox("Highlight Top Target in HUD##prio",
                    &m_settings.priorityShowTopBadge);
            }

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

        // ── Face Detection & Recognition ─────────────────────────────────────
        if (ImGui::BeginTabItem("Gesichtserkennung")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Gesichtserkennung");
            changed |= ImGui::Checkbox("Aktivieren##fr", &m_settings.faceRecognitionEnabled);

            ImGui::BeginDisabled(!m_settings.faceRecognitionEnabled);
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Erkennungs-Schwelle");
            ImGui::TextDisabled("Cosine-Similarity: höher = strenger (weniger Fehlerkennungen)");
            changed |= ImGui::SliderFloat("Erkennungs-Schwelle##fr",
                &m_settings.faceRecognitionThreshold, 0.10f, 0.90f, "%.2f");

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Auto-Registrierung");
            ImGui::TextDisabled("Mindest-Konfidenz des YuNet-Detektors, um ein neues");
            ImGui::TextDisabled("unbekanntes Gesicht automatisch zu registrieren.");
            changed |= ImGui::SliderFloat("Mindest-Konfidenz##fr",
                &m_settings.faceDetectionMinConfidence, 0.50f, 1.00f, "%.2f");

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Gesichts-Box");
            changed |= ImGui::Checkbox("Gesichts-Box zeichnen##fr", &m_settings.showFaceBoxes);
            ImGui::BeginDisabled(!m_settings.showFaceBoxes);
            {
                // Color picker (ImGui ColorEdit4 works on float[4])
                static float s_faceColorF[4] = {0.0f, 0.86f, 1.0f, 0.86f};
                static bool  s_faceColorInit = false;
                if (!s_faceColorInit) {
                    if (m_settings.faceBoxColor != 0) {
                        s_faceColorF[0] = ((m_settings.faceBoxColor >>  0) & 0xFF) / 255.0f;
                        s_faceColorF[1] = ((m_settings.faceBoxColor >>  8) & 0xFF) / 255.0f;
                        s_faceColorF[2] = ((m_settings.faceBoxColor >> 16) & 0xFF) / 255.0f;
                        s_faceColorF[3] = ((m_settings.faceBoxColor >> 24) & 0xFF) / 255.0f;
                    }
                    s_faceColorInit = true;
                }
                if (ImGui::ColorEdit4("Box-Farbe##fr", s_faceColorF,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                    m_settings.faceBoxColor =
                        IM_COL32(static_cast<int>(s_faceColorF[0] * 255),
                                 static_cast<int>(s_faceColorF[1] * 255),
                                 static_cast<int>(s_faceColorF[2] * 255),
                                 static_cast<int>(s_faceColorF[3] * 255));
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled(); // faceRecognitionEnabled
            ImGui::EndTabItem();
        }

        // ── Radar ──────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Radar")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f),
                               "Live Aircraft Scope (OpenSky Network)");
            changed |= ImGui::Checkbox("Enable Radar Polling##rad", &m_settings.radarEnabled);
            ImGui::Spacing();

            changed |= ImGui::InputFloat("Center Latitude##rad",  &m_settings.radarCenterLat, 0.0f, 0.0f, "%.4f");
            changed |= ImGui::InputFloat("Center Longitude##rad", &m_settings.radarCenterLon, 0.0f, 0.0f, "%.4f");
            m_settings.radarCenterLat = std::clamp(m_settings.radarCenterLat, -90.0f, 90.0f);
            m_settings.radarCenterLon = std::clamp(m_settings.radarCenterLon, -180.0f, 180.0f);

            changed |= ImGui::SliderFloat("Range (km)##rad",       &m_settings.radarRadiusKm, 10.0f, 400.0f, "%.0f");
            changed |= ImGui::SliderFloat("Sweep Period (s)##rad", &m_settings.radarSweepPeriodSec, 1.0f, 12.0f, "%.1f");
            changed |= ImGui::SliderInt("Refresh (ms)##rad",       &m_settings.radarRefreshMs, 2000, 30000);
            ImGui::TextDisabled("Anonymous OpenSky access is rate-limited; >=10000 ms recommended.");

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "Map Background (OpenStreetMap)");
            changed |= ImGui::Checkbox("Enable Map##rad", &m_settings.radarMapEnabled);
            ImGui::BeginDisabled(!m_settings.radarMapEnabled);
            changed |= ImGui::SliderFloat("Map Opacity##rad", &m_settings.radarMapOpacity, 0.0f, 1.0f, "%.2f");
            ImGui::Spacing();
            ImGui::TextDisabled("Tile Server URL  ({z} {x} {y} = zoom / tile coords)");

            // Presets
            static const char* kPresets[] = {
                "OpenStreetMap (Standard)",
                "OpenTopoMap",
                "Custom"
            };
            static const char* kPresetUrls[] = {
                "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
                "https://tile.opentopomap.org/{z}/{x}/{y}.png",
                ""
            };
            static int radMapPreset = 0;
            static char tileUrlBuf[256] = {0};
            static bool tileUrlInit = false;
            if (!tileUrlInit) {
                std::strncpy(tileUrlBuf, m_settings.radarMapTileUrl.c_str(), sizeof(tileUrlBuf) - 1);
                // Match preset
                radMapPreset = 2; // default to Custom
                for (int i = 0; i < 2; ++i)
                    if (m_settings.radarMapTileUrl == kPresetUrls[i]) { radMapPreset = i; break; }
                tileUrlInit = true;
            }
            if (ImGui::Combo("Tile Server##rad", &radMapPreset, kPresets, 3)) {
                if (radMapPreset < 2) {
                    m_settings.radarMapTileUrl = kPresetUrls[radMapPreset];
                    std::strncpy(tileUrlBuf, kPresetUrls[radMapPreset], sizeof(tileUrlBuf) - 1);
                    changed = true;
                }
            }
            ImGui::BeginDisabled(radMapPreset != 2);
            if (ImGui::InputText("URL##radtile", tileUrlBuf, sizeof(tileUrlBuf))) {
                m_settings.radarMapTileUrl = tileUrlBuf;
                changed = true;
            }
            ImGui::EndDisabled(); // custom URL
            ImGui::TextDisabled("Note: changing URL clears the tile cache.");
            ImGui::EndDisabled(); // map enabled

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.4f, 1.0f), "OpenSky OAuth2 (optional)");
            ImGui::TextDisabled("Leave empty for anonymous access.");

            static char clientIdBuf[128]     = {0};
            static char clientSecretBuf[128] = {0};
            static bool radCredsInit = false;
            if (!radCredsInit) {
                std::strncpy(clientIdBuf,     m_settings.openSkyClientId.c_str(),     sizeof(clientIdBuf) - 1);
                std::strncpy(clientSecretBuf, m_settings.openSkyClientSecret.c_str(), sizeof(clientSecretBuf) - 1);
                radCredsInit = true;
            }
            if (ImGui::InputText("Client ID##rad", clientIdBuf, sizeof(clientIdBuf))) {
                m_settings.openSkyClientId = clientIdBuf;
                changed = true;
            }
            if (ImGui::InputText("Client Secret##rad", clientSecretBuf, sizeof(clientSecretBuf),
                                 ImGuiInputTextFlags_Password)) {
                m_settings.openSkyClientSecret = clientSecretBuf;
                changed = true;
            }

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
