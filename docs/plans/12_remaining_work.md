# Plan 12: Verbleibende Arbeiten nach Architektur-Refactoring

## Übersicht

Die modulare Blackboard-Architektur (Plan 11) ist fertiggestellt und läuft stabil.
Dieser Plan erfasst alle verbleibenden Punkte — aufgeteilt in drei Prioritätsstufen.

---

## Priorität 1 — Konkrete Bugs / Lücken im aktuellen Build

### 1.1 classNames nie auf Blackboard geschrieben ⬜

**Problem:** `VisionSystem::init()` erstellt den `ObjectDetector`, ruft aber nie
`m_blackboard.setClassNames()` auf. `UIManager` liest `status.classNames` —
das Array bleibt leer. Im Dev Console → Detector-Tab sind keine Klassen wählbar.

**Fix:** In `VisionSystem::init()` nach `m_detector`-Erstellung:
```cpp
AppStatusState st = m_blackboard.getAppStatus();
st.classNames = m_detector->getClasses();
m_blackboard.setAppStatus(st);
```

**Betroffene Dateien:** `src/vision/VisionSystem.cpp`

---

### 1.2 Blackboard::getVisionState() kopiert cv::Mat-Frames ⬜

**Problem:** Jeder `getVisionState()`-Call in `UIManager::update()` kopiert drei
volle `cv::Mat`s (`rawFrame`, `trackingFrame`, `heatmapFrame`). Bei 1080p sind
das ~6 MB pro Frame-Copy, ~250 MB/s unnötige Speicherbandbreite.

**Fix:** `rawFrame` und `trackingFrame` werden von UIManager nicht direkt
gerendert (nur `heatmapFrame` über den Heatmap-Renderer, `zoomCrop` über den
Zoom-Renderer). Die schweren Frames aus `VisionState` entfernen — UIManager
nutzt für das Hauptbild ausschließlich den DisplayFrame-Fast-Path (bereits korrekt
über `consumeDisplayFrame()`).

**Konkrete Schritte:**
1. `VisionState`-Struct: `rawFrame` und `trackingFrame` entfernen.
2. `VisionSystem::workerLoop()`: Diese Felder nicht mehr in `vState` kopieren.
3. `UIManager_CameraView.cpp` und `UIManager_Panels.cpp`: prüfen ob `vision.rawFrame`
   oder `vision.trackingFrame` direkt verwendet wird — ggf. ersetzen.

**Betroffene Dateien:** `src/core/Blackboard.hpp`, `src/vision/VisionSystem.cpp`,
`src/ui/UIManager_CameraView.cpp`

---

### 1.3 AppStatus-Mutex-Contention im Capture-Thread ⬜

**Problem:** `captureWorkerLoop` inkrementiert `totalFramesProcessed` bei jedem
Frame über `getAppStatus()` + `setAppStatus()` — das ist ein vollständiger
Mutex-Lock/Unlock + Struct-Copy pro Frame (bei 30 fps = 60 Mutex-Ops/s, harmlos
aber unnötig).

**Fix:** Blackboard-API `updateStatusCounts(int det, int track, int frames)`
existiert bereits (Zeile 311 in `Blackboard.hpp`). `captureWorkerLoop` sollte
direkt `m_blackboard.updateStatusCounts(0, 0, 1)` nutzen statt get+set.

**Betroffene Dateien:** `src/vision/VisionSystem.cpp` (captureWorkerLoop)

---

## Priorität 2 — Qualität & Robustheit

### 2.1 ThreadSanitizer-Lauf ⬜

Das Refactoring hat mehrere neue Mutex-Grenzen eingeführt. Ein TSan-Lauf
sichert ab, dass keine Data Races existieren.

**Schritte:**
```bash
cmake -S . -B build_tsan \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build_tsan
./build_tsan/Tactileviewer.app/Contents/MacOS/Tactileviewer 1
```
App 60 Sekunden laufen lassen, Ausgabe auf `ThreadSanitizer: DATA RACE` prüfen.

**Erwartete Ergebnisse:** Keine Races, da alle Blackboard-Zugriffe durch eigene
Mutexes geschützt sind. Sollte ein Race gefunden werden, ist es sehr wahrscheinlich
in `UIManager_CameraView.cpp` (ROI-Edit-State ohne Lock).

---

### 2.2 FPS-Performance-Baseline ⬜

Vergleich: Alter monolithischer Build (git-gesicherte Version vor Plan 11) vs.
neuer modularer Build auf identischem Testmaterial.

**Metriken:**
- Render FPS (Ziel: ≥ vorheriger Wert)
- Camera FPS (Ziel: identisch)
- Frame Time ms (Ziel: ≤ vorheriger Wert)

Dev Console → System-Tab liefert alle drei Werte live.

---

### 2.3 Kamera-Wechsel (Hot-Swap) End-to-End testen ⬜

Settings → Camera & Zoom → Neue Adresse eingeben → Apply.
`Blackboard.requestCameraChange()` → `VisionSystem::captureWorkerLoop` konsumiert.

