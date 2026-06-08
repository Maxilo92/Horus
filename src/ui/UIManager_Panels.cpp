#include "UIManager.hpp"
#include "UIManager_internal.hpp"
#include <algorithm>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// updateFilteredHistory
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::updateFilteredHistory(const std::string& filter, int sortCol, bool sortDesc) {
    auto toLower = [](const std::string& v) {
        std::string lo = v;
        std::transform(lo.begin(), lo.end(), lo.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return lo;
    };

    const std::string filterLower = toLower(filter);
    
    // Check if we actually need to re-filter/re-sort
    // Note: We always re-sort if SpecsDirty, but updateFilteredHistory is called 
    // after the table logic has already determined the new sort specs.
    bool needsUpdate = (m_targetHistory.size() != m_lastHistorySize) || 
                       (filterLower != m_lastFilterText) ||
                       (sortCol != m_lastSortCol) ||
                       (sortDesc != m_lastSortDesc);
    
    if (!needsUpdate) return;

    m_filteredHistory.clear();
    m_filteredHistory.reserve(m_targetHistory.size());

    // 1. Filter
    for (const auto& rec : m_targetHistory) {
        if (filterLower.empty()) {
            m_filteredHistory.push_back(rec);
            continue;
        }
        std::string state = rec.is_currently_active ? "active" : "lost";
        std::string searchSource = std::to_string(rec.track_id) + " " + toLower(rec.className) + " " + state;
        if (searchSource.find(filterLower) != std::string::npos) {
            m_filteredHistory.push_back(rec);
        }
    }

    // 2. Sort
    if (sortCol >= 0) {
        std::stable_sort(m_filteredHistory.begin(), m_filteredHistory.end(),
            [&](const UniqueTargetRecord& a, const UniqueTargetRecord& b) {
                auto less = [sortDesc](auto x, auto y){ return sortDesc ? y < x : x < y; };
                switch (sortCol) {
                    case 0: return less(a.track_id, b.track_id);
                    case 1: return less(toLower(a.className), toLower(b.className));
                    case 3: return less(a.max_confidence, b.max_confidence);
                    case 4: return less(a.is_currently_active?1:0, b.is_currently_active?1:0);
                    case 5: return less(a.first_seen_timestamp, b.first_seen_timestamp);
                    case 6: return less(a.last_seen_timestamp,  b.last_seen_timestamp);
                    default: return less(a.track_id, b.track_id);
                }
            });
    }

    m_lastFilterText  = filterLower;
    m_lastSortCol     = sortCol;
    m_lastSortDesc    = sortDesc;
    m_lastHistorySize = m_targetHistory.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// renderDataPanel
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::renderDataPanel(const std::vector<TrackedObject>& tracked,
                                const TrackedTarget& locked,
                                float cameraFps, float renderFps) {
    ImGui::Begin("Data Panel");

    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.5f, 1.0f), "TRACKS: %zu", tracked.size());
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.0f, 1.0f), "  |  CAM FPS: %.1f", cameraFps);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "  |  RENDER FPS: %.1f", renderFps);
    ImGui::Separator();

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##DataPanelFilter", "Filter by ID, class, or state",
                              m_dataPanelFilter, sizeof(m_dataPanelFilter));
    ImGui::SameLine();
    if (ImGui::Button("Clear##DataPanelFilter")) m_dataPanelFilter[0] = '\0';
    ImGui::Separator();

    auto toLower = [](const std::string& v) {
        std::string lo = v;
        std::transform(lo.begin(), lo.end(), lo.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return lo;
    };
    const std::string filterText = m_dataPanelFilter; // use raw filter here, updateFilteredHistory handles toLower

    if (ImGui::BeginTabBar("DataPanelTabs")) {

        // ── Active Tracks ─────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Active Tracks")) {
            const std::string filterLower = toLower(filterText);
            std::vector<TrackedObject> visible;
            visible.reserve(tracked.size());
            for (const auto& obj : tracked) {
                if (filterLower.empty()) { visible.push_back(obj); continue; }
                std::string state;
                if (locked.state != TrackingState::SEARCHING && locked.track_id == obj.track_id) state = "locked";
                else if (obj.is_confirmed) state = "lock";
                else if (obj.lost_frames > 0) state = "lost";
                else state = "init";
                if (toLower(std::to_string(obj.track_id) + " " + obj.className + " " + state)
                        .find(filterLower) != std::string::npos)
                    visible.push_back(obj);
            }

            auto stateRank = [&](const TrackedObject& o) {
                if (locked.state != TrackingState::SEARCHING && locked.track_id == o.track_id) return 3;
                if (o.is_confirmed) return 2;
                if (o.lost_frames > 0) return 1;
                return 0;
            };

            if (ImGui::BeginTable("Tracks", 5,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("ID",        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 35);
                ImGui::TableSetupColumn("Class",     ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Pos (X,Y)", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Conf",      ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("State",     ImGuiTableColumnFlags_WidthFixed, 55);
                ImGui::TableHeadersRow();

                if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
                    int col = ss->Specs[0].ColumnIndex;
                    bool desc = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                    std::stable_sort(visible.begin(), visible.end(),
                        [&](const TrackedObject& a, const TrackedObject& b) {
                            auto less = [desc](auto x, auto y){ return desc ? y < x : x < y; };
                            switch (col) {
                                case 0: return less(a.track_id, b.track_id);
                                case 1: return less(toLower(a.className), toLower(b.className));
                                case 3: return less(a.confidence, b.confidence);
                                case 4: return less(stateRank(a), stateRank(b));
                                default: return less(a.track_id, b.track_id);
                            }
                        });
                    ss->SpecsDirty = false;
                }

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(visible.size()));
                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                        const auto& obj = visible[i];
                        bool isLocked = (locked.state != TrackingState::SEARCHING && locked.track_id == obj.track_id);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        char lbl[32];
                        snprintf(lbl, sizeof(lbl), "%03d", obj.track_id);
                        if (ImGui::Selectable(lbl, isLocked, ImGuiSelectableFlags_SpanAllColumns)) {
                            if (isLocked) { m_blackboard.requestTargetRelease(); m_selectedAnalyzerTargetId = -1; }
                            else          { m_blackboard.requestTargetLock(obj.track_id); m_selectedAnalyzerTargetId = obj.track_id; }
                        }
                        char ctxId[64];
                        snprintf(ctxId, sizeof(ctxId), "ActiveRowCtx##%d", obj.track_id);
                        if (ImGui::BeginPopupContextItem(ctxId)) {
                            if (ImGui::MenuItem("Export Target Data (JSON/PNG)")) {
                                auto it = std::find_if(m_targetHistory.begin(), m_targetHistory.end(),
                                    [&](const UniqueTargetRecord& r){ return r.track_id == obj.track_id; });
                                if (it != m_targetHistory.end()) exportTarget(*it);
                                else m_log(LogLevel::ERR, "Target record not found for export.");
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%s", obj.className.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%.0f, %.0f",
                                    obj.box.x + obj.box.width  / 2.0f,
                                    obj.box.y + obj.box.height / 2.0f);
                        ImGui::TableSetColumnIndex(3);
                        ImVec4 cc = obj.confidence > 0.6f ? ImVec4(0,1,0.4f,1)
                                  : obj.confidence > 0.4f ? ImVec4(1,0.8f,0,1)
                                                          : ImVec4(1,0.3f,0.3f,1);
                        ImGui::TextColored(cc, "%.2f", obj.confidence);
                        ImGui::TableSetColumnIndex(4);
                        if (isLocked)           ImGui::TextColored(ImVec4(1,0.2f,0.2f,1), "LOCKED");
                        else if (obj.is_confirmed) ImGui::TextColored(ImVec4(0,1,0.5f,1),  "LOCK");
                        else if (obj.lost_frames>0) ImGui::TextColored(ImVec4(1,0.5f,0,1),  "LOST");
                        else                       ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1),"INIT");
                    }
                }
                ImGui::EndTable();
            }
            if (locked.state != TrackingState::SEARCHING) {
                ImGui::Separator();
                if (ImGui::Button("Release Lock", ImVec2(-FLT_MIN, 0)))
                    m_blackboard.requestTargetRelease();
            }
            ImGui::EndTabItem();
        }

        // ── Target History ────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Target History")) {
            int sortCol = -1;
            bool sortDesc = false;

            if (ImGui::BeginTable("HistoryTable", 7,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 35);
                ImGui::TableSetupColumn("Class",    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Pos (X,Y)",ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Max Conf", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("State",    ImGuiTableColumnFlags_WidthFixed, 55);
                ImGui::TableSetupColumn("Found",    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Lost",     ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
                    sortCol = ss->Specs[0].ColumnIndex;
                    sortDesc = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                    // We don't reset SpecsDirty here, we let updateFilteredHistory handle the change detection
                }

                // Lazy update of the filtered/sorted history
                updateFilteredHistory(filterText, sortCol, sortDesc);

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(m_filteredHistory.size()));
                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                        const auto& rec = m_filteredHistory[i];
                        bool isSel = (m_selectedAnalyzerTargetId == rec.track_id);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        char lbl[32]; snprintf(lbl, sizeof(lbl), "%03d", rec.track_id);
                        if (ImGui::Selectable(lbl, isSel, ImGuiSelectableFlags_SpanAllColumns)) {
                            if (isSel) m_selectedAnalyzerTargetId = -1;
                            else {
                                m_selectedAnalyzerTargetId = rec.track_id;
                                if (rec.is_currently_active) m_blackboard.requestTargetLock(rec.track_id);
                            }
                        }
                        char ctxId[64]; snprintf(ctxId, sizeof(ctxId), "HistCtx##%d", rec.track_id);
                        if (ImGui::BeginPopupContextItem(ctxId)) {
                            if (ImGui::MenuItem("Export Target Data (JSON/PNG)")) exportTarget(rec);
                            ImGui::EndPopup();
                        }
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%s", rec.className.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%.0f, %.0f",
                                    rec.last_box.x + rec.last_box.width  / 2.0f,
                                    rec.last_box.y + rec.last_box.height / 2.0f);
                        ImGui::TableSetColumnIndex(3);
                        ImVec4 cc = rec.max_confidence > 0.6f ? ImVec4(0,1,0.4f,1)
                                  : rec.max_confidence > 0.4f ? ImVec4(1,0.8f,0,1)
                                                               : ImVec4(1,0.3f,0.3f,1);
                        ImGui::TextColored(cc, "%.2f", rec.max_confidence);
                        ImGui::TableSetColumnIndex(4);
                        if (rec.is_currently_active) ImGui::TextColored(ImVec4(0,1,0.5f,1),  "ACTIVE");
                        else                         ImGui::TextColored(ImVec4(1,0.3f,0.3f,1),"LOST");
                        ImGui::TableSetColumnIndex(5); ImGui::Text("%s", rec.first_seen_timestamp.c_str());
                        ImGui::TableSetColumnIndex(6);
                        ImGui::Text("%s", rec.is_currently_active ? "-" : rec.last_seen_timestamp.c_str());
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // ── Export & Logging ──────────────────────────────────────────────
        if (ImGui::BeginTabItem("Export & Logging")) {
            ImGui::TextDisabled("Session Data Recording");
            ImGui::Spacing();
            if (m_settings.dataLoggingEnabled) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f,0.1f,0.1f,1));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f,0.2f,0.2f,1));
                if (ImGui::Button("  STOP MISSION LOGGING  ", ImVec2(-1, 35))) {
                    m_settings.dataLoggingEnabled = false;
                    pushSettingsToBlackboard();
                }
                ImGui::PopStyleColor(2);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f,0.5f,0.1f,1));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f,0.7f,0.1f,1));
                if (ImGui::Button("  START MISSION LOGGING  ", ImVec2(-1, 35))) {
                    m_settings.dataLoggingEnabled = true;
                    pushSettingsToBlackboard();
                }
                ImGui::PopStyleColor(2);
            }

            ImGui::Separator();
            ImGui::Columns(2, "logsettings", false);
            ImGui::SetColumnWidth(0, 120);

            ImGui::Text("Format:"); ImGui::NextColumn();
            static const char* kFmts[] = {"CSV (Table)", "JSON-Lines (Stream)"};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##logfmt", &m_settings.dataLoggingFormat, kFmts, 2))
                pushSettingsToBlackboard();
            ImGui::NextColumn();

            ImGui::Text("Frequency:"); ImGui::NextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##logfreq", &m_settings.dataLoggingFreqFrames, 1, 30, "%d frames"))
                pushSettingsToBlackboard();
            ImGui::NextColumn();

            ImGui::Text("Directory:"); ImGui::NextColumn();
            static char s_logDirBuf[256] = {0};
            if (s_logDirBuf[0] == '\0' && !m_settings.dataLoggingOutputDir.empty())
                strncpy(s_logDirBuf, m_settings.dataLoggingOutputDir.c_str(), 255);
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - 5);
            if (ImGui::InputText("##logdir", s_logDirBuf, sizeof(s_logDirBuf))) {
                m_settings.dataLoggingOutputDir = s_logDirBuf;
                pushSettingsToBlackboard();
            }
            ImGui::SameLine();
            if (ImGui::Button("...##logdirBtn", ImVec2(ImGui::GetFrameHeight(), 0)))
                m_log(LogLevel::INFO, "Logging directory: " + m_settings.dataLoggingOutputDir);
            ImGui::NextColumn();

            ImGui::Text("Export Dir:"); ImGui::NextColumn();
            static char s_expDirBuf[256] = {0};
            if (s_expDirBuf[0] == '\0' && !m_settings.exportOutputDir.empty())
                strncpy(s_expDirBuf, m_settings.exportOutputDir.c_str(), 255);
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - 5);
            if (ImGui::InputText("##exportdir", s_expDirBuf, sizeof(s_expDirBuf))) {
                m_settings.exportOutputDir = s_expDirBuf;
                pushSettingsToBlackboard();
            }
            ImGui::SameLine();
            if (ImGui::Button("...##expdirBtn", ImVec2(ImGui::GetFrameHeight(), 0)))
                m_log(LogLevel::INFO, "Export directory: " + m_settings.exportOutputDir);
            ImGui::NextColumn();

            ImGui::Columns(1);
            ImGui::Separator();

            if (m_dataLogger.isOpen()) {
                ImGui::TextColored(ImVec4(1,0.8f,0,1), "LIVE PATH: %s",
                                   m_dataLogger.getCurrentPath().c_str());
                ImGui::Text("RECORDS: %llu  |  SIZE: %.2f MB",
                            (unsigned long long)m_dataLogger.getRowsWritten(),
                            (float)m_dataLogger.getBytesWritten() / (1024.f * 1024.f));
            } else {
                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "STATUS: IDLE (No active log file)");
            }

            ImGui::Separator();
            if (ImGui::Button("Export Target History (JSON)", ImVec2(-1, 0)))
                exportTargetHistory(m_targetHistory);
            if (ImGui::Button("Take Screenshot (PNG)", ImVec2(-1, 0)))
                m_screenshotPending = true;

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
// renderZoomWindow
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::renderZoomWindow(const cv::Mat& zoomFrame, const TrackedTarget& locked) {
    ImGui::Begin("Target Zoom");

    if (locked.state != TrackingState::SEARCHING) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f,0.15f,0.15f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f,0.25f,0.25f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.5f,0.1f,0.1f,1));
        if (ImGui::Button("UNLOCK TARGET / RELEASE LOCK", ImVec2(-FLT_MIN, 28)))
            m_blackboard.requestTargetRelease();
        ImGui::PopStyleColor(3);
        ImGui::Spacing();
    }

    if (locked.state != TrackingState::SEARCHING && !zoomFrame.empty()) {
        m_zoomRenderer->updateTexture(zoomFrame);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 pos   = ImGui::GetCursorScreenPos();
        float fa = static_cast<float>(zoomFrame.cols) / zoomFrame.rows;
        float wa = avail.x / avail.y;
        float tw, th, dx, dy;
        if (wa > fa) { th=avail.y; tw=th*fa; dx=pos.x+(avail.x-tw)/2; dy=pos.y; }
        else         { tw=avail.x; th=tw/fa; dy=pos.y+(avail.y-th)/2; dx=pos.x; }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddImage(reinterpret_cast<void*>(static_cast<intptr_t>(m_zoomRenderer->getTextureID())),
                     ImVec2(dx,dy), ImVec2(dx+tw,dy+th));

        ImU32 hc = ApplyBrightnessLocal(m_settings.hudColor    ? m_settings.hudColor    : IM_COL32(0,200,100,220), m_settings.hudBrightness);
        ImU32 tc = ApplyBrightnessLocal(m_settings.targetColor ? m_settings.targetColor : IM_COL32(255,180,0,255),  m_settings.hudBrightness);

        // Crosshair reticle
        ImVec2 ctr(dx + tw/2, dy + th/2);
        float rs = 15.0f;
        dl->AddLine(ImVec2(ctr.x-rs,ctr.y), ImVec2(ctr.x-4,ctr.y), tc, 1.0f);
        dl->AddLine(ImVec2(ctr.x+4,ctr.y),  ImVec2(ctr.x+rs,ctr.y), tc, 1.0f);
        dl->AddLine(ImVec2(ctr.x,ctr.y-rs), ImVec2(ctr.x,ctr.y-4), tc, 1.0f);
        dl->AddLine(ImVec2(ctr.x,ctr.y+4),  ImVec2(ctr.x,ctr.y+rs), tc, 1.0f);
        dl->AddCircle(ctr, 6.0f, tc, 12, 1.0f);

        // Corner brackets
        float bl = 12.0f;
        dl->AddLine(ImVec2(dx,dy),       ImVec2(dx+bl,dy),      hc, 1.5f);
        dl->AddLine(ImVec2(dx,dy),       ImVec2(dx,dy+bl),      hc, 1.5f);
        dl->AddLine(ImVec2(dx+tw,dy),    ImVec2(dx+tw-bl,dy),   hc, 1.5f);
        dl->AddLine(ImVec2(dx+tw,dy),    ImVec2(dx+tw,dy+bl),   hc, 1.5f);
        dl->AddLine(ImVec2(dx,dy+th),    ImVec2(dx+bl,dy+th),   hc, 1.5f);
        dl->AddLine(ImVec2(dx,dy+th),    ImVec2(dx,dy+th-bl),   hc, 1.5f);
        dl->AddLine(ImVec2(dx+tw,dy+th), ImVec2(dx+tw-bl,dy+th),hc, 1.5f);
        dl->AddLine(ImVec2(dx+tw,dy+th), ImVec2(dx+tw,dy+th-bl),hc, 1.5f);

        // Info overlay
        char info[128];
        snprintf(info, sizeof(info), "TRK ID: %03d | CLASS: %s",
                 locked.track_id, locked.className.c_str());
        dl->AddText(ImVec2(dx+8,dy+8), hc, info);
        snprintf(info, sizeof(info), "CONF: %.2f | SIZE: %dx%d",
                 locked.confidence, zoomFrame.cols, zoomFrame.rows);
        dl->AddText(ImVec2(dx+8,dy+22), hc, info);
        snprintf(info, sizeof(info), "4K ZOOM: %s | MAG: %.1fx",
                 m_settings.enable4KZoom ? "ON" : "OFF",
                 m_settings.targetZoomMagnification);
        dl->AddText(ImVec2(dx+8,dy+36), hc, info);

    } else if (locked.state != TrackingState::SEARCHING) {
        // Locked but no valid frame
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 pos   = ImGui::GetCursorScreenPos();
        const char* txt = "INVALID TARGET BOUNDS";
        ImVec2 ts = ImGui::CalcTextSize(txt);
        ImGui::SetCursorScreenPos(ImVec2(pos.x+(avail.x-ts.x)*0.5f, pos.y+(avail.y-ts.y)*0.5f));
        ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "%s", txt);
    } else {
        // Standby grid
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 pos   = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 gc = IM_COL32(0,200,100,20);
        for (float x = pos.x; x < pos.x+avail.x; x += 30) dl->AddLine(ImVec2(x,pos.y),ImVec2(x,pos.y+avail.y),gc);
        for (float y = pos.y; y < pos.y+avail.y; y += 30) dl->AddLine(ImVec2(pos.x,y),ImVec2(pos.x+avail.x,y),gc);
        const char* txt = "ZOOM STANDBY\nNO TARGET LOCKED";
        ImVec2 ts = ImGui::CalcTextSize(txt);
        ImGui::SetCursorScreenPos(ImVec2(pos.x+(avail.x-ts.x)*0.5f, pos.y+(avail.y-ts.y)*0.5f));
        ImGui::TextColored(ImVec4(0,0.8f,0.4f,0.6f), "%s", txt);
    }

    ImGui::End();
}


