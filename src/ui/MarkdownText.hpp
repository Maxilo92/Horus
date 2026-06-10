#pragma once
#include <string>

// Leichtgewichtiger Markdown-Renderer für ImGui — gedacht für die KI-Dossier-
// Texte (Archiv, Live-Dossier-Panel). Unterstützt die Untermenge, die VLMs
// typischerweise produzieren:
//   #/##/### Überschriften, - / * Aufzählungen (mit Einrückung), 1. Listen,
//   **fett**, *kursiv*, `code`, --- Trennlinien.
// Fließtext wird wortweise umbrochen, sodass Inline-Formatierung über
// Zeilengrenzen funktioniert.
namespace MarkdownText {
    void Render(const std::string& text);
}
