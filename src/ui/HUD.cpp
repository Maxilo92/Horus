#include "HUD.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>

static float GetTimeSeconds() {
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(now - start).count();
}

static float calculateIOU(const cv::Rect& box1, const cv::Rect& box2) {
    int x1 = std::max(box1.x, box2.x);
    int y1 = std::max(box1.y, box2.y);
    int x2 = std::min(box1.x + box1.width, box2.x + box2.width);
    int y2 = std::min(box1.y + box1.height, box2.y + box2.height);
    if (x1 >= x2 || y1 >= y2) return 0.0f;
    float intersectionArea = static_cast<float>((x2 - x1) * (y2 - y1));
    float area1 = static_cast<float>(box1.width * box1.height);
    float area2 = static_cast<float>(box2.width * box2.height);
    return intersectionArea / (area1 + area2 - intersectionArea);
}

HUD::HUD() {
    m_hudColor = IM_COL32(0, 200, 100, 220);
    m_targetColor = IM_COL32(255, 180, 0, 255);
    m_startTime = GetTimeSeconds();
}

HUD::~HUD() {}

void HUD::render(ImDrawList* drawList, int width, int height, float fps, const std::vector<TrackedObject>& trackedObjects, const ViewportInfo& view, const SystemSettings& settings) {
    ImVec2 center = ImVec2(view.pos_x + view.target_w / 2.0f, view.pos_y + view.target_h / 2.0f);
    float currentTime = GetTimeSeconds() - m_startTime;

    drawList->PushClipRect(ImVec2(view.pos_x, view.pos_y), ImVec2(view.pos_x + view.target_w, view.pos_y + view.target_h), true);

    if (settings.showTacticalOverlay) drawTacticalOverlay(drawList, view, currentTime);
    if (settings.showCrosshair) drawCrosshair(drawList, center, m_hudColor);
    if (settings.showCornerBrackets) drawCornerBrackets(drawList, view, m_hudColor);
    if (settings.showStatusWindows) drawStatusWindows(drawList, view, fps, trackedObjects.size());

    for (const auto& obj : trackedObjects) {
        drawTrackedObject(drawList, obj, view, settings);
    }

    drawList->PopClipRect();
}

void HUD::drawTacticalOverlay(ImDrawList* drawList, const ViewportInfo& view, float time) {
    float centerX = view.pos_x + view.target_w / 2.0f;
    float centerY = view.pos_y + view.target_h / 2.0f;
    ImU32 rulerColor = IM_COL32(0, 200, 100, 100);
    for (int i = -5; i <= 5; ++i) {
        if (i == 0) continue;
        float x = centerX + i * 100.0f;
        if (x < view.pos_x || x > view.pos_x + view.target_w) continue;
        drawList->AddLine(ImVec2(x, centerY - 10), ImVec2(x, centerY + 10), rulerColor, 1.0f);
    }
    for (int i = -3; i <= 3; ++i) {
        if (i == 0) continue;
        float y = centerY + i * 100.0f;
        if (y < view.pos_y || y > view.pos_y + view.target_h) continue;
        drawList->AddLine(ImVec2(centerX - 10, y), ImVec2(centerX + 10, y), rulerColor, 1.0f);
    }
}

void HUD::drawCrosshair(ImDrawList* drawList, ImVec2 center, ImU32 color) {
    float size = 40.0f, gap = 10.0f;
    drawList->AddLine(ImVec2(center.x - size, center.y), ImVec2(center.x - gap, center.y), color, 1.0f);
    drawList->AddLine(ImVec2(center.x + gap, center.y), ImVec2(center.x + size, center.y), color, 1.0f);
    drawList->AddLine(ImVec2(center.x, center.y - size), ImVec2(center.x, center.y - gap), color, 1.0f);
    drawList->AddLine(ImVec2(center.x, center.y + gap), ImVec2(center.x, center.y + size), color, 1.0f);
    drawList->AddCircle(center, 4.0f, color, 12, 1.0f);
}

