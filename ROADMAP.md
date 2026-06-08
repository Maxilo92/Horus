# Roadmap — Project Horus / Tactileviewer

Geplante Features und Plattform-Erweiterungen. Reihenfolge spiegelt Priorität wider.

---

## Kurzfristig (v1.17.x)

- [ ] **Bug-Fixes Beta** — Abbau der bekannten Instabilitäten aus v1.16.7-beta
- [ ] **Update-Dialog polishing** — Versionshinweis persistent dismissbar (pro Version, nicht nur pro Session)
- [ ] **CI-gestützte Release-Pipeline** — GitHub Actions baut und hängt Artefakte automatisch an Horus-Releases an

---

## Mittelfristig (v1.18.x – v1.20.x)

- [ ] **Automatischer Download** — Optionaler In-App-Download des Updates via CURL (statt Browser-Weiterleitung)
- [ ] **Verbessertes Face-Recognition-Matching** — Schwellwert-Tuning und Konfidenzanzeige im Dossier
- [ ] **ROI-Import/Export** — Zonen als JSON speichern und zwischen Sessions laden
- [ ] **Replay-Panel Erweiterungen** — Frame-genaue Navigation, Exportfunktion

---

## Linux-Port (geplant, kein fester Termin)

Aufwand: **mittel (~1–2 Tage Portierungsarbeit + CI-Setup)**

Blockierende Aufgaben:
- [ ] Audio-Backend abstrahieren (`AudioBackend`-Interface)
- [ ] macOS-spezifische Frameworks ersetzen: `AudioToolbox` / `CoreAudio` / `CoreFoundation` → ALSA oder PulseAudio
- [ ] CMakeLists.txt plattformbedingte Abhängigkeiten (`if(APPLE)` / `if(LINUX)`)
- [ ] Packaging: `.AppImage` oder `.deb` statt `.app`-Bundle
- [ ] GitHub Actions Runner: `ubuntu-latest`

Technische Einschätzung: Der Kern (OpenCV, CURL, ImGui, GLFW, SQLite) ist bereits plattformunabhängig. Der einzige macOS-Lock-in ist das Audio-System.

---

## Windows-Port (geplant, kein fester Termin)

Aufwand: **hoch (~3–5 Tage)**

Blockierende Aufgaben:
- [ ] Audio-Backend: `AudioToolbox` → WASAPI oder XAudio2
- [ ] Abhängigkeitsverwaltung via `vcpkg` (OpenCV, CURL, GLFW, GLEW als Windows-Binaries)
- [ ] MSVC-Kompatibilität prüfen (C++17 filesystem, Threading, `#pragma` differences)
- [ ] DLL-Deployment (alle `.dll`s ins Release-Verzeichnis)
- [ ] Packaging: NSIS- oder WiX-Installer für `.exe`
- [ ] Code-Signing (optional, verhindert Windows-Defender-Warnung)
- [ ] GitHub Actions Runner: `windows-latest`

Technische Einschätzung: Größte Hürde ist die DLL-Verwaltung und das Audio-Backend. Mit vcpkg und einem abstrahierten Audio-Layer ist der Port realistisch.

---

## Langfristig / Ideen

- [ ] **Netzwerk-Streaming** — RTSP-Output für Live-Übertragung an externe Clients
- [ ] **Plugin-System** — Erweiterbare Detektoren ohne Recompile
- [ ] **Mobile Companion App** — Status-Ansicht und Alerts auf iOS/Android
