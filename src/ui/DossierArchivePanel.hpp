#pragma once

#include <imgui.h>
#include <GLFW/glfw3.h>
#include <string>
#include <vector>
#include <cstdint>
#include "Common.hpp"
#include "DossierDatabase.hpp"

class DossierArchivePanel {
public:
    DossierArchivePanel(DossierDatabase& db);
    ~DossierArchivePanel();

    void render(bool* p_open);

private:
    DossierDatabase& m_db;

    std::vector<DossierEntry> m_entities;
    std::vector<DossierEntry> m_filtered;

    char m_filterBuf[128] = {0};
    int m_selectedIdx = -1;

    // Thumbnail texture state
    GLuint      m_thumbTex = 0;
    std::string m_thumbUuid;
    int         m_thumbW = 0;
    int         m_thumbH = 0;

    void refreshData();
    void applyFilter();
    void loadThumbnail(const std::string& uuid);
};
