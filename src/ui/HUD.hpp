#ifndef HUD_HPP
#define HUD_HPP

#include <string>
#include <vector>
#include "imgui.h"
#include "Common.hpp"

// Default motion overlay color: orange-red, semi-transparent
static constexpr ImU32 kDefaultMotionColor = IM_COL32(255, 89, 0, 165);

class HUD {
public:
    HUD();
    ~HUD();

    void render(ImDrawList* drawList, int width, int height, float fps,
                const std::vector<TrackedObject>& trackedObjects,
                const TrackedTarget& lockedTarget,
                const ViewportInfo& view,
                const SystemSettings& settings);

    // Draw motion detection overlay (Option C: fill + outline).
    // Must be called before drawTrackedObject() so motion regions appear
    // behind tracking bounding boxes.
    void drawMotionOverlay(ImDrawList* drawList,
                           const std::vector<cv::Rect>& regions,
                           const ViewportInfo& view,
                           const SystemSettings& settings);

private:
    void drawTacticalOverlay(ImDrawList* drawList, const ViewportInfo& view,
                              float time, ImU32 color);
    void drawCrosshair(ImDrawList* drawList, ImVec2 center, ImU32 color, float scale);
    void drawStatusWindows(ImDrawList* drawList, const ViewportInfo& view,
                            float fps, size_t trackedCount, const TrackedTarget& lockedTarget, ImU32 textColor);
    void drawCornerBrackets(ImDrawList* drawList, const ViewportInfo& view, ImU32 color);
    void drawTrackedObject(ImDrawList* drawList, const TrackedObject& obj,
                            const ViewportInfo& view, const SystemSettings& settings,
                            ImU32 targetColor, ImU32 hudColor, bool isLocked);

    ImU32 m_hudColor;
    ImU32 m_targetColor;
    ImU32 m_motionColor;
    float m_startTime;
};

#endif // HUD_HPP