// ─────────────────────────────────────────────────────────────────────────────
// renderDossierPanel
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::renderDossierPanel(const DossierState& dossier) {
    if (!m_showDossierPanel) return;

    ImGui::Begin("AI Dossier", &m_showDossierPanel);

    if (!dossier.hasActiveDossier) {
        ImGui::TextWrapped("Select and LOCK a stable target to view its persistent dossier.");
        if (dossier.queueSize > 0) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "AI Processing Queue: %d", dossier.queueSize);
        }
        ImGui::End();
        return;
    }

    const auto& entry = dossier.activeDossier;

    ImGui::TextColored(ImVec4(0, 0.8f, 1, 1), "ID: %s", entry.uuid.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Type: %s", entry.type.c_str());
    ImGui::Separator();

    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "FIRST SEEN:");
    ImGui::Text("%s", entry.first_seen.c_str());
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f);
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "SIGHTINGS:");
    ImGui::Text("%d times", entry.sightings_count);
    ImGui::EndGroup();

    ImGui::Separator();
    
    ImGui::TextColored(ImVec4(0, 1, 0, 1), "DOSSIER DATA:");
    if (dossier.updatePending || entry.dossier_text == "Analysis pending...") {
        ImGui::TextDisabled("Requesting AI analysis... (Queue: %d)", dossier.queueSize);
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.1f, 0.12f, 1.0f));
    ImGui::BeginChild("DossierText", ImVec2(0, 0), true);
    ImGui::TextWrapped("%s", entry.dossier_text.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::End();
}
