#include "HUD.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>

static float GetTimeSeconds() {
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - start).count();
}

// Apply brightness to an ImU32 color
static ImU32 ApplyBrightness(ImU32 col, float brightness) {
    float r = ((col >>  0) & 0xFF) / 255.0f * brightness;
    float g = ((col >>  8) & 0xFF) / 255.0f * brightness;
    float b = ((col >> 16) & 0xFF) / 255.0f * brightness;
    float a = ((col >> 24) & 0xFF) / 255.0f;
    return IM_COL32(
        static_cast<int>(std::min(r, 1.0f) * 255),
        static_cast<int>(std::min(g, 1.0f) * 255),
        static_cast<int>(std::min(b, 1.0f) * 255),
        static_cast<int>(a * 255)
    );
}

HUD::HUD() {
    m_hudColor    = IM_COL32(0, 200, 100, 220);
    m_targetColor = IM_COL32(255, 180, 0, 255);
    m_motionColor = kDefaultMotionColor;
    m_startTime   = GetTimeSeconds();
}

HUD::~HUD() {}

void HUD::render(ImDrawList* drawList, int width, int height, float fps,
                 const std::vector<TrackedObject>& trackedObjects,
                 const TrackedTarget& lockedTarget,
                 const ViewportInfo& view, const SystemSettings& settings) {

    // Resolve live colors from settings (overrides internal defaults if set)
    ImU32 hudColor    = (settings.hudColor    != 0) ? settings.hudColor    : m_hudColor;
    ImU32 targetColor = (settings.targetColor != 0) ? settings.targetColor : m_targetColor;

    hudColor    = ApplyBrightness(hudColor,    settings.hudBrightness);
    targetColor = ApplyBrightness(targetColor, settings.hudBrightness);

    ImVec2 center    = ImVec2(view.pos_x + view.target_w / 2.0f,
                              view.pos_y + view.target_h / 2.0f);
    float currentTime = GetTimeSeconds() - m_startTime;

    drawList->PushClipRect(ImVec2(view.pos_x, view.pos_y),
                           ImVec2(view.pos_x + view.target_w, view.pos_y + view.target_h),
                           true);

    if (settings.showTacticalOverlay) drawTacticalOverlay(drawList, view, currentTime, hudColor);
    if (settings.showCrosshair)       drawCrosshair(drawList, center, hudColor, settings.crosshairScale);
    if (settings.showCornerBrackets)  drawCornerBrackets(drawList, view, hudColor);
    if (settings.showStatusWindows)   drawStatusWindows(drawList, view, fps, trackedObjects.size(), lockedTarget, hudColor);

    for (const auto& obj : trackedObjects) {
        bool isLocked = (lockedTarget.state != TrackingState::SEARCHING && lockedTarget.track_id == obj.track_id);
        drawTrackedObject(drawList, obj, view, settings, targetColor, hudColor, isLocked);
    }

    drawList->PopClipRect();
}

void HUD::drawTacticalOverlay(ImDrawList* drawList, const ViewportInfo& view,
                               float time, ImU32 color) {
    float centerX = view.pos_x + view.target_w / 2.0f;
    float centerY = view.pos_y + view.target_h / 2.0f;

    // Build a semi-transparent version
    float r = ((color >>  0) & 0xFF) / 255.0f;
    float g = ((color >>  8) & 0xFF) / 255.0f;
    float b = ((color >> 16) & 0xFF) / 255.0f;
    ImU32 rulerColor = IM_COL32(
        static_cast<int>(r * 255),
        static_cast<int>(g * 255),
        static_cast<int>(b * 255),
        80
    );

    for (int i = -5; i <= 5; ++i) {
        if (i == 0) continue;
        float x = centerX + i * 100.0f;
        if (x < view.pos_x || x > view.pos_x + view.target_w) continue;
        float tickH = (i % 5 == 0) ? 18.0f : 10.0f;
        drawList->AddLine(ImVec2(x, centerY - tickH), ImVec2(x, centerY + tickH), rulerColor, 1.0f);
    }
    for (int i = -3; i <= 3; ++i) {
        if (i == 0) continue;
        float y = centerY + i * 100.0f;
        if (y < view.pos_y || y > view.pos_y + view.target_h) continue;
        float tickW = (i % 3 == 0) ? 18.0f : 10.0f;
        drawList->AddLine(ImVec2(centerX - tickW, y), ImVec2(centerX + tickW, y), rulerColor, 1.0f);
    }
}

void HUD::drawCrosshair(ImDrawList* drawList, ImVec2 center, ImU32 color, float scale) {
    float size = 40.0f * scale;
    float gap  = 10.0f * scale;
    float lw   = 1.0f;

    drawList->AddLine(ImVec2(center.x - size, center.y), ImVec2(center.x - gap, center.y), color, lw);
    drawList->AddLine(ImVec2(center.x + gap, center.y),  ImVec2(center.x + size, center.y), color, lw);
    drawList->AddLine(ImVec2(center.x, center.y - size), ImVec2(center.x, center.y - gap), color, lw);
    drawList->AddLine(ImVec2(center.x, center.y + gap),  ImVec2(center.x, center.y + size), color, lw);
    drawList->AddCircle(center, 4.0f * scale, color, 12, lw);
    // Small center dot
    drawList->AddCircleFilled(center, 1.5f, color, 8);
}

