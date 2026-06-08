#include "DevConsolePanel.hpp"
#include "UIManager_internal.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <GLFW/glfw3.h>

DevConsolePanel::DevConsolePanel() {
    m_fpsHistory.resize(kFpsHistorySize, 0.0f);
    m_inferHistory.resize(kFpsHistorySize, 0.0f);
    m_trackHistory.resize(kFpsHistorySize, 0.0f);
}

void DevConsolePanel::appendLog(LogLevel level, const std::string& msg, float appSeconds) {
    ConsoleEntry e;
    e.level     = level;
    e.message   = msg;
    e.timestamp = appSeconds;
    std::lock_guard<std::mutex> lk(m_consoleMutex);
    m_consoleLog.push_back(std::move(e));
    if (m_consoleLog.size() > kMaxLogEntries)
        m_consoleLog.pop_front();
}

void DevConsolePanel::clearLog() {
    std::lock_guard<std::mutex> lk(m_consoleMutex);
    m_consoleLog.clear();
}

void DevConsolePanel::addFpsSample(float fps) {
    m_fpsHistory[m_fpsHistoryIdx] = fps;
    m_fpsHistoryIdx = (m_fpsHistoryIdx + 1) % kFpsHistorySize;
}

void DevConsolePanel::addPerformanceSamples(float inferMs, float trackTimeMs) {
    m_inferHistory[m_perfHistoryIdx] = inferMs;
    m_trackHistory[m_perfHistoryIdx] = trackTimeMs;
    m_perfHistoryIdx = (m_perfHistoryIdx + 1) % kFpsHistorySize;
}