**Prüfpunkte:**
- Neuer Feed erscheint ohne App-Neustart
- `cameraAddress` im AppStatus wird aktualisiert
- UIManager-InputBuffer zeigt neue Adresse

---

### 2.4 Settings-Persistenz nach Neustart verifizieren ⬜

Einstellungen ändern (z.B. HUD-Farbe, Detection-Threshold) → App schließen →
neu starten → Werte müssen wiederhergestellt sein.

`UIManager::stop()` → `savePersistedSettings()` → nächster Start →
`UIManager::start()` → `loadPersistedSettings()` → `pushSettingsToBlackboard()`.

---

### 2.5 Lock / Pixel-Lock Funktion ⬜

Ziel auf ein Objekt klicken → Lock aktivieren → Tracking-Rahmen erscheint.
Pixel-Lock: Rechtsklick auf Frame-Bereich.

Prüft: `m_blackboard.requestTargetLock(id)` → `TrackingSystem` konsumiert →
`TrackingStateData.lockedTarget` auf Blackboard → UIManager zeigt HUD.

---

## Priorität 3 — Technische Schulden / Zukunft

### 3.1 Blackboard Double-Buffer für Display-Frame (Optional) ⬜

Aktuell: `updateDisplayFrame()` und `consumeDisplayFrame()` teilen einen Mutex.
Bei sehr hohen Frame-Raten (4K/60fps) könnte der Mutex zum Flaschenhals werden.

**Verbesserung:** Triple-Buffer-Schema — drei Slots (write, ready, display).
Writer rotiert in `write`-Slot, setzt atomaren `ready`-Index; Reader tauscht
`ready` und `display` atomar aus. Keine Mutex-Contention.

Nur nötig wenn TSan oder Performance-Profiling auf diesen Engpass zeigt.

---

### 3.2 UIManager Panel-Klassen (Optional) ⬜

`UIManager` hat ~50 Member-Variablen und 9 Quelldateien — funktional, aber für
einen neuen Entwickler unübersichtlich. Mittelfristig könnten einzelne Panels
eigene State-Structs oder Klassen werden:

| Panel | Kandidat für eigene Klasse |
|---|---|
| `renderTargetAnalyzer` | `AnalyzerPanel` (hat eigenen Texture-Cache) |
| `renderDevConsole` | `DevConsolePanel` (hat eigenen Log-State) |
| `renderSettingsWindow` | `SettingsPanel` (hat eigene Preset-Logik) |

Vorbedingung: Plan 2.1 (TSan) und 2.2 (Performance) müssen grün sein.

---

### 3.3 Validierungsprotokoll (Plan 09) ⬜

Aus `09_validation_protocol.md`:
- Ground-Truth-Vergleich mit manuell annotierten Referenz-Videos
- 72-Stunden-Dauertest (Memory Leaks, Drift)
- Latenz-Analyse (Glass-to-Glass)
- Automatisierter Validierungs-PDF-Report

Diese Punkte setzen einen stabilen Build voraus und sind der finale QA-Gate
vor einem produktiven Einsatz.

---

### 3.4 README und docs/plans/README.md aktualisieren ⬜

`docs/plans/README.md` endet bei Plan 9 (Zeile 15 abgeschnitten).
Pläne 10, 11, 12 eintragen.

---

## Reihenfolge-Empfehlung

```
1.1 classNames-Fix       ← 15 Minuten, sofortiger UI-Gewinn
1.2 VisionState-Bloat    ← 30 Minuten, Speicherbandbreite halbiert
1.3 AppStatus-Contention ← 10 Minuten, sauberes API-Nutzen
2.1 TSan-Lauf            ← 1 Stunde Setup + Laufzeit
2.2 FPS-Baseline         ← 30 Minuten
2.3 Hot-Swap Test        ← 20 Minuten
2.4 Settings-Persistenz  ← 15 Minuten
2.5 Lock/Pixel-Lock      ← 20 Minuten
3.x                      ← nach Bedarf
```

## Status-Übersicht

| # | Titel | Priorität | Status |
|---|---|---|---|
| 1.1 | classNames auf Blackboard | Bug | ✅ Erledigt |
| 1.2 | VisionState ohne schwere Frames | Performance | ✅ Erledigt |
| 1.3 | AppStatus-Contention | Performance | ✅ Erledigt |
| 2.1 | ThreadSanitizer-Lauf | Qualität | ✅ Erledigt (Clean) |
| 2.2 | FPS-Baseline | Qualität | ✅ Erledigt (Opt. impl.) |
| 2.3 | Hot-Swap End-to-End | Qualität | ✅ Erledigt (verified) |
| 2.4 | Settings-Persistenz | Qualität | ✅ Erledigt (verified) |
| 2.5 | Lock / Pixel-Lock | Qualität | ✅ Erledigt (verified) |
| 3.1 | Display-Frame Double-Buffer | Optional | ⬜ Offen |
| 3.2 | UIManager Panel-Klassen | Optional | ⬜ Offen |
| 3.3 | Validierungsprotokoll (Plan 09) | QA-Gate | ⬜ Offen |
| 3.4 | README aktualisieren | Docs | ✅ Erledigt |
