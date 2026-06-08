#include "DossierArchivePanel.hpp"
#include <algorithm>
#include <iostream>

DossierArchivePanel::DossierArchivePanel(DossierDatabase& db) : m_db(db) {}

void DossierArchivePanel::render(bool* p_open) {
    if (!*p_open) return;

    ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AI Dossier Archive", p_open)) {
        ImGui::End();
        return;
    }

    // Header & Stats
    ImGui::TextColored(ImVec4(0, 0.8f, 1, 1), "PERSISTENT OBJECT MEMORY");
    ImGui::SameLine(ImGui::GetWindowWidth() - 150);
    if (ImGui::Button("Refresh Database")) {
        refreshData();
    }
    ImGui::Separator();

    // Split Layout: List (Left) | Detail (Right)
    static float leftWidth = 300.0f;
    ImGui::BeginChild("ArchiveList", ImVec2(leftWidth, 0), true);
    
    // Filter
    if (ImGui::InputTextWithHint("##Filter", "Search UUID/Type...", m_filterBuf, sizeof(m_filterBuf))) {
        applyFilter();
    }
    ImGui::Separator();

    if (m_entities.empty()) {
        refreshData();
    }

    if (ImGui::BeginTable("ArchiveTable", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(m_filtered.size()); ++i) {
            const auto& entry = m_filtered[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            
            bool isSelected = (m_selectedIdx == i);
            if (ImGui::Selectable(entry.uuid.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                m_selectedIdx = i;
            }
            
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", entry.type.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Detail View
    ImGui::BeginChild("ArchiveDetail", ImVec2(0, 0), true);
    if (m_selectedIdx >= 0 && m_selectedIdx < static_cast<int>(m_filtered.size())) {
        const auto& entry = m_filtered[m_selectedIdx];
        
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "DOSSIER DETAILS");
        ImGui::Separator();
        
        ImGui::Columns(2, "DetailCols", false);
        ImGui::SetColumnWidth(0, 120.0f);
        
        ImGui::Text("UUID:"); ImGui::NextColumn(); ImGui::Text("%s", entry.uuid.c_str()); ImGui::NextColumn();
        ImGui::Text("Type:"); ImGui::NextColumn(); ImGui::Text("%s", entry.type.c_str()); ImGui::NextColumn();
        ImGui::Text("First Seen:"); ImGui::NextColumn(); ImGui::Text("%s", entry.first_seen.c_str()); ImGui::NextColumn();
        ImGui::Text("Last Seen:"); ImGui::NextColumn(); ImGui::Text("%s", entry.last_seen.c_str()); ImGui::NextColumn();
        ImGui::Text("Total Sightings:"); ImGui::NextColumn(); ImGui::Text("%d", entry.sightings_count); ImGui::NextColumn();
        
        ImGui::Columns(1);
        ImGui::Separator();
        
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1, 1), "AI ANALYSIS:");
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.07f, 1.0f));
        ImGui::BeginChild("DossierDetailText", ImVec2(0, 0), true);
        ImGui::TextWrapped("%s", entry.dossier_text.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Select an entity from the list to view its dossier.");
    }
    ImGui::EndChild();

    ImGui::End();
}

void DossierArchivePanel::refreshData() {
    m_entities = m_db.getAllEntities();
    applyFilter();
}

void DossierArchivePanel::applyFilter() {
    m_filtered.clear();
    std::string filter = m_filterBuf;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
    
    for (const auto& entry : m_entities) {
        if (filter.empty()) {
            m_filtered.push_back(entry);
            continue;
        }
        
        std::string uuid = entry.uuid;
        std::string type = entry.type;
        std::string text = entry.dossier_text;
        std::transform(uuid.begin(), uuid.end(), uuid.begin(), ::tolower);
        std::transform(type.begin(), type.end(), type.begin(), ::tolower);
        std::transform(text.begin(), text.end(), text.begin(), ::tolower);
        
        if (uuid.find(filter) != std::string::npos || 
            type.find(filter) != std::string::npos ||
            text.find(filter) != std::string::npos) {
            m_filtered.push_back(entry);
        }
    }
    
    if (m_selectedIdx >= static_cast<int>(m_filtered.size())) {
        m_selectedIdx = -1;
    }
}
