#include "UIManager.hpp"
#include "UIManager_internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
// renderCameraView
// Source: extracted from Application::run() lines 3656–4487
//
// API substitutions:
//   m_roiManager->           → m_roiManager. (reference, not pointer)
//   m_lockRequested / m_requestedLockId  → m_blackboard.requestTargetLock(id)
//   m_pixelLockRequested     → m_blackboard.requestPixelLock(point, rect)
//   m_pixelLockDragging      → m_blackboard.setPixelLockDragging(true/false)
//   m_pixelLockRect (during drag) → m_lockedTarget.box for visual feedback,
//                             committed on release via m_blackboard.requestPixelLockRectUpdate()
//   m_sharedLockedTarget.box → not written (Blackboard is source of truth)
//   log(...)                 → m_log(...)
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::renderCameraView(const cv::Mat& currentFrame,
                                 const std::vector<TrackedObject>& tracked,
                                 const TrackedTarget& locked,
                                 const std::vector<cv::Rect>& motionRegions,
                                 const VisionState& vision) {
    ImGui::Begin("Camera View", nullptr,
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ViewportInfo view = {};

    if (m_renderer->getTextureID() != 0 && m_cameraWidth > 0 && m_cameraHeight > 0) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 pos   = ImGui::GetCursorScreenPos();

        float fa = (float)m_cameraWidth / (float)m_cameraHeight;
        float wa = avail.x / avail.y;
        if (wa > fa) {
            view.target_h = avail.y;
            view.target_w = view.target_h * fa;
            view.pos_x    = pos.x + (avail.x - view.target_w) / 2.0f;
            view.pos_y    = pos.y;
            view.scale    = view.target_h / (float)m_cameraHeight;
        } else {
            view.target_w = avail.x;
            view.target_h = view.target_w / fa;
            view.pos_y    = pos.y + (avail.y - view.target_h) / 2.0f;
            view.pos_x    = pos.x;
            view.scale    = view.target_w / (float)m_cameraWidth;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddImage(reinterpret_cast<void*>((intptr_t)m_renderer->getTextureID()),
                     ImVec2(view.pos_x, view.pos_y),
                     ImVec2(view.pos_x + view.target_w, view.pos_y + view.target_h));

        if (m_settings.motionDetectionEnabled && m_settings.motionHeatmapOverlay &&
            m_heatmapRenderer->getTextureID() != 0) {
            dl->AddImage(reinterpret_cast<void*>((intptr_t)m_heatmapRenderer->getTextureID()),
                         ImVec2(view.pos_x, view.pos_y),
                         ImVec2(view.pos_x + view.target_w, view.pos_y + view.target_h));
        }

        // ── ROI & Pixel-Lock Interaction ──────────────────────────────────
        int hoveredZoneId       = -1;
        ROIEditState hoveredAction = ROIEditState::NONE;

        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_None)) {
            ImVec2 mpos = ImGui::GetMousePos();
            cv::Point mVideo((int)((mpos.x - view.pos_x) / view.scale),
                             (int)((mpos.y - view.pos_y) / view.scale));

            auto zones = m_roiManager.getROIs();
            double tol = std::max(4.0, 8.0 / view.scale);

            if (m_editState == ROIEditState::NONE) {
                // Pixel target corners (id 999)
                if (locked.state != TrackingState::SEARCHING && locked.className == "Pixel Target") {
                    cv::Rect r = locked.box;
                    auto checkCorner = [&](cv::Point p, ROIEditState action) {
                        if (std::hypot(mVideo.x - p.x, mVideo.y - p.y) <= tol) {
                            hoveredZoneId = 999; hoveredAction = action;
                        }
                    };
                    checkCorner({r.x,           r.y},            ROIEditState::RESIZING_TL);
                    if (hoveredZoneId == -1) checkCorner({r.x+r.width, r.y},            ROIEditState::RESIZING_TR);
                    if (hoveredZoneId == -1) checkCorner({r.x,          r.y+r.height},  ROIEditState::RESIZING_BL);
                    if (hoveredZoneId == -1) checkCorner({r.x+r.width,  r.y+r.height},  ROIEditState::RESIZING_BR);
                }

                // ROI zone corners
                if (hoveredZoneId == -1) {
                    for (const auto& z : zones) {
                        cv::Rect r = z.rect;
                        if (std::hypot(mVideo.x-r.x,       mVideo.y-r.y)           <= tol) { hoveredZoneId=z.id; hoveredAction=ROIEditState::RESIZING_TL; break; }
                        if (std::hypot(mVideo.x-r.x-r.width, mVideo.y-r.y)         <= tol) { hoveredZoneId=z.id; hoveredAction=ROIEditState::RESIZING_TR; break; }
                        if (std::hypot(mVideo.x-r.x,       mVideo.y-r.y-r.height)  <= tol) { hoveredZoneId=z.id; hoveredAction=ROIEditState::RESIZING_BL; break; }
                        if (std::hypot(mVideo.x-r.x-r.width, mVideo.y-r.y-r.height)<= tol) { hoveredZoneId=z.id; hoveredAction=ROIEditState::RESIZING_BR; break; }
                    }
                }

                // Pixel target edges
                if (hoveredZoneId == -1 && locked.state != TrackingState::SEARCHING && locked.className == "Pixel Target") {
                    cv::Rect r = locked.box;
                    if      (std::abs(mVideo.x-r.x)         <=tol && mVideo.y>=r.y && mVideo.y<=r.y+r.height) { hoveredZoneId=999; hoveredAction=ROIEditState::RESIZING_L; }
                    else if (std::abs(mVideo.x-r.x-r.width) <=tol && mVideo.y>=r.y && mVideo.y<=r.y+r.height) { hoveredZoneId=999; hoveredAction=ROIEditState::RESIZING_R; }
                    else if (std::abs(mVideo.y-r.y)         <=tol && mVideo.x>=r.x && mVideo.x<=r.x+r.width)  { hoveredZoneId=999; hoveredAction=ROIEditState::RESIZING_T; }
                    else if (std::abs(mVideo.y-r.y-r.height)<=tol && mVideo.x>=r.x && mVideo.x<=r.x+r.width)  { hoveredZoneId=999; hoveredAction=ROIEditState::RESIZING_B; }
                }

                // ROI zone edges
                if (hoveredZoneId == -1) {
                    for (const auto& z : zones) {
                        cv::Rect r = z.rect;
                        if      (std::abs(mVideo.x-r.x)         <=tol && mVideo.y>=r.y && mVideo.y<=r.y+r.height) { hoveredZoneId=z.id; hoveredAction=ROIEditState::RESIZING_L; break; }
                        else if (std::abs(mVideo.x-r.x-r.width) <=tol && mVideo.y>=r.y && mVideo.y<=r.y+r.height) { hoveredZoneId=z.id; hoveredAction=ROIEditState::RESIZING_R; break; }
                        else if (std::abs(mVideo.y-r.y)         <=tol && mVideo.x>=r.x && mVideo.x<=r.x+r.width)  { hoveredZoneId=z.id; hoveredAction=ROIEditState::RESIZING_T; break; }
                        else if (std::abs(mVideo.y-r.y-r.height)<=tol && mVideo.x>=r.x && mVideo.x<=r.x+r.width)  { hoveredZoneId=z.id; hoveredAction=ROIEditState::RESIZING_B; break; }
                    }
                }

                // Center (moving)
                if (hoveredZoneId == -1 && locked.state != TrackingState::SEARCHING && locked.className == "Pixel Target") {
                    cv::Rect r = locked.box;
                    if (mVideo.x>r.x && mVideo.x<r.x+r.width && mVideo.y>r.y && mVideo.y<r.y+r.height)
                        { hoveredZoneId=999; hoveredAction=ROIEditState::MOVING; }
                }
                if (hoveredZoneId == -1) {
                    for (const auto& z : zones) {
                        cv::Rect r = z.rect;
                        if (mVideo.x>r.x && mVideo.x<r.x+r.width && mVideo.y>r.y && mVideo.y<r.y+r.height)
                            { hoveredZoneId=z.id; hoveredAction=ROIEditState::MOVING; break; }
                    }
                }
            } else {
                hoveredZoneId = m_editZoneId;
                hoveredAction = m_editState;
            }

            // Cursor shape
            if (m_editState == ROIEditState::DRAWING) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            } else {
                switch ((m_editState == ROIEditState::NONE) ? hoveredAction : m_editState) {
                    case ROIEditState::RESIZING_TL: case ROIEditState::RESIZING_BR:
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
                    case ROIEditState::RESIZING_TR: case ROIEditState::RESIZING_BL:
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
                    case ROIEditState::RESIZING_L: case ROIEditState::RESIZING_R:
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
                    case ROIEditState::RESIZING_T: case ROIEditState::RESIZING_B:
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); break;
                    case ROIEditState::MOVING:
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll); break;
                    default:
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow); break;
                }
            }

            // ── Mouse Down: begin edit or lock target ─────────────────────
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (hoveredAction != ROIEditState::NONE) {
                    m_editState        = hoveredAction;
                    m_editZoneId       = hoveredZoneId;
                    m_editDragStartMouse = mVideo;
                    if (m_editZoneId == 999) {
                        m_editDragStartRect = locked.box;
                        m_blackboard.setPixelLockDragging(true);
                    } else {
                        for (const auto& z : zones) {
                            if (z.id == m_editZoneId) { m_editDragStartRect = z.rect; break; }
                        }
                    }
                } else {
                    // Click on tracked object → lock
                    bool lockedOnObj = false;
                    for (const auto& obj : tracked) {
                        if (obj.box.contains(mVideo)) {
                            m_blackboard.requestTargetLock(obj.track_id);
                            m_selectedAnalyzerTargetId = obj.track_id;
                            m_log(LogLevel::INFO, "Target lock requested: " + obj.className +
                                  " TrackID=" + std::to_string(obj.track_id));
                            lockedOnObj = true;
                            break;
                        }
                    }
                    if (!lockedOnObj) {
                        m_editState = ROIEditState::DRAWING;
                        m_editDragStartMouse = mVideo;
                        m_roiManager.beginDrag(mVideo);
                    }
                }
            }

            // ── Mouse Drag ────────────────────────────────────────────────
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (m_editState == ROIEditState::DRAWING && m_roiManager.isDragging()) {
                    m_roiManager.updateDrag(mVideo);
                } else if (m_editState != ROIEditState::NONE && m_editZoneId != -1) {
                    int dx = mVideo.x - m_editDragStartMouse.x;
                    int dy = mVideo.y - m_editDragStartMouse.y;
                    int cols = m_cameraWidth, rows = m_cameraHeight;
                    cv::Rect nr = m_editDragStartRect;

                    auto clamp = [](int v, int lo, int hi){ return std::max(lo, std::min(hi, v)); };

                    switch (m_editState) {
                        case ROIEditState::MOVING: {
                            int x = clamp(m_editDragStartRect.x+dx, 0, cols-m_editDragStartRect.width);
                            int y = clamp(m_editDragStartRect.y+dy, 0, rows-m_editDragStartRect.height);
                            nr = {x, y, m_editDragStartRect.width, m_editDragStartRect.height};
                            break;
                        }
                        case ROIEditState::RESIZING_TL: {
                            int x1 = clamp(m_editDragStartRect.x+dx, 0, m_editDragStartRect.x+m_editDragStartRect.width-4);
                            int y1 = clamp(m_editDragStartRect.y+dy, 0, m_editDragStartRect.y+m_editDragStartRect.height-4);
                            nr = {x1, y1, (m_editDragStartRect.x+m_editDragStartRect.width)-x1, (m_editDragStartRect.y+m_editDragStartRect.height)-y1};
                            break;
                        }
                        case ROIEditState::RESIZING_TR: {
                            int x2 = clamp(m_editDragStartRect.x+m_editDragStartRect.width+dx, m_editDragStartRect.x+4, cols);
                            int y1 = clamp(m_editDragStartRect.y+dy, 0, m_editDragStartRect.y+m_editDragStartRect.height-4);
                            nr = {m_editDragStartRect.x, y1, x2-m_editDragStartRect.x, (m_editDragStartRect.y+m_editDragStartRect.height)-y1};
                            break;
                        }
                        case ROIEditState::RESIZING_BL: {
                            int x1 = clamp(m_editDragStartRect.x+dx, 0, m_editDragStartRect.x+m_editDragStartRect.width-4);
                            int y2 = clamp(m_editDragStartRect.y+m_editDragStartRect.height+dy, m_editDragStartRect.y+4, rows);
                            nr = {x1, m_editDragStartRect.y, (m_editDragStartRect.x+m_editDragStartRect.width)-x1, y2-m_editDragStartRect.y};
                            break;
                        }
                        case ROIEditState::RESIZING_BR: {
                            int x2 = clamp(m_editDragStartRect.x+m_editDragStartRect.width+dx, m_editDragStartRect.x+4, cols);
                            int y2 = clamp(m_editDragStartRect.y+m_editDragStartRect.height+dy, m_editDragStartRect.y+4, rows);
                            nr = {m_editDragStartRect.x, m_editDragStartRect.y, x2-m_editDragStartRect.x, y2-m_editDragStartRect.y};
                            break;
                        }
                        case ROIEditState::RESIZING_L: {
                            int x1 = clamp(m_editDragStartRect.x+dx, 0, m_editDragStartRect.x+m_editDragStartRect.width-4);
                            nr = {x1, m_editDragStartRect.y, (m_editDragStartRect.x+m_editDragStartRect.width)-x1, m_editDragStartRect.height};
                            break;
                        }
                        case ROIEditState::RESIZING_R: {
                            int x2 = clamp(m_editDragStartRect.x+m_editDragStartRect.width+dx, m_editDragStartRect.x+4, cols);
                            nr = {m_editDragStartRect.x, m_editDragStartRect.y, x2-m_editDragStartRect.x, m_editDragStartRect.height};
                            break;
                        }
                        case ROIEditState::RESIZING_T: {
                            int y1 = clamp(m_editDragStartRect.y+dy, 0, m_editDragStartRect.y+m_editDragStartRect.height-4);
                            nr = {m_editDragStartRect.x, y1, m_editDragStartRect.width, (m_editDragStartRect.y+m_editDragStartRect.height)-y1};
                            break;
                        }
                        case ROIEditState::RESIZING_B: {
                            int y2 = clamp(m_editDragStartRect.y+m_editDragStartRect.height+dy, m_editDragStartRect.y+4, rows);
                            nr = {m_editDragStartRect.x, m_editDragStartRect.y, m_editDragStartRect.width, y2-m_editDragStartRect.y};
                            break;
                        }
                        default: break;
                    }

                    if (m_editZoneId == 999) {
                        // Clamp to frame
                        nr.x = std::max(0, std::min(nr.x, cols-4));
                        nr.y = std::max(0, std::min(nr.y, rows-4));
                        nr.width  = std::max(4, std::min(nr.width,  cols-nr.x));
                        nr.height = std::max(4, std::min(nr.height, rows-nr.y));
                        // Update local locked target for visual feedback (frame-level, not shared)
                        m_lockedTarget.box = nr;
                    } else {
                        m_roiManager.updateRect(m_editZoneId, nr);
                    }
                }
            }

            // ── Mouse Released ────────────────────────────────────────────
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (m_editState == ROIEditState::DRAWING) {
                    int dx = std::abs(mVideo.x - m_editDragStartMouse.x);
                    int dy = std::abs(mVideo.y - m_editDragStartMouse.y);

                    if (dx < 5 && dy < 5) {
                        // Point click → pixel template lock (60×60 px around click)
                        m_roiManager.cancelDrag();
                        const int ts = 60;
                        cv::Point pt = m_editDragStartMouse;
                        m_blackboard.requestPixelLock(cv::Rect(pt.x - ts/2, pt.y - ts/2, ts, ts));
                    } else {
                        if (m_roiEditMode) {
                            // Create ROI zone
                            int newId = m_roiManager.commitDrag();
                            if (newId >= 0) {
                                int bufIdx = newId % ROIManager::kMaxZones;
                                for (const auto& z : m_roiManager.getROIs()) {
                                    if (z.id == newId) {
                                        strncpy(m_roiLabelBuf[bufIdx], z.label.c_str(), 63);
                                        break;
                                    }
                                }
                                m_log(LogLevel::INFO, "ROI zone added: ID " + std::to_string(newId));
                            }
                            if ((int)m_roiManager.getROIs().size() >= ROIManager::kMaxZones)
                                m_roiEditMode = false;
                        } else {
                            // Draw → pixel template rect
                            cv::Rect dr = m_roiManager.getDragRect();
                            m_roiManager.cancelDrag();
                            if (dr.width > 4 && dr.height > 4)
                                m_blackboard.requestPixelLock(dr);
                        }
                    }
                } else if (m_editZoneId == 999) {
                    // Commit pixel target drag/resize
                    m_blackboard.setPixelLockDragging(false);
                    m_blackboard.requestPixelLockRectUpdate(m_lockedTarget.box);
                    m_log(LogLevel::INFO, "Pixel target updated via drag: (" +
                          std::to_string(m_lockedTarget.box.x) + "," +
                          std::to_string(m_lockedTarget.box.y) + "," +
                          std::to_string(m_lockedTarget.box.width) + "x" +
                          std::to_string(m_lockedTarget.box.height) + ")");
                }
                m_editState  = ROIEditState::NONE;
                m_editZoneId = -1;
            }

            // Right-click: cancel drag
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                if (m_editState == ROIEditState::DRAWING && m_roiManager.isDragging()) {
                    m_roiManager.cancelDrag();
                } else if (m_editZoneId == 999) {
                    m_blackboard.setPixelLockDragging(false);
                    m_lockedTarget.box = m_editDragStartRect; // local revert
                }
                m_editState  = ROIEditState::NONE;
                m_editZoneId = -1;
            }

            // Draw in-progress drag rectangle
            if (m_editState == ROIEditState::DRAWING && m_roiManager.isDragging()) {
                cv::Rect dr = m_roiManager.getDragRect();
                ImVec2 rMin(view.pos_x + dr.x * view.scale,
                            view.pos_y + dr.y * view.scale);
                ImVec2 rMax(view.pos_x + (dr.x+dr.width)  * view.scale,
                            view.pos_y + (dr.y+dr.height) * view.scale);
                dl->AddRect(rMin, rMax, IM_COL32(255,220,0,220), 0, 0, 2.f);
                dl->AddRectFilled(rMin, rMax, IM_COL32(255,220,0,25));
            }
        }

        // ── ROI Zone Overlay ──────────────────────────────────────────────
        if (m_settings.showROIOverlay) {
            auto zones = m_roiManager.getROIs();
            float appSec = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - m_appStart).count();

            for (const auto& z : zones) {
                bool hov = (hoveredZoneId == z.id);
                bool edt = (m_editZoneId  == z.id);

                bool hasObj = false;
                if (z.active && z.function == ROIFunction::ALARM) {
                    for (const auto& obj : tracked) {
                        cv::Point ctr(obj.box.x+obj.box.width/2, obj.box.y+obj.box.height/2);
                        if (z.rect.contains(ctr)) { hasObj = true; break; }
                    }
                }

                ImU32 col = IM_COL32(150,150,150,120);
                ImU32 fill = IM_COL32(150,150,150,10);
                float bw = (hov || edt) ? 2.5f : 1.5f;

                if (z.active) {
                    if (z.function == ROIFunction::DETECTION)
                        { col = IM_COL32(100,255,80,200);  fill = IM_COL32(100,255,80,18); }
                    else if (z.function == ROIFunction::EXCLUDE)
                        { col = IM_COL32(240,140,30,200);  fill = IM_COL32(240,140,30,15); }
                    else if (z.function == ROIFunction::ALARM) {
                        if (hasObj) {
                            bool flash = ((int)(appSec * 4.0f) % 2 == 0);
                            col  = flash ? IM_COL32(255,0,0,255) : IM_COL32(150,0,0,220);
                            fill = IM_COL32(255,0,0,flash?35:15);
                            bw   = 3.0f;
                        } else {
                            col  = IM_COL32(220,50,50,180);
                            fill = IM_COL32(220,50,50,15);
                        }
                    }
                }
                if (hov || edt) col = IM_COL32(255,255,0,255);

                ImVec2 rMin(view.pos_x + z.rect.x * view.scale,
                            view.pos_y + z.rect.y * view.scale);
                ImVec2 rMax(view.pos_x + (z.rect.x+z.rect.width)  * view.scale,
                            view.pos_y + (z.rect.y+z.rect.height) * view.scale);

                dl->AddRect(rMin, rMax, col, 0, 0, bw);
                dl->AddRectFilled(rMin, rMax, fill);

                if (hov || edt) {
                    ImVec2 tl(rMin.x,rMin.y), tr(rMax.x,rMin.y),
                           bl(rMin.x,rMax.y), br(rMax.x,rMax.y);
                    auto square = [&](ImVec2 p){
                        dl->AddRectFilled(ImVec2(p.x-3,p.y-3),ImVec2(p.x+3,p.y+3),col);
                    };
                    square(tl); square(tr); square(bl); square(br);
                }

                const char* fp = "";
                if (z.function == ROIFunction::DETECTION) fp = "[DET] ";
                else if (z.function == ROIFunction::EXCLUDE) fp = "[EXC] ";
                else if (z.function == ROIFunction::ALARM)
                    fp = hasObj ? "[ALARM TRIGGERED] " : "[ALM] ";

                char lbl[120];
                snprintf(lbl, sizeof(lbl), "%s[%d] %s%s",
                         fp, z.id, z.label.c_str(), z.active ? "" : " (off)");
                dl->AddText(ImVec2(rMin.x+4,rMin.y+3), col, lbl);
            }
        }

        // ── Pixel Target Handles ──────────────────────────────────────────
        if (locked.state != TrackingState::SEARCHING && locked.className == "Pixel Target") {
            bool hov = (hoveredZoneId == 999);
            bool edt = (m_editZoneId  == 999);
            ImU32 col = (hov || edt) ? IM_COL32(255,255,0,255) : IM_COL32(255,50,50,255);
            float bw  = (hov || edt) ? 2.5f : 0.f;

            const cv::Rect& r = m_lockedTarget.box; // Use local (may be mid-drag)
            ImVec2 rMin(view.pos_x + r.x * view.scale,
                        view.pos_y + r.y * view.scale);
            ImVec2 rMax(view.pos_x + (r.x+r.width)  * view.scale,
                        view.pos_y + (r.y+r.height) * view.scale);

            if (bw > 0.f) dl->AddRect(rMin, rMax, col, 0, 0, bw);
            dl->AddRectFilled(rMin, rMax, IM_COL32(255,50,50,25));

            if (hov || edt) {
                auto sq = [&](ImVec2 p){ dl->AddRectFilled({p.x-3,p.y-3},{p.x+3,p.y+3},col); };
                sq(rMin); sq({rMax.x,rMin.y}); sq({rMin.x,rMax.y}); sq(rMax);
            }

            char lbl[64];
            snprintf(lbl, sizeof(lbl), "[PIXEL TARGET] ID:%d", locked.track_id);
            dl->AddText(ImVec2(rMin.x+4,rMin.y+3), col, lbl);
        }

        // ── Motion Overlay ────────────────────────────────────────────────
        if (m_settings.motionDetectionEnabled && m_settings.motionShowOverlay)
            m_hud->drawMotionOverlay(dl, motionRegions, view, m_settings);

        // ── HUD (tracks, crosshair, status) ───────────────────────────────
        m_hud->render(dl, (int)avail.x, (int)avail.y,
                      m_cameraFps, tracked, locked, view, m_settings);

        // ── Audio Visualizer (integrated into camera image) ────────────────
        if (m_audioVisualizerPanel) {
            m_audioVisualizerPanel->draw(m_audioEngine, m_blackboard, &view);
        }

        // ── Sub-Zoom Inserts (inline in Camera View) ──────────────────────
        if (m_settings.subZoomsEnabled && !m_settings.subZoomsUseSeparateWindows) {
            float insert_w = 120.f, insert_h = 120.f, margin = 20.f;
            float sf = std::min(1.0f, view.target_w / 640.0f);
            insert_w *= sf; insert_h *= sf; margin *= sf;

            ImU32 activeC  = m_settings.hudColor ? ApplyBrightnessLocal(m_settings.hudColor, m_settings.hudBrightness) : IM_COL32(0,200,100,220);
            ImU32 holdC    = IM_COL32(255,120,0,220);
            ImU32 lineA    = m_settings.hudColor ? ApplyBrightnessLocal(m_settings.hudColor, m_settings.hudBrightness*0.7f) : IM_COL32(0,200,100,150);
            ImU32 lineH    = IM_COL32(255,120,0,100);

            // Obstacle list for collision avoidance
            struct Box { float x1,y1,x2,y2; };
            std::vector<Box> obstacles;
            for (const auto& obj : tracked) {
                float ox1=view.pos_x+obj.box.x*view.scale, oy1=view.pos_y+obj.box.y*view.scale;
                float p=12.f;
                obstacles.push_back({ox1-p,oy1-p,ox1+obj.box.width*view.scale+p,oy1+obj.box.height*view.scale+p});
            }
            if (locked.state != TrackingState::SEARCHING) {
                float ox1=view.pos_x+locked.box.x*view.scale,oy1=view.pos_y+locked.box.y*view.scale;
                float p=12.f;
                obstacles.push_back({ox1-p,oy1-p,ox1+locked.box.width*view.scale+p,oy1+locked.box.height*view.scale+p});
            }
            // HUD status windows
            obstacles.push_back({view.pos_x+margin-5, view.pos_y+margin-5,
                                  view.pos_x+margin+180*sf+5, view.pos_y+margin+100*sf+5});
            obstacles.push_back({view.pos_x+margin-5, view.pos_y+view.target_h-60*sf-margin-5,
                                  view.pos_x+margin+180*sf+5, view.pos_y+view.target_h-margin+5});

            std::vector<Box> placed;
            float min_x=view.pos_x+margin, max_x=view.pos_x+view.target_w-insert_w-margin;
            float min_y=view.pos_y+margin, max_y=view.pos_y+view.target_h-insert_h-margin;

            auto closestOnRect = [](ImVec2 mn, ImVec2 mx, ImVec2 p) -> ImVec2 {
                return {std::clamp(p.x,mn.x,mx.x), std::clamp(p.y,mn.y,mx.y)};
            };

            for (int i = 0; i < 4; ++i) {
                if (!m_subZooms[i].active) continue;

                ImVec2 pos;
                if (max_x < min_x || max_y < min_y) {
                    // Tiny viewport fallback
                    if      (i==0) pos={view.pos_x+margin, view.pos_y+margin+110*sf};
                    else if (i==1) pos={view.pos_x+view.target_w-insert_w-margin, view.pos_y+margin};
                    else if (i==2) pos={view.pos_x+margin, view.pos_y+view.target_h-insert_h-margin-70*sf};
                    else           pos={view.pos_x+view.target_w-insert_w-margin, view.pos_y+view.target_h-insert_h-margin};
                } else {
                    ImVec2 defPos;
                    if      (i==0) defPos={min_x,min_y};
                    else if (i==1) defPos={max_x,min_y};
                    else if (i==2) defPos={min_x,max_y};
                    else           defPos={max_x,max_y};

                    std::vector<ImVec2> cands;
                    cands.push_back(defPos);
                    float step=25.f;
                    if      (i==0){ for(float y=min_y+step;y<=max_y;y+=step) cands.push_back({min_x,y});
                                    for(float x=min_x+step;x<=max_x;x+=step) cands.push_back({x,min_y}); }
                    else if (i==1){ for(float y=min_y+step;y<=max_y;y+=step) cands.push_back({max_x,y});
                                    for(float x=max_x-step;x>=min_x;x-=step) cands.push_back({x,min_y}); }
                    else if (i==2){ for(float y=max_y-step;y>=min_y;y-=step) cands.push_back({min_x,y});
                                    for(float x=min_x+step;x<=max_x;x+=step) cands.push_back({x,max_y}); }
                    else          { for(float y=max_y-step;y>=min_y;y-=step) cands.push_back({max_x,y});
                                    for(float x=max_x-step;x>=min_x;x-=step) cands.push_back({x,max_y}); }

                    ImVec2 bestPos = defPos; float minOvlp = -1.f;
                    for (const auto& cand : cands) {
                        float cx1=cand.x, cy1=cand.y, cx2=cx1+insert_w, cy2=cy1+insert_h;
                        float total=0.f;
                        for (const auto& ob : obstacles) {
                            float ox1=std::max(cx1,ob.x1),oy1=std::max(cy1,ob.y1);
                            float ox2=std::min(cx2,ob.x2),oy2=std::min(cy2,ob.y2);
                            if(ox2>ox1&&oy2>oy1) total+=(ox2-ox1)*(oy2-oy1);
                        }
                        for (const auto& pz : placed) {
                            float ox1=std::max(cx1,pz.x1),oy1=std::max(cy1,pz.y1);
                            float ox2=std::min(cx2,pz.x2),oy2=std::min(cy2,pz.y2);
                            if(ox2>ox1&&oy2>oy1) total+=(ox2-ox1)*(oy2-oy1);
                        }
                        if(total==0.f){ bestPos=cand; minOvlp=0.f; break; }
                        if(minOvlp<0.f||total<minOvlp){ minOvlp=total; bestPos=cand; }
                    }
                    pos = bestPos;
                    placed.push_back({pos.x, pos.y, pos.x+insert_w, pos.y+insert_h});
                }

                ImVec2 mCtr(view.pos_x+(m_subZooms[i].box.x+m_subZooms[i].box.width/2.f)*view.scale,
                            view.pos_y+(m_subZooms[i].box.y+m_subZooms[i].box.height/2.f)*view.scale);
                ImVec2 startPt = closestOnRect(pos, {pos.x+insert_w,pos.y+insert_h}, mCtr);
                ImU32 lc = m_subZooms[i].isLost ? lineH : lineA;
                ImU32 bc = m_subZooms[i].isLost ? holdC : activeC;

                // Leader line
                if (m_subZooms[i].isLost) {
                    float dx=mCtr.x-startPt.x, dy=mCtr.y-startPt.y;
                    float len=std::hypot(dx,dy);
                    if(len>1.f){ dx/=len; dy/=len;
                        float dist=0.f;
                        while(dist<len){ float nd=std::min(len,dist+6.f);
                            dl->AddLine({startPt.x+dx*dist,startPt.y+dy*dist},
                                        {startPt.x+dx*nd,  startPt.y+dy*nd},lc,1.f);
                            dist+=10.f; }
                    }
                } else {
                    dl->AddLine(startPt, mCtr, lc, 1.f);
                }

                // Target marker
                ImVec2 tMin(view.pos_x+m_subZooms[i].box.x*view.scale, view.pos_y+m_subZooms[i].box.y*view.scale);
                ImVec2 tMax(tMin.x+m_subZooms[i].box.width*view.scale, tMin.y+m_subZooms[i].box.height*view.scale);
                dl->AddRect(tMin, tMax, bc, 0, 0, 2.5f);
                float mr = std::max(4.f, std::min(40.f*sf, std::min(tMax.x-tMin.x,tMax.y-tMin.y)*0.4f));
                dl->AddCircle(mCtr, mr, bc, 16, 2.5f);

                // Insert image
                if (m_subZoomRenderers[i]->getTextureID() != 0)
                    dl->AddImage(reinterpret_cast<void*>((intptr_t)m_subZoomRenderers[i]->getTextureID()),
                                 pos, {pos.x+insert_w, pos.y+insert_h});
                dl->AddRect(pos, {pos.x+insert_w, pos.y+insert_h}, bc, 0, 0, 2.5f);

                char slotName[32];
                snprintf(slotName, sizeof(slotName), "M-%02d", m_subZooms[i].motion_id);
                dl->AddRectFilled({pos.x+2,pos.y+2},{pos.x+40*sf,pos.y+16*sf},IM_COL32(0,0,0,180));
                dl->AddText({pos.x+4,pos.y+2}, bc, slotName);

                if (m_subZooms[i].isLost) {
                    const char* ht = "HOLD";
                    ImVec2 ts = ImGui::CalcTextSize(ht);
                    float hx = pos.x + insert_w - ts.x - 4.f;
                    float hy = pos.y + 2.f;
                    dl->AddRectFilled({hx-2,hy},{pos.x+insert_w-2,hy+16*sf},IM_COL32(0,0,0,180));
                    dl->AddText({hx,hy}, holdC, ht);
                }
            }
        }
    }
    ImGui::End();

    // ── Sub-Zoom Separate Windows ─────────────────────────────────────────
    if (m_settings.subZoomsEnabled && m_settings.subZoomsUseSeparateWindows) {
        for (int i = 0; i < 4; ++i) {
            if (!m_subZooms[i].active) continue;
            char wname[64];
            snprintf(wname, sizeof(wname), "Sub Zoom %d (M-%02d)", i+1, m_subZooms[i].motion_id);
            ImGui::SetNextWindowSize(ImVec2(180, 220), ImGuiCond_FirstUseEver);
            ImGui::Begin(wname);
            if (m_subZoomRenderers[i]->getTextureID() != 0) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                float sz = std::min(avail.x, avail.y - 30.f);
                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImDrawList* wdl = ImGui::GetWindowDrawList();
                wdl->AddImage(reinterpret_cast<void*>((intptr_t)m_subZoomRenderers[i]->getTextureID()),
                              pos, {pos.x+sz, pos.y+sz});
                ImU32 bc = m_subZooms[i].isLost ? IM_COL32(255,120,0,220)
                    : (m_settings.hudColor ? ApplyBrightnessLocal(m_settings.hudColor, m_settings.hudBrightness)
                                           : IM_COL32(0,200,100,220));
                wdl->AddRect(pos, {pos.x+sz,pos.y+sz}, bc, 0, 0, 2.5f);
                ImGui::Dummy(ImVec2(sz, sz));
                if (m_subZooms[i].isLost) ImGui::TextColored(ImVec4(1,.5f,0,1), "STATUS: HOLDING");
                else                       ImGui::TextColored(ImVec4(0,.8f,.4f,1),"STATUS: TRACKING");
                ImGui::Text("Box: %d,%d %dx%d",
                            m_subZooms[i].box.x, m_subZooms[i].box.y,
                            m_subZooms[i].box.width, m_subZooms[i].box.height);
            } else {
                ImGui::Text("No Frame Available");
            }
            ImGui::End();
        }
    }
}
