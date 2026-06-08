#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// UIManager_internal.hpp  –  private helpers shared across UIManager_*.cpp
// DO NOT include outside of UIManager translation units.
// ─────────────────────────────────────────────────────────────────────────────
#include <imgui.h>
#include <algorithm>
#include <string>

static constexpr ImU32 kDefaultHudColor    = IM_COL32(0, 200, 100, 220);
static constexpr ImU32 kDefaultTargetColor = IM_COL32(255, 180, 0, 255);

// packed ABGR → float[4] RGBA
static void ImU32ToFloat4(ImU32 col, float out[4]) {
    out[0] = ((col >>  0) & 0xFF) / 255.0f;
    out[1] = ((col >>  8) & 0xFF) / 255.0f;
    out[2] = ((col >> 16) & 0xFF) / 255.0f;
    out[3] = ((col >> 24) & 0xFF) / 255.0f;
}

static ImU32 Float4ToImU32(const float col[4]) {
    return IM_COL32(
        static_cast<int>(col[0] * 255),
        static_cast<int>(col[1] * 255),
        static_cast<int>(col[2] * 255),
        static_cast<int>(col[3] * 255));
}

static ImU32 ApplyBrightnessLocal(ImU32 col, float brightness) {
    return IM_COL32(
        static_cast<int>(((col >>  0) & 0xFF) * brightness),
        static_cast<int>(((col >>  8) & 0xFF) * brightness),
        static_cast<int>(((col >> 16) & 0xFF) * brightness),
        static_cast<int>(((col >> 24) & 0xFF)));
}

static std::string EscapeJsonString(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char ch : v) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += ch;     break;
        }
    }
    return out;
}
