#include "AnalyzerPanel.hpp"
#include "Blackboard.hpp"
#include <algorithm>
#include <filesystem>
#include <opencv2/imgcodecs.hpp>
#include <GLFW/glfw3.h>

void AnalyzerPanel::render(int& selectedId, std::vector<UniqueTargetRecord>& history,
                           Blackboard& blackboard, const SystemSettings& settings,
                           std::function<void(LogLevel, const std::string&)> logFn,
                           std::function<void(const UniqueTargetRecord&)> exportFn,
                           std::function<void(const cv::Mat&, uint32_t&, int&, int)> updateTexFn) {
    ImGui::Begin("Target Analyzer");

    if (selectedId == -1) {
        ImGui::TextDisabled("No target selected for analysis.");
        ImGui::End();
        return;
    }

    int targetIdx = -1;
    for (int i = 0; i < (int)history.size(); ++i) {
        if (history[i].track_id == selectedId) { targetIdx = i; break; }
    }
    if (targetIdx == -1) {
        ImGui::TextColored(ImVec4(1,0,0,1), "Target ID %d not found.", selectedId);
        if (ImGui::Button("Clear")) selectedId = -1;
        ImGui::End();
        return;
    }

    auto& rec = history[targetIdx];
    auto& uiState = m_uiStates[rec.track_id];
    auto& texInfo = m_textures[rec.track_id];

    // Texture Updates
    updateTexFn(rec.snapshot_first.image, texInfo.texture_id_first, texInfo.texture_version_first, rec.cropped_image_first_version);
    updateTexFn(rec.snapshot_mid.image,   texInfo.texture_id_mid,   texInfo.texture_version_mid,   rec.cropped_image_mid_version);
    updateTexFn(rec.snapshot_last.image,  texInfo.texture_id_last,  texInfo.texture_version_last,  rec.cropped_image_last_version);

    if (rec.gallery_version != texInfo.gallery_version || texInfo.texture_ids_periodic.size() != rec.periodic_snapshots.size()) {
        // Cleanup old textures if we are resizing (especially during decimation)
        for (uint32_t tid : texInfo.texture_ids_periodic) {
            if (tid != 0) glDeleteTextures(1, &tid);
        }
        texInfo.texture_ids_periodic.assign(rec.periodic_snapshots.size(), 0);
        texInfo.texture_versions_periodic.assign(rec.periodic_snapshots.size(), -1);
        texInfo.gallery_version = rec.gallery_version;
    }
    for (size_t i = 0; i < rec.periodic_snapshots.size(); ++i) {
        updateTexFn(rec.periodic_snapshots[i].image, texInfo.texture_ids_periodic[i], texInfo.texture_versions_periodic[i], 1);
    }

    // Header
    ImGui::TextColored(ImVec4(0, 1, 0.5f, 1), "TARGET REPORT: ID %03d", rec.track_id);
    ImGui::Separator();

    if (ImGui::SmallButton(uiState.show_full_gallery ? "[ VIEW: SINGLE ]" : "[ VIEW: GALLERY ]"))
        uiState.show_full_gallery = !uiState.show_full_gallery;

    if (uiState.show_full_gallery) {
        const int cols = 3;
        float availW = ImGui::GetContentRegionAvail().x;
        float imgW = (availW / cols) - 4.0f;

        for (int i = 0; i < (int)rec.periodic_snapshots.size(); ++i) {
            uint32_t texID = (i < (int)texInfo.texture_ids_periodic.size()) ? texInfo.texture_ids_periodic[i] : 0;
            if (texID) {
                const cv::Mat& img = rec.periodic_snapshots[i].image;
                float ar = 0.75f; // Fallback 4:3
                if (!img.empty() && img.cols > 0) ar = (float)img.rows / (float)img.cols;
                
                if (ImGui::ImageButton((std::string("##img")+std::to_string(i)).c_str(), (void*)(intptr_t)texID, ImVec2(imgW, imgW * ar)))
                    uiState.selected_snapshot_idx = i;
            }
            if ((i+1) % cols != 0) ImGui::SameLine();
        }
    } else {
        // Single View
        static int activeMilestone = 1; // 0=First, 1=Mid, 2=Last
        if (ImGui::Button("< Prev")) activeMilestone = (activeMilestone + 2) % 3; ImGui::SameLine();
        if (ImGui::Button("Next >")) activeMilestone = (activeMilestone + 1) % 3;
        
        uint32_t tex = (activeMilestone == 0) ? texInfo.texture_id_first : (activeMilestone == 1 ? texInfo.texture_id_mid : texInfo.texture_id_last);
        const cv::Mat& img = (activeMilestone == 0) ? rec.snapshot_first.image : (activeMilestone == 1 ? rec.snapshot_mid.image : rec.snapshot_last.image);
        
        if (tex && !img.empty() && img.cols > 0) {
            float availW = ImGui::GetContentRegionAvail().x;
            float ar = (float)img.rows / (float)img.cols;
            float w = availW;
            float h = w * ar;

            // Limit height to prevent the panel from becoming too long, adjust width accordingly
            if (h > 350.0f) {
                h = 350.0f;
                w = h / ar;
                float offset = (availW - w) * 0.5f;
                if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
            }
            ImGui::Image((void*)(intptr_t)tex, ImVec2(w, h));
        } else if (tex) {
            ImGui::Image((void*)(intptr_t)tex, ImVec2(ImGui::GetContentRegionAvail().x, 200));
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Export Details")) exportFn(rec);
    if (ImGui::Button("Deselect")) selectedId = -1;

    ImGui::End();
}
