#include "DossierArchivePanel.hpp"
#include "MarkdownText.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <iostream>

DossierArchivePanel::DossierArchivePanel(DossierDatabase& db) : m_db(db) {}

DossierArchivePanel::~DossierArchivePanel() {
    if (m_thumbTex) glDeleteTextures(1, &m_thumbTex);
}

void DossierArchivePanel::loadThumbnail(const std::string& uuid) {
    if (uuid == m_thumbUuid) return;

    if (m_thumbTex) {
        glDeleteTextures(1, &m_thumbTex);
        m_thumbTex = 0;
        m_thumbW = m_thumbH = 0;
    }
    m_thumbUuid = uuid;

    std::vector<uchar> jpegData;
    if (!m_db.getEntityThumbnail(uuid, jpegData) || jpegData.empty()) return;

    cv::Mat img = cv::imdecode(jpegData, cv::IMREAD_COLOR);
    if (img.empty()) return;

    m_thumbW = img.cols;
    m_thumbH = img.rows;

    glGenTextures(1, &m_thumbTex);
    glBindTexture(GL_TEXTURE_2D, m_thumbTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                 img.cols, img.rows, 0,
                 GL_BGR, GL_UNSIGNED_BYTE, img.data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

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

        loadThumbnail(entry.uuid);

        ImGui::TextColored(ImVec4(0, 1, 0, 1), "DOSSIER DETAILS");
        ImGui::Separator();

        // Side-by-side: thumbnail on the left, metadata on the right
        if (m_thumbTex && m_thumbW > 0 && m_thumbH > 0) {
            const float thumbDisplayH = 140.0f;
            float aspect = static_cast<float>(m_thumbW) / static_cast<float>(m_thumbH);
            float thumbDisplayW = thumbDisplayH * aspect;

            ImGui::Image((ImTextureID)(intptr_t)m_thumbTex,
                         ImVec2(thumbDisplayW, thumbDisplayH));
            ImGui::SameLine();
        }

        ImGui::BeginGroup();
        ImGui::Columns(2, "DetailCols", false);
        ImGui::SetColumnWidth(0, 120.0f);

        ImGui::Text("UUID:"); ImGui::NextColumn(); ImGui::Text("%s", entry.uuid.c_str()); ImGui::NextColumn();
        ImGui::Text("Type:"); ImGui::NextColumn(); ImGui::Text("%s", entry.type.c_str()); ImGui::NextColumn();
        if (!entry.plate.empty()) {
            ImGui::Text("Plate:"); ImGui::NextColumn();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.1f, 1.0f), "%s", entry.plate.c_str());
            ImGui::NextColumn();
        }
        ImGui::Text("First Seen:"); ImGui::NextColumn(); ImGui::Text("%s", entry.first_seen.c_str()); ImGui::NextColumn();
        ImGui::Text("Last Seen:"); ImGui::NextColumn(); ImGui::Text("%s", entry.last_seen.c_str()); ImGui::NextColumn();
        ImGui::Text("Total Sightings:"); ImGui::NextColumn(); ImGui::Text("%d", entry.sightings_count); ImGui::NextColumn();

        ImGui::Columns(1);
        ImGui::EndGroup();

        ImGui::Separator();

        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1, 1), "AI ANALYSIS:");
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.07f, 1.0f));
        ImGui::BeginChild("DossierDetailText", ImVec2(0, 0), true);
        MarkdownText::Render(entry.dossier_text);
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
        std::string plate = entry.plate;
        std::transform(uuid.begin(), uuid.end(), uuid.begin(), ::tolower);
        std::transform(type.begin(), type.end(), type.begin(), ::tolower);
        std::transform(text.begin(), text.end(), text.begin(), ::tolower);
        std::transform(plate.begin(), plate.end(), plate.begin(), ::tolower);

        if (uuid.find(filter) != std::string::npos ||
            type.find(filter) != std::string::npos ||
            text.find(filter) != std::string::npos ||
            plate.find(filter) != std::string::npos) {
            m_filtered.push_back(entry);
        }
    }
    
    if (m_selectedIdx >= static_cast<int>(m_filtered.size())) {
        m_selectedIdx = -1;
    }
}