void HUD::drawStatusWindows(ImDrawList* drawList, const ViewportInfo& view, float fps, size_t trackedCount) {
    ImU32 textColor = IM_COL32(0, 255, 100, 255);
    ImU32 bgColor = IM_COL32(0, 0, 0, 150);
    float padding = 8.0f;
    float margin = 20.0f;

    // Data Block (Top Left)
    {
        char dataText[128];
        snprintf(dataText, sizeof(dataText), "FPS: %.1f\nTRK: %zu", fps, trackedCount);
        ImVec2 textSize = ImGui::CalcTextSize(dataText);
        ImVec2 bgStart(view.pos_x + margin, view.pos_y + margin);
        ImVec2 bgEnd(bgStart.x + textSize.x + padding * 2, bgStart.y + textSize.y + padding * 2);
        drawList->AddRectFilled(bgStart, bgEnd, bgColor);
        drawList->AddText(ImVec2(bgStart.x + padding, bgStart.y + padding), textColor, dataText);
    }

    // SysLog Block (Bottom Left)
    {
        const char* sysLogText = "SYS: ONLINE\nACTIVE THRT";
        ImVec2 textSize = ImGui::CalcTextSize(sysLogText);
        ImVec2 bgStart(view.pos_x + margin, view.pos_y + view.target_h - textSize.y - padding * 2 - margin);
        ImVec2 bgEnd(bgStart.x + textSize.x + padding * 2, bgStart.y + textSize.y + padding * 2);
        drawList->AddRectFilled(bgStart, bgEnd, bgColor);
        drawList->AddText(ImVec2(bgStart.x + padding, bgStart.y + padding), textColor, sysLogText);
    }
}

void HUD::drawCornerBrackets(ImDrawList* drawList, const ViewportInfo& view, ImU32 color) {
    float m = 30.0f, l = 40.0f, x1 = view.pos_x, y1 = view.pos_y, x2 = view.pos_x + view.target_w, y2 = view.pos_y + view.target_h;
    drawList->AddPolyline((ImVec2[]){ImVec2(x1+m+l, y1+m), ImVec2(x1+m, y1+m), ImVec2(x1+m, y1+m+l)}, 3, color, 0, 1.5f);
    drawList->AddPolyline((ImVec2[]){ImVec2(x2-m-l, y1+m), ImVec2(x2-m, y1+m), ImVec2(x2-m, y1+m+l)}, 3, color, 0, 1.5f);
    drawList->AddPolyline((ImVec2[]){ImVec2(x1+m+l, y2-m), ImVec2(x1+m, y2-m), ImVec2(x1+m, y2-m-l)}, 3, color, 0, 1.5f);
    drawList->AddPolyline((ImVec2[]){ImVec2(x2-m-l, y2-m), ImVec2(x2-m, y2-m), ImVec2(x2-m, y2-m-l)}, 3, color, 0, 1.5f);
}

void HUD::drawTrackedObject(ImDrawList* drawList, const TrackedObject& obj, const ViewportInfo& view, const SystemSettings& settings) {
    float x1 = view.pos_x + obj.box.x * view.scale;
    float y1 = view.pos_y + obj.box.y * view.scale;
    float x2 = x1 + obj.box.width * view.scale;
    float y2 = y1 + obj.box.height * view.scale;

    ImU32 color = obj.is_confirmed ? m_targetColor : IM_COL32(150, 150, 150, 150);
    
    // Draw trail
    if (settings.showTrails && obj.trail.size() > 1) {
        for (size_t i = 0; i < obj.trail.size() - 1; ++i) {
            ImVec2 p1(view.pos_x + obj.trail[i].x * view.scale, view.pos_y + obj.trail[i].y * view.scale);
            ImVec2 p2(view.pos_x + obj.trail[i+1].x * view.scale, view.pos_y + obj.trail[i+1].y * view.scale);
            drawList->AddLine(p1, p2, color, 1.5f);
        }
    }

    // Draw box
    drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), color, 0.0f, 0, 1.5f);

    // Draw ID and class
    char tag[128];
    snprintf(tag, sizeof(tag), "ID:%d [%s]\nCONF:%.2f", obj.track_id, obj.className.c_str(), obj.confidence);
    ImVec2 tagPos(x1, y1 - 35);
    if (tagPos.y < view.pos_y) tagPos.y = y1 + 5;
    
    drawList->AddRectFilled(tagPos, ImVec2(tagPos.x + 100, tagPos.y + 32), IM_COL32(0, 0, 0, 180));
    drawList->AddText(ImVec2(tagPos.x + 5, tagPos.y + 2), color, tag);
}