void DevConsolePanel::render(bool& show,
                             float renderFps, float frameTimeMs, float cameraFps,
                             float inferTimeMs, float trackTimeMs, int loggerQueueSize,
                             int camW, int camH, int trackW, int trackH, int zoomW, int zoomH,
                             int activeTracks, int detections, uint64_t totalFrames,
                             const std::vector<std::string>& classNames,
                             std::string& cameraAddress,
                             const AppStatusState& status,
                             bool isCameraChangePending,
                             std::function<void(const std::string&)> requestCameraChange,
                             std::function<void()> requestMotionReset,
                             std::function<void()> applyStandardPreset,
                             SystemSettings& settings,
                             bool& settingsChanged,
                             std::function<void(LogLevel, const std::string&)> logFn,
                             GLFWwindow* window,
                             float remoteRttMs,
                             const std::string& activeModelName,
                             const TrackingStateData* trackingState,
                             const FaceDebugState* faceDbg) {
    if (!show) return;

    ImGui::Begin("Dev Console", &show);

    const std::string& cameraStatus = status.cameraStatus;
    const bool cameraStatusOk = status.cameraStatusOk;

    // ── Header ───────────────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.5f, 1.0f), "HORUS DEV CONSOLE");
    ImGui::SameLine();
    ImGui::TextDisabled("v1.14.6");
    ImGui::Separator();

    if (ImGui::BeginTabBar("DevTabBar")) {

        // ── TAB: System ──────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("System")) {
            {
                char overlay[32];
                snprintf(overlay, sizeof(overlay), "%.1f fps", renderFps);
                ImGui::PlotLines("##fps", m_fpsHistory.data(),
                                 static_cast<int>(m_fpsHistory.size()),
                                 m_fpsHistoryIdx, overlay,
                                 0.0f, 120.0f, ImVec2(0, 60));
            }

            if (settings.debugPerformanceGraphs) {
                ImGui::Separator();
                ImGui::TextDisabled("Micro-Benchmarking (ms)");
                
                char inferOverlay[32];
                snprintf(inferOverlay, sizeof(inferOverlay), "Inference: %.1f ms", inferTimeMs);
                ImGui::PlotLines("##infer_bench", m_inferHistory.data(),
                                 static_cast<int>(m_inferHistory.size()),
                                 m_perfHistoryIdx, inferOverlay,
                                 0.0f, 100.0f, ImVec2(0, 50));

                char trackOverlay[32];
                snprintf(trackOverlay, sizeof(trackOverlay), "Tracking: %.1f ms", trackTimeMs);
                ImGui::PlotLines("##track_bench", m_trackHistory.data(),
                                 static_cast<int>(m_trackHistory.size()),
                                 m_perfHistoryIdx, trackOverlay,
                                 0.0f, 20.0f, ImVec2(0, 50));
            }
            ImGui::Separator();

            ImGui::Columns(2, "sysmetrics", true);
            auto metric = [](const char* label, const char* fmt, ...) {
                va_list args;
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", label);
                ImGui::NextColumn();
                char buf[64];
                va_start(args, fmt);
                vsnprintf(buf, sizeof(buf), fmt, args);
                va_end(args);
                ImGui::Text("%s", buf);
                ImGui::NextColumn();
            };
            metric("Render FPS",           "%.2f",      renderFps);
            metric("Frame Time",           "%.2f ms",   frameTimeMs);
            metric("Inference Time",       "%.2f ms",   inferTimeMs);
            metric("Tracking Time",        "%.2f ms",   trackTimeMs);
            metric("Logger Backlog",       "%d frames", loggerQueueSize);
            metric("Camera FPS",           "%.2f",      cameraFps);
            metric("Capture Resolution",   "%dx%d",     camW, camH);
            metric("Tracking Resolution",  "%dx%d",     trackW, trackH);
            {
                const int zSrcW = settings.enable4KZoom ? camW : trackW;
                const int zSrcH = settings.enable4KZoom ? camH : trackH;
                metric("Zoom Source Resolution", "%dx%d", zSrcW, zSrcH);
            }
            metric("Zoom Crop Resolution", "%dx%d",     zoomW, zoomH);
            metric("Active Tracks",        "%d",        activeTracks);
            metric("Detections",           "%d",        detections);
            metric("Total Frames",         "%llu",      (unsigned long long)totalFrames);
            if (!activeModelName.empty())
                metric("Active Model",     "%s",        activeModelName.c_str());
            if (settings.remoteInferenceEnabled) {
                if (remoteRttMs >= 0.0f)
                    metric("Remote RTT",   "%.1f ms",   remoteRttMs);
                else
                    metric("Remote RTT",   "%s",        "—");
            }
            ImGui::Columns(1);

            ImGui::Separator();
            ImGui::TextDisabled("Debug Overlays");
            settingsChanged |= ImGui::Checkbox("Show Raw YOLO Detections", &settings.debugShowRawDetections);
            settingsChanged |= ImGui::Checkbox("Show Kalman Velocity Vectors", &settings.debugShowKalmanVectors);
            settingsChanged |= ImGui::Checkbox("Freeze Vision Processing", &settings.debugFreezeVision);
            settingsChanged |= ImGui::Checkbox("Performance Graphs", &settings.debugPerformanceGraphs);

            ImGui::Separator();
            ImGui::TextDisabled("Pipeline");
            settingsChanged |= ImGui::Checkbox("Enable Detection",  &settings.enableDetection);
            ImGui::SameLine();
            settingsChanged |= ImGui::Checkbox("Enable Tracking",   &settings.enableTracking);
            settingsChanged |= ImGui::Checkbox("Grayscale Input (faster)", &settings.grayscaleInput);
            ImGui::SetNextItemWidth(160);
            settingsChanged |= ImGui::SliderInt("Detection Skip Frames",
                                                &settings.detectionSkipFrames, 0, 10);
            
            ImGui::Separator();
            ImGui::TextDisabled("Camera Resolution & Zoom");
            settingsChanged |= ImGui::Checkbox("Request 4K Camera Resolution", &settings.request4KCamera);
            ImGui::SameLine();
            settingsChanged |= ImGui::Checkbox("Enable 4K Target Zoom", &settings.enable4KZoom);
            ImGui::SetNextItemWidth(220.0f);
            settingsChanged |= ImGui::SliderFloat("Target Zoom Magnification",
                &settings.targetZoomMagnification, 1.0f, 4.0f, "%.1fx");

            ImGui::Separator();
            ImGui::TextDisabled("Motion Detection");
            settingsChanged |= ImGui::Checkbox("Enable Motion Detection##md",
                &settings.motionDetectionEnabled);
            if (settings.motionDetectionEnabled) {
                ImGui::SameLine();
                settingsChanged |= ImGui::Checkbox("Show Overlay##md", &settings.motionShowOverlay);
                ImGui::SameLine();
                settingsChanged |= ImGui::Checkbox("Heatmap##md", &settings.motionHeatmapOverlay);

                if (settings.motionHeatmapOverlay) {
                    ImGui::SetNextItemWidth(140);
                    settingsChanged |= ImGui::SliderFloat("Heatmap Decay##md",
                        &settings.motionHeatmapDecay, 0.5f, 0.99f, "%.2f");
                    ImGui::SetNextItemWidth(140);
                    settingsChanged |= ImGui::SliderFloat("Heatmap Sensitivity##md",
                        &settings.motionHeatmapSensitivity, 1.0f, 50.0f, "%.1f");
                    ImGui::SetNextItemWidth(140);
                    settingsChanged |= ImGui::SliderFloat("Heatmap Alpha##md",
                        &settings.motionHeatmapAlpha, 0.0f, 1.0f, "%.2f");
                }

                settingsChanged |= ImGui::Checkbox("Enable Sub Zooms##sz", &settings.subZoomsEnabled);
                if (settings.subZoomsEnabled) {
                    ImGui::SameLine();
                    settingsChanged |= ImGui::Checkbox("Use Separate Windows##sz",
                        &settings.subZoomsUseSeparateWindows);
                    ImGui::SetNextItemWidth(220.0f);
                    settingsChanged |= ImGui::SliderInt("Sub Zoom Padding (px)##sz",
                        &settings.subZoomPaddingPx, 0, 100);
                    ImGui::SetNextItemWidth(220.0f);
                    settingsChanged |= ImGui::SliderFloat("Sub Zoom Magnification##sz",
                        &settings.subZoomMagnification, 1.0f, 4.0f, "%.1fx");
                }

                ImGui::SetNextItemWidth(220.0f);
                settingsChanged |= ImGui::SliderFloat("Sensitivity##md",
                    &settings.motionSensitivity, 5.0f, 100.0f, "%.1f");
                ImGui::SetNextItemWidth(220.0f);
                settingsChanged |= ImGui::SliderInt("Min. Area (px)##md",
                    &settings.motionMinArea, 1, 5000, "%d", ImGuiSliderFlags_Logarithmic);
                ImGui::SetNextItemWidth(220.0f);
                settingsChanged |= ImGui::SliderFloat("Track Hold Duration (s)##md",
                    &settings.motionTrackHoldDuration, 0.1f, 10.0f, "%.1f s");

                if (ImGui::Button("Reset Background##md")) {
                    requestMotionReset();
                }
            }

            ImGui::Separator();
            ImGui::TextDisabled("Source Selection");

            {
                // Camera Source Dropdown
                if (ImGui::BeginCombo("Video Source##cam", cameraAddress.c_str())) {
                    // List detected cameras
                    for (size_t i = 0; i < status.cameraDeviceNames.size(); ++i) {
                        bool isSelected = (cameraAddress == std::to_string(status.cameraDeviceIDs[i]));
                        if (ImGui::Selectable(status.cameraDeviceNames[i].c_str(), isSelected)) {
                            std::string newAddr = std::to_string(status.cameraDeviceIDs[i]);
                            cameraAddress = newAddr;
                            requestCameraChange(newAddr);
                        }
                    }
                    ImGui::Separator();
                    // Manual entry
                    static char s_manualCam[256] = {0};
                    if (ImGui::InputTextWithHint("##manualcam", "Custom URL / ID", s_manualCam, sizeof(s_manualCam), ImGuiInputTextFlags_EnterReturnsTrue)) {
                        std::string newAddr(s_manualCam);
                        cameraAddress = newAddr;
                        requestCameraChange(newAddr);
                    }
                    ImGui::EndCombo();
                }

                // Audio Source Dropdown
                std::string currentAudioName = "None (Synthesized Only)";
                for (size_t i = 0; i < status.audioDeviceIDs.size(); ++i) {
                    if (status.audioDeviceIDs[i] == settings.audioCaptureDeviceId && settings.audioCaptureEnabled) {
                        currentAudioName = status.audioDeviceNames[i];
                        break;
                    }
                }

                if (ImGui::BeginCombo("Audio Source##aud", currentAudioName.c_str())) {
                    if (ImGui::Selectable("None (Synthesized Only)", !settings.audioCaptureEnabled)) {
                        settings.audioCaptureEnabled = false;
                        settingsChanged = true;
                    }
                    for (size_t i = 0; i < status.audioDeviceNames.size(); ++i) {
                        bool isSelected = (settings.audioCaptureEnabled && settings.audioCaptureDeviceId == status.audioDeviceIDs[i]);
                        if (ImGui::Selectable(status.audioDeviceNames[i].c_str(), isSelected)) {
                            settings.audioCaptureDeviceId = status.audioDeviceIDs[i];
                            settings.audioCaptureEnabled = true;
                            settingsChanged = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }

            if (isCameraChangePending)
                ImGui::TextColored(ImVec4(1.0f,0.8f,0.1f,1.0f), "  Switching...");

            ImGui::Separator();
            static bool confirmQuit = false;
            ImGui::Checkbox("Enable Admin Actions", &confirmQuit);
            if (confirmQuit) {
                if (ImGui::Button("Reset All Settings", ImVec2(-1, 0))) {
                    applyStandardPreset();
                    settingsChanged = true;
                }
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
                if (ImGui::Button("Quit Application", ImVec2(-1, 0)))
                    glfwSetWindowShouldClose(window, true);
                ImGui::PopStyleColor();
            }

            ImGui::EndTabItem();
        }

        // ── TAB: Detector ────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Detector")) {
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Confidence##det", &settings.detectorConfThreshold, 0.01f, 1.0f, "%.3f");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("Score##det", &settings.detectorScoreThreshold, 0.01f, 1.0f, "%.3f");
            ImGui::SetNextItemWidth(-1);
            settingsChanged |= ImGui::SliderFloat("NMS IoU##det", &settings.detectorNmsThreshold, 0.01f, 1.0f, "%.3f");

            ImGui::Separator();
            ImGui::TextDisabled("Class Filter");
            settingsChanged |= ImGui::Checkbox("Filter by Priority Classes", &settings.filterByPriorityClasses);

            if (settings.filterByPriorityClasses && !classNames.empty()) {
                if (ImGui::Button("Select All")) {
                    for (int i = 0; i < (int)classNames.size(); ++i) settings.priorityClasses.insert(i);
                    settingsChanged = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear All")) {
                    settings.priorityClasses.clear();
                    settingsChanged = true;
                }

                if (ImGui::BeginTable("ClassGrid", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg, ImVec2(0, 180.0f))) {
                    for (size_t i = 0; i < classNames.size(); ++i) {
                        if (i % 4 == 0) ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(i % 4);
                        bool checked = (settings.priorityClasses.find((int)i) != settings.priorityClasses.end());
                        if (ImGui::Checkbox(classNames[i].c_str(), &checked)) {
                            if (checked) settings.priorityClasses.insert((int)i);
                            else settings.priorityClasses.erase((int)i);
                            settingsChanged = true;
                        }
                    }
                    ImGui::EndTable();
                }
            }

            ImGui::EndTabItem();
        }

        // ── TAB: Face / KI ───────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Face / KI")) {

            // ── Face Recognizer Status ───────────────────────────────────────
            ImGui::TextDisabled("Face Recognizer");
            if (faceDbg) {
                if (faceDbg->initOk) {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "[OK]");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "[FEHLER - nicht geladen]");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Identitaeten: %d", faceDbg->identityCount);

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::TextWrapped("Det:  %s", faceDbg->detPath.empty() ? "(kein Pfad)" : faceDbg->detPath.c_str());
                ImGui::TextWrapped("Rec:  %s", faceDbg->recPath.empty() ? "(kein Pfad)" : faceDbg->recPath.c_str());
                ImGui::PopStyleColor();

                ImGui::Separator();
                ImGui::Columns(2, "facestats", false);
                ImGui::TextDisabled("Aufrufe gesamt");  ImGui::NextColumn();
                ImGui::Text("%d", faceDbg->callCount);  ImGui::NextColumn();
                ImGui::TextDisabled("Gesichter (letzter Aufruf)"); ImGui::NextColumn();
                ImGui::Text("%d", faceDbg->lastFacesFound); ImGui::NextColumn();
                ImGui::TextDisabled("Gesichter gesamt");  ImGui::NextColumn();
                ImGui::Text("%d", faceDbg->totalFacesFound); ImGui::NextColumn();
                ImGui::Columns(1);
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f), "Keine Debug-Daten verfuegbar");
            }

            // ── Live Controls ────────────────────────────────────────────────
            ImGui::Separator();
            ImGui::TextDisabled("Live-Einstellungen");
            settingsChanged |= ImGui::Checkbox("Gesichtserkennung aktiv##face", &settings.faceRecognitionEnabled);
            settingsChanged |= ImGui::Checkbox("Gesichts-Boxen anzeigen##face", &settings.showFaceBoxes);
            ImGui::SetNextItemWidth(220.0f);
            settingsChanged |= ImGui::SliderFloat("Erkennungs-Schwellwert##face",
                &settings.faceRecognitionThreshold, 0.1f, 1.0f, "%.3f");
            ImGui::SetNextItemWidth(220.0f);
            settingsChanged |= ImGui::SliderFloat("Erkennungs-Mindestkonfidenz##face",
                &settings.faceDetectionMinConfidence, 0.1f, 1.0f, "%.3f");

            // ── Per-Track Face State ─────────────────────────────────────────
            ImGui::Separator();
            ImGui::TextDisabled("Pro-Track Gesichtszustand");
            if (trackingState && !trackingState->activeTracks.empty()) {
                if (ImGui::BeginTable("FaceTrackTable", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0, 160.0f))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Track",    ImGuiTableColumnFlags_WidthFixed, 42.0f);
                    ImGui::TableSetupColumn("Box",      ImGuiTableColumnFlags_WidthFixed, 72.0f);
                    ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed, 32.0f);
                    ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (const auto& obj : trackingState->activeTracks) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%d", obj.track_id);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%dx%d", obj.box.width, obj.box.height);
                        ImGui::TableSetColumnIndex(2);
                        if (obj.face_id >= 0)
                            ImGui::Text("%d", obj.face_id);
                        else
                            ImGui::TextDisabled("--");
                        ImGui::TableSetColumnIndex(3);
                        if (!obj.face_name.empty())
                            ImGui::TextUnformatted(obj.face_name.c_str());
                        else
                            ImGui::TextDisabled("(unbekannt)");
                    }
                    ImGui::EndTable();
                }
            } else {
                ImGui::TextDisabled("Keine aktiven Tracks");
            }

            // ── KI / Dossier ─────────────────────────────────────────────────
            ImGui::Separator();
            ImGui::TextDisabled("KI / Dossier");
            settingsChanged |= ImGui::Checkbox("KI-Dossier aktivieren##ai", &settings.aiDossierEnabled);

            ImGui::EndTabItem();
        }

        // ── TAB: Console ─────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Console")) {
            ImGui::SetNextItemWidth(180);
            ImGui::InputText("Filter", m_logFilter, sizeof(m_logFilter));
            ImGui::SameLine();
            if (ImGui::Button("Clear")) clearLog();
            ImGui::SameLine();
            ImGui::Checkbox("Auto-Scroll", &m_autoScrollLog);

            ImGui::Separator();
            ImGui::BeginChild("ConsoleScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

            std::lock_guard<std::mutex> lk(m_consoleMutex);
            for (const auto& entry : m_consoleLog) {
                if (m_logFilter[0] != '\0' && entry.message.find(m_logFilter) == std::string::npos) continue;

                ImVec4 col;
                const char* prefix;
                switch (entry.level) {
                    case LogLevel::VERBOSE: col={0.6f,0.6f,0.6f,1}; prefix="[VRB]"; break;
                    case LogLevel::INFO:    col={0.8f,0.9f,1.0f,1}; prefix="[INF]"; break;
                    case LogLevel::WARN:    col={1.0f,0.8f,0.2f,1}; prefix="[WRN]"; break;
                    case LogLevel::ERR:     col={1.0f,0.3f,0.3f,1}; prefix="[ERR]"; break;
                    default:               col={1.0f,1.0f,1.0f,1}; prefix="[???]"; break;
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

    ImGui::End();
}
