#include "MarkdownText.hpp"
#include "imgui.h"
#include <cstring>
#include <vector>

namespace {

struct Segment {
    std::string text;
    bool bold   = false;
    bool italic = false;
    bool code   = false;
};

// Zerlegt eine Zeile in Inline-Segmente (**fett**, *kursiv*, `code`).
std::vector<Segment> parseInline(const std::string& line) {
    std::vector<Segment> segs;
    Segment cur;
    bool bold = false, italic = false, code = false;

    auto flush = [&]() {
        if (!cur.text.empty()) segs.push_back(cur);
        cur.text.clear();
    };

    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '`') {
            flush();
            code = !code;
            cur.bold = bold; cur.italic = italic; cur.code = code;
            continue;
        }
        if (!code && i + 1 < line.size() && line[i] == '*' && line[i + 1] == '*') {
            flush();
            bold = !bold;
            cur.bold = bold; cur.italic = italic; cur.code = code;
            ++i;
            continue;
        }
        if (!code && line[i] == '*') {
            flush();
            italic = !italic;
            cur.bold = bold; cur.italic = italic; cur.code = code;
            continue;
        }
        cur.text += line[i];
    }
    flush();
    return segs;
}

// Rendert Segmente als wortweise umbrochenen Fließtext. ImGui kann keine
// Inline-Stile innerhalb von TextWrapped — daher manueller Umbruch über
// CalcTextSize + SameLine.
void renderSegmentsWrapped(const std::vector<Segment>& segs, float indent) {
    const float wrapX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    bool lineStarted = false;

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);

    const ImVec4 textCol   = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 boldCol   = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    const ImVec4 italicCol = ImVec4(textCol.x * 0.85f, textCol.y * 0.85f, textCol.z * 0.95f, textCol.w);
    const ImVec4 codeCol   = ImVec4(0.55f, 0.95f, 0.65f, 1.0f);

    for (const auto& seg : segs) {
        ImVec4 col = textCol;
        if (seg.code) col = codeCol;
        else if (seg.bold) col = boldCol;
        else if (seg.italic) col = italicCol;

        // Wortweise ausgeben
        size_t p = 0;
        while (p < seg.text.size()) {
            size_t sp = seg.text.find(' ', p);
            std::string word = (sp == std::string::npos)
                ? seg.text.substr(p)
                : seg.text.substr(p, sp - p + 1); // Leerzeichen am Wort behalten
            p = (sp == std::string::npos) ? seg.text.size() : sp + 1;
            if (word.empty()) continue;

            ImVec2 sz = ImGui::CalcTextSize(word.c_str());
            if (lineStarted) {
                ImGui::SameLine(0.0f, 0.0f);
                if (ImGui::GetCursorPosX() + sz.x > wrapX) {
                    ImGui::NewLine();
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                }
            }
            ImGui::TextColored(col, "%s", word.c_str());
            lineStarted = true;
        }
    }
    if (!lineStarted) ImGui::NewLine(); // leere Zeile → Abstand
}

} // namespace

void MarkdownText::Render(const std::string& text) {
    const ImVec4 h1Col(0.0f, 0.85f, 1.0f, 1.0f);
    const ImVec4 h2Col(0.3f, 0.9f, 0.9f, 1.0f);
    const ImVec4 h3Col(0.5f, 0.85f, 0.75f, 1.0f);
    const ImVec4 bulletCol(0.0f, 0.8f, 1.0f, 1.0f);

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? text.substr(pos)
            : text.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        // Trailing CR entfernen
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Führende Spaces für Listen-Einrückung zählen
        size_t lead = line.find_first_not_of(' ');
        if (lead == std::string::npos) { ImGui::Spacing(); continue; }
        std::string trimmed = line.substr(lead);

        // Trennlinie
        if (trimmed == "---" || trimmed == "***" || trimmed == "___") {
            ImGui::Separator();
            continue;
        }

        // Überschriften
        int hLevel = 0;
        while (hLevel < 3 && hLevel < static_cast<int>(trimmed.size()) && trimmed[hLevel] == '#') ++hLevel;
        if (hLevel > 0 && hLevel < static_cast<int>(trimmed.size()) && trimmed[hLevel] == ' ') {
            std::string heading = trimmed.substr(hLevel + 1);
            ImGui::Spacing();
            const ImVec4& col = (hLevel == 1) ? h1Col : (hLevel == 2) ? h2Col : h3Col;
            ImGui::TextColored(col, "%s", heading.c_str());
            if (hLevel <= 2) ImGui::Separator();
            continue;
        }

        // Aufzählungen (- oder *), Einrückungstiefe aus führenden Spaces
        if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*') && trimmed[1] == ' ') {
            float indent = 14.0f + static_cast<float>(lead) * 0.5f * ImGui::GetFontSize();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent - 14.0f);
            ImGui::TextColored(bulletCol, "%s", "\xE2\x80\xA2"); // •
            ImGui::SameLine(0.0f, 6.0f);
            renderSegmentsWrapped(parseInline(trimmed.substr(2)), 0.0f);
            continue;
        }

        // Nummerierte Listen: "1. ", "12. "
        {
            size_t d = 0;
            while (d < trimmed.size() && isdigit(static_cast<unsigned char>(trimmed[d]))) ++d;
            if (d > 0 && d + 1 < trimmed.size() && trimmed[d] == '.' && trimmed[d + 1] == ' ') {
                float indent = 14.0f + static_cast<float>(lead) * 0.5f * ImGui::GetFontSize();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent - 14.0f);
                ImGui::TextColored(bulletCol, "%s", trimmed.substr(0, d + 1).c_str());
                ImGui::SameLine(0.0f, 6.0f);
                renderSegmentsWrapped(parseInline(trimmed.substr(d + 2)), 0.0f);
                continue;
            }
        }

        // Normaler Fließtext
        renderSegmentsWrapped(parseInline(trimmed), 0.0f);
    }
}