void HUD::drawStatusWindows(ImDrawList* drawList, const ViewportInfo& view,
                             float fps, size_t trackedCount, const TrackedTarget& lockedTarget, ImU32 textColor) {
    ImU32 bgColor = IM_COL32(0, 0, 0, 150);
    float padding = 8.0f;
    float margin  = 20.0f;

    // Data Block (Top Left)
    {
        char dataText[256];
        if (lockedTarget.state != TrackingState::SEARCHING) {
            const char* stateStr = (lockedTarget.state == TrackingState::LOCKED) ? "LOCKED" : "LOST";
            snprintf(dataText, sizeof(dataText), "FPS: %.1f\nTRK: %zu\n\nTARGET: %03d\nSTATE: %s\nCLS: %s", 
                     fps, trackedCount, lockedTarget.track_id, stateStr, lockedTarget.className.c_str());
        } else {
            snprintf(dataText, sizeof(dataText), "FPS: %.1f\nTRK: %zu\n\nTARGET: NONE", fps, trackedCount);
        }

        ImVec2 textSize = ImGui::CalcTextSize(dataText);
        ImVec2 bgStart(view.pos_x + margin, view.pos_y + margin);
        ImVec2 bgEnd(bgStart.x + textSize.x + padding * 2, bgStart.y + textSize.y + padding * 2);
        drawList->AddRectFilled(bgStart, bgEnd, bgColor, 2.0f);
        drawList->AddRect(bgStart, bgEnd, textColor, 2.0f, 0, 0.5f);
        drawList->AddText(ImVec2(bgStart.x + padding, bgStart.y + padding), textColor, dataText);
    }

    // SysLog Block (Bottom Left)
    {
        const char* sysLogText = "SYS: ONLINE\nMISSION: ACTIVE";
        ImVec2 textSize = ImGui::CalcTextSize(sysLogText);
        ImVec2 bgStart(view.pos_x + margin,
                       view.pos_y + view.target_h - textSize.y - padding * 2 - margin);
        ImVec2 bgEnd(bgStart.x + textSize.x + padding * 2, bgStart.y + textSize.y + padding * 2);
        drawList->AddRectFilled(bgStart, bgEnd, bgColor, 2.0f);
        drawList->AddRect(bgStart, bgEnd, textColor, 2.0f, 0, 0.5f);
        drawList->AddText(ImVec2(bgStart.x + padding, bgStart.y + padding), textColor, sysLogText);
    }
}

void HUD::drawCornerBrackets(ImDrawList* drawList, const ViewportInfo& view, ImU32 color) {
    float m  = 30.0f;
    float l  = 40.0f;
    float x1 = view.pos_x, y1 = view.pos_y;
    float x2 = view.pos_x + view.target_w;
    float y2 = view.pos_y + view.target_h;
    float lw = 1.5f;
    drawList->AddPolyline((ImVec2[]){ImVec2(x1+m+l, y1+m), ImVec2(x1+m, y1+m), ImVec2(x1+m, y1+m+l)}, 3, color, 0, lw);
    drawList->AddPolyline((ImVec2[]){ImVec2(x2-m-l, y1+m), ImVec2(x2-m, y1+m), ImVec2(x2-m, y1+m+l)}, 3, color, 0, lw);
    drawList->AddPolyline((ImVec2[]){ImVec2(x1+m+l, y2-m), ImVec2(x1+m, y2-m), ImVec2(x1+m, y2-m-l)}, 3, color, 0, lw);
    drawList->AddPolyline((ImVec2[]){ImVec2(x2-m-l, y2-m), ImVec2(x2-m, y2-m), ImVec2(x2-m, y2-m-l)}, 3, color, 0, lw);
}

