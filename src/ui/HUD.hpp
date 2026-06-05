#ifndef HUD_HPP
#define HUD_HPP

#include <string>
#include <vector>
#include "imgui.h"
#include "Common.hpp"

class HUD {
public:
    HUD();
    ~HUD();

    void render(ImDrawList* drawList, int width, int height, float fps,
                const std::vector<TrackedObject>& trackedObjects,
                const ViewportInfo& view,
                const SystemSettings& settings);

private:
    void drawTacticalOverlay(ImDrawList* drawList, const ViewportInfo& view,
                              float time, ImU32 color);
    void drawCrosshair(ImDrawList* drawList, ImVec2 center, ImU32 color, float scale);
    void drawStatusWindows(ImDrawList* drawList, const ViewportInfo& view,
                            float fps, size_t trackedCount, ImU32 textColor);
    void drawCornerBrackets(ImDrawList* drawList, const ViewportInfo& view, ImU32 color);
    void drawTrackedObject(ImDrawList* drawList, const TrackedObject& obj,
                            const ViewportInfo& view, const SystemSettings& settings,
                            ImU32 targetColor, ImU32 hudColor);

    ImU32 m_hudColor;
    ImU32 m_targetColor;
    float m_startTime;
};

#endif // HUD_HPP
