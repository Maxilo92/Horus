#pragma once
#include "Common.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <functional>
#include <imgui.h>

class Blackboard;

class AnalyzerPanel {
public:
    struct TextureInfo {
        uint32_t texture_id_first = 0; int texture_version_first = -1;
        uint32_t texture_id_mid   = 0; int texture_version_mid   = -1;
        uint32_t texture_id_last  = 0; int texture_version_last  = -1;
        std::vector<uint32_t> texture_ids_periodic;
        std::vector<int>      texture_versions_periodic;
        int   gallery_version  = 0;
        float max_confidence   = 0.0f;
    };

    struct UiState {
        bool show_full_gallery       = false;
        int  selected_snapshot_idx   = -1;
        bool show_large_view_modal   = false;
        int  large_view_snapshot_idx = -1;
        uint32_t large_view_tex_id   = 0;
        char name_buf[64]            = {0};  // Edit buffer for the face name
        bool name_buf_initialized    = false;
    };

    AnalyzerPanel() = default;
    ~AnalyzerPanel() = default;

    void render(int& selectedId, std::vector<UniqueTargetRecord>& history,
                Blackboard& blackboard, const SystemSettings& settings,
                std::function<void(LogLevel, const std::string&)> logFn,
                std::function<void(const UniqueTargetRecord&)> exportFn,
                std::function<void(const cv::Mat&, uint32_t&, int&, int)> updateTexFn);

    TextureInfo& getTextureInfo(int trackId) { return m_textures[trackId]; }
    UiState& getUiState(int trackId) { return m_uiStates[trackId]; }

private:
    std::unordered_map<int, TextureInfo> m_textures;
    std::unordered_map<int, UiState>      m_uiStates;
};