void HUD::drawTrackedObject(ImDrawList* drawList, const TrackedObject& obj,
                             const ViewportInfo& view, const SystemSettings& settings,
                             ImU32 targetColor, ImU32 hudColor, bool isLocked) {
    float x1 = view.pos_x + obj.box.x * view.scale;
    float y1 = view.pos_y + obj.box.y * view.scale;
    float x2 = x1 + obj.box.width  * view.scale;
    float y2 = y1 + obj.box.height * view.scale;

    ImU32 boxColor = obj.is_confirmed ? targetColor : IM_COL32(150, 150, 150, 150);
    if (isLocked) {
        boxColor = IM_COL32(255, 50, 50, 255); // Solid red for locked target
    }

    float lw = isLocked ? settings.boxLineWidth * 1.8f : settings.boxLineWidth;

    // Draw fading trail
    if (settings.showTrails && obj.trail.size() > 1) {
        size_t n = obj.trail.size();
        for (size_t i = 0; i < n - 1; ++i) {
            float alpha = settings.showTrailFade
                ? static_cast<float>(i + 1) / static_cast<float>(n)
                : 1.0f;

            ImU32 trailColor = IM_COL32(
                (boxColor >>  0) & 0xFF,
                (boxColor >>  8) & 0xFF,
                (boxColor >> 16) & 0xFF,
                static_cast<int>(alpha * 200)
            );

            ImVec2 p1(view.pos_x + obj.trail[i].x   * view.scale,
                      view.pos_y + obj.trail[i].y   * view.scale);
            ImVec2 p2(view.pos_x + obj.trail[i+1].x * view.scale,
                      view.pos_y + obj.trail[i+1].y * view.scale);
            drawList->AddLine(p1, p2, trailColor, lw);
        }
    }

    // Bounding box
    drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), boxColor, 0.0f, 0, lw);

    if (isLocked) {
        ImVec2 center(x1 + (x2 - x1) / 2.0f, y1 + (y2 - y1) / 2.0f);
        float radius = std::min(x2 - x1, y2 - y1) * 0.4f;
        if (radius < 4.0f) radius = 4.0f;
        drawList->AddCircle(center, radius, boxColor, 16, lw);
    }

    // Label: build according to visibility toggles
    char tag[160] = {0};
    if (isLocked) {
        snprintf(tag, sizeof(tag), ">>> ID:%03d <<<", obj.track_id);
    } else if (settings.showTrackIDs && settings.showConfidence) {
        snprintf(tag, sizeof(tag), "ID:%d [%s]  CONF:%.2f",
                 obj.track_id, obj.className.c_str(), obj.confidence);
    } else if (settings.showTrackIDs) {
        snprintf(tag, sizeof(tag), "ID:%d [%s]",
                 obj.track_id, obj.className.c_str());
    } else if (settings.showConfidence) {
        snprintf(tag, sizeof(tag), "[%s]  %.2f",
                 obj.className.c_str(), obj.confidence);
    } else {
        snprintf(tag, sizeof(tag), "[%s]", obj.className.c_str());
    }

    if (tag[0] != '\0') {
        ImVec2 tSize = ImGui::CalcTextSize(tag);
        ImVec2 tPos(x1, y1 - tSize.y - 4.0f);
        if (tPos.y < view.pos_y) tPos.y = y1 + 4.0f;

        drawList->AddRectFilled(tPos, ImVec2(tPos.x + tSize.x + 6.0f, tPos.y + tSize.y),
                                IM_COL32(0, 0, 0, 160));
        drawList->AddText(ImVec2(tPos.x + 3.0f, tPos.y), boxColor, tag);
    }
}

// ============================================================
// Motion Overlay — Plan 10
// Option C: semi-transparent fill + solid outline
// ============================================================
void HUD::drawMotionOverlay(ImDrawList* drawList,
                             const std::vector<cv::Rect>& regions,
                             const ViewportInfo& view,
                             const SystemSettings& settings) {
    if (!settings.motionShowOverlay || regions.empty()) return;

    // Resolve color from settings; fall back to default orange-red
    ImU32 baseColor = (settings.motionOverlayColor != 0)
        ? settings.motionOverlayColor
        : m_motionColor;

    // Extract RGB channels from the base color (stored as RGBA ImU32)
    const uint8_t r = static_cast<uint8_t>((baseColor >>  0) & 0xFF);
    const uint8_t g = static_cast<uint8_t>((baseColor >>  8) & 0xFF);
    const uint8_t b = static_cast<uint8_t>((baseColor >> 16) & 0xFF);

    // Fill alpha from settings; outline is always opaque
    const uint8_t fillAlpha    = static_cast<uint8_t>(
        std::clamp(settings.motionOverlayAlpha, 0.0f, 1.0f) * 255.0f);
    const ImU32 fillColor      = IM_COL32(r, g, b, fillAlpha);
    const ImU32 outlineColor   = IM_COL32(r, g, b, 220);
    constexpr float kLineWidth = 1.0f;

    for (const cv::Rect& rect : regions) {
        // Transform from frame-space to viewport-space
        const float vx1 = view.pos_x + rect.x              * view.scale;
        const float vy1 = view.pos_y + rect.y              * view.scale;
        const float vx2 = view.pos_x + (rect.x + rect.width)  * view.scale;
        const float vy2 = view.pos_y + (rect.y + rect.height) * view.scale;

        // Fill (drawn first so outline is rendered on top)
        if (fillAlpha > 0) {
            drawList->AddRectFilled(ImVec2(vx1, vy1), ImVec2(vx2, vy2), fillColor, 0.0f);
        }

        // Outline
        drawList->AddRect(ImVec2(vx1, vy1), ImVec2(vx2, vy2), outlineColor, 0.0f, 0, kLineWidth);
    }
}
