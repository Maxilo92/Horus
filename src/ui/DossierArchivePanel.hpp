#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include "Common.hpp"
#include "DossierDatabase.hpp"

class DossierArchivePanel {
public:
    DossierArchivePanel(DossierDatabase& db);
    ~DossierArchivePanel() = default;

    void render(bool* p_open);

private:
    DossierDatabase& m_db;
    
    std::vector<DossierEntry> m_entities;
    std::vector<DossierEntry> m_filtered;
    
    char m_filterBuf[128] = {0};
    int m_selectedIdx = -1;
    
    void refreshData();
    void applyFilter();
};
