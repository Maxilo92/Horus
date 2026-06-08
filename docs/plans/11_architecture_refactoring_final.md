# Plan: Architecture Refactoring (Blackboard / StateStore)

## Objective
Refactor the monolithic `Application` class into a modular architecture using a central Blackboard (StateStore) pattern. This will decouple the Vision, Tracking, and UI systems, improving scalability, making it easier to integrate new features without side-effects, and adhering strictly to the military-grade requirements (determinism, thread safety, zero-allocation in hot loops).

## Scope & Impact
- **Impacts**: `src/core/Application.cpp`, `src/core/Application.hpp`, UI rendering logic, Thread management.
- **Does Not Impact**: Core algorithmic logic inside `MultiTracker`, `ObjectDetector`, `CameraModule`, or `FaceRecognizer`. These will remain functionally identical but will be orchestrated differently.

## Key Files & Context
- `Project_Horus/src/core/Application.cpp / .hpp` -> Will be drastically reduced to a bootstrapper.
- `Project_Horus/src/core/Blackboard.hpp` (NEW) -> Central thread-safe state store.
- `Project_Horus/src/core/IModule.hpp` (NEW) -> Standard interface for subsystems.
- `Project_Horus/src/ui/UIManager.hpp / .cpp` (NEW) -> Extracts all ImGui window logic.
- `Project_Horus/src/vision/VisionSystem.hpp / .cpp` (NEW) -> Orchestrates camera, detection, and motion.
- `Project_Horus/src/tracking/TrackingSystem.hpp / .cpp` (NEW) -> Orchestrates tracking, ROIs, and target locking.

## Proposed Solution: The Blackboard Pattern
We will introduce a `Blackboard` class that holds isolated state structs (e.g., `VisionState`, `TrackingState`, `UIState`). Modules will run independently and communicate exclusively by reading from and writing to the Blackboard using double-buffered or granularly locked data structures to ensure real-time performance without blocking.

---

## Implementation Steps & Current Status

### ✅ Phase 1: Foundation — COMPLETE

1. **`IModule.hpp`** — `src/core/IModule.hpp` ✅ Done
   ```cpp
   class IModule {
   public:
       virtual ~IModule() = default;
       virtual void start() = 0;
       virtual void stop() = 0;
       virtual void update() = 0;
   };
   ```

2. **`Blackboard.hpp`** — `src/core/Blackboard.hpp` ✅ Done (346 lines)
   - Structs: `MotionTrack`, `SubZoomData`, `VisionState`, `DetectionState`, `TrackingStateData`, `UICommandState`, `AppStatusState`
   - API: `setSettings/getSettings`, `setVisionState/getVisionState`, `setDetectionState/getDetectionState`, `setTrackingState/getTrackingState`, `setAppStatus/getAppStatus`, `consumeUICommand()`, `updateDisplayFrame/consumeDisplayFrame` (fast-path), alle request-Methoden für Commands.

---

### ✅ Phase 2A: VisionSystem — COMPLETE

- **`src/vision/VisionSystem.hpp`** ✅ Done
- **`src/vision/VisionSystem.cpp`** ✅ Done (548 lines)
  - `captureWorkerLoop`: liest Kamera, ruft `m_blackboard.updateDisplayFrame()` auf.
  - `detectorWorkerLoop`: YOLO-Inferenz asynchron.
  - `workerLoop`: Motion Detection, Sub-Zoom-Management, ruft `m_trackingSystem->submitFrame()` auf, pusht `VisionState` und `DetectionState` auf Blackboard.

---

### ✅ Phase 2B: TrackingSystem — COMPLETE

- **`src/tracking/TrackingSystem.hpp`** ✅ Done
- **`src/tracking/TrackingSystem.cpp`** ✅ Done (687 lines)
  - `trackingWorkerLoop`: MultiTracker, ROI-Filterung, Pixel-Lock, Face Recognition, Alarm-Zonen, Target-History.
  - `submitFrame()`: Thread-safe Übergabe von VisionSystem zu TrackingSystem.
  - Pusht `TrackingStateData` auf Blackboard nach jeder Tracking-Iteration.
  - Konsumiert `UICommandState` vom Blackboard (Lock, Release, PixelLock, etc.)

---

### ✅ Phase 2C: UIManager — COMPLETE (Build erfolgreich)

#### UIManager.hpp ✅ DONE
- `src/ui/UIManager.hpp` — vollständige Deklaration aller Render-Methoden, Member-Variablen.
- Konstruktor: `UIManager(Blackboard&, ROIManager&, DataLogger&, AudioEngine&, GLFWwindow*, const std::string& settingsPath, LogFn)`

#### Erstellte Quelldateien ✅

| Datei | Inhalt |
|---|---|
| `UIManager.cpp` | Konstruktor, initRenderers, start/stop, appendLog, updateGLTexture, update() Hauptschleife |
| `UIManager_Settings.cpp` | loadPersistedSettings, savePersistedSettings, Presets, syncColorEditors, pushSettingsToBlackboard |
| `UIManager_Panels.cpp` | renderDataPanel (Active Tracks, Target History, Export & Logging), renderZoomWindow |
| `UIManager_CameraView.cpp` | renderCameraView: Video-Image, ROI-Edit, Pixel-Lock, Sub-Zoom-Inserts (inkl. Kollisionsvermeidung), HUD-Render |
| `UIManager_DevConsole.cpp` | renderDevConsole: System, Detector, Tracker, HUD, Console, ROI Tabs |
| `UIManager_SettingsWindow.cpp` | renderSettingsWindow: Display&HUD, Camera&Zoom, Detection&Tracking, Audio Alerts, System&Admin |
| `UIManager_Export.cpp` | exportTarget, exportTargetHistory, takeScreenshot, saveFeedback |
| `UIManager_Analyzer.cpp` | renderTargetAnalyzer: Gallery-View, Single-View, Large-View-Modal |
| `UIManager_internal.hpp` | Shared private helpers: ApplyBrightnessLocal, Float4ToImU32, ImU32ToFloat4, EscapeJsonString |

#### Fixes während der Compilation
- `LogLevel` + `ConsoleEntry` in `Common.hpp` verschoben (war nur in `Application.hpp`)
- `Application.hpp`: doppelte Definition entfernt
- `VisionSystem.cpp L173`: ungültige `auto* = nullptr` placeholder entfernt
- `UIManager_Export.cpp`: `GL/gl.h` → GLFW-Header (macOS)
- `UIManager_Settings.cpp`: `HUD::kDefaultMotionColor` → `kDefaultMotionColor` (file-level static)
- `UIManager.hpp`: private `ViewportInfo` struct entfernt (kollidiert mit globalem in Common.hpp)

**Kritische API-Substitutionen** (Application → UIManager):

| Alt (Application)                              | Neu (UIManager / Blackboard)                                |
|------------------------------------------------|-------------------------------------------------------------|
| `log(level, msg)`                              | `m_log(level, msg)`                                        |
| `syncSettingsToSharedState()`                  | `pushSettingsToBlackboard()` → `m_blackboard.setSettings(m_settings)` |
| `m_lockRequested.store(true); m_requestedLockId.store(id)` | `m_blackboard.requestTargetLock(id)`         |
| `m_releaseLockRequested.store(true)`           | `m_blackboard.requestTargetRelease()`                       |
| `m_screenshotRequested = true`                 | `m_blackboard.requestScreenshot()`                          |
| `m_pixelLockRequested.store(true)`             | `m_blackboard.requestPixelLock(rect)`                       |
| `m_pixelLockDragging.store(v)`                 | `m_blackboard.setPixelLockDragging(v)`                      |
| `m_pixelLockRectUpdateRequested.store(true)`   | `m_blackboard.requestPixelLockRectUpdate(rect)`             |
| `m_cameraChangeRequested.store(true); m_pendingCameraAddress=addr` | `m_blackboard.requestCameraChange(addr)` |
| `m_cameraChangeRequested.load()`               | `m_blackboard.isCameraChangePending()`                      |
| `m_workerDetectionCount.load()`                | `m_blackboard.getAppStatus().workerDetectionCount`          |
| `m_workerTrackCount.load()`                    | `m_blackboard.getAppStatus().workerTrackCount`              |
| `m_totalFramesProcessed.load()`                | `m_blackboard.getAppStatus().totalFramesProcessed`          |
| `m_detector->getClasses()` / `numClasses()`   | `m_classNames` / `m_classNames.size()`                      |
| `m_motionDetector.reset()`                     | `m_blackboard.requestMotionDetectorReset()`                 |
| `m_manualCaptureTargetId = id`                 | `m_blackboard.requestManualCapture(id)`                     |
| `m_cameraAddress`                              | `m_blackboard.getAppStatus().cameraAddress`                  |
| `m_cameraStatus` / `m_cameraStatusOk`          | `m_blackboard.getAppStatus().cameraStatus` / `.cameraStatusOk` |
| Shared-Data-Mutex: `m_sharedSettings`          | `m_blackboard.getSettings()` / `setSettings()`              |
| `m_sharedLockedTarget.box = newRect`           | direkt auf lokale Kopie + `requestPixelLockRectUpdate(rect)` |

**UIManager.cpp Aufgaben-Checkliste:**

- [x] **Task 1**: Konstruktor (korrektes Matching mit HPP-Signatur), `~UIManager`, `appendLog()`, `initRenderers()`, `start()`, `stop()`
- [x] **Task 2**: Settings-Persistenz: `loadPersistedSettings()`, `savePersistedSettings()`, `syncColorEditorsFromSettings()`, `pushSettingsToBlackboard()`, alle Preset-Funktionen
- [x] **Task 3**: `renderDataPanel()` — aus Application.cpp Zeilen 1752–2174
- [x] **Task 4**: `renderCameraView()` — aus Application::run() Zeilen 3656–4441 (Camera View inkl. ROI-Edit, Pixel-Lock, Sub-Zoom-Inserts)
- [x] **Task 5**: `renderZoomWindow()` — aus Application.cpp Zeilen 2177–2295
- [x] **Task 6**: `renderDevConsole()` — aus Application.cpp Zeilen 2301–3089 (System, Detector, Tracker, HUD, Console, ROI Tabs)
- [x] **Task 7**: `renderSettingsWindow()` — aus Application.cpp Zeilen 3095–3396 (5 Tabs)
- [x] **Task 8**: Hilfsfunktionen: `exportTarget()`, `exportTargetHistory()`, `takeScreenshot()`, `saveFeedback()`, `updateGLTexture()`
- [x] **Task 9**: `renderTargetAnalyzer()` — aus Application.cpp Zeilen 4976–5353
- [x] **Task 10**: `update()` Hauptschleife — Blackboard-Snapshot, Render-Timing, Display-Frame, Texture-Updates, DockSpace, Menüleiste, Feedback-Popup, alle Render-Aufrufe, Keyboard-Shortcuts, OpenGL-Finalisierung

**Quell-Zeilen-Referenz (Application.cpp):**

| Funktion                    | Zeilen         |
|-----------------------------|----------------|
| Static helpers              | 20–125         |
| `syncColorEditorsFromSettings` | 311–321     |
| `savePersistedSettings()`   | 328–439        |
| `loadPersistedSettings()`   | 442–574        |
| Preset-Funktionen           | 576–645        |
| `renderDataPanel()`         | 1752–2174      |
| `renderZoomWindow()`        | 2177–2295      |
| `renderDevConsole()`        | 2301–3089      |
| `renderSettingsWindow()`    | 3095–3396      |
| `run()` (Camera View + Hauptschleife) | 3402–4541 |
| `saveFeedback()`            | 4543–4582      |
| `exportTarget()`            | 4819–4881      |
| `exportTargetHistory()`     | 4883–4934      |
| `takeScreenshot()`          | 4936–4974      |
| `renderTargetAnalyzer()`    | 4976–5353      |

---

### ⬜ Phase 3: Application Slim-Down — NEXT STEP

- `Application::init()`: `VisionSystem` und `TrackingSystem` instanziieren, verdrahten, starten.
- `Application::run()`: Nur noch GLFW-Polling + `m_uiManager->update()`.
- `Application.hpp`: Alle alten Worker-Threads, Atomics, Shared-Frames, Mutexes entfernen.

---

### ⬜ Phase 4: Kompilierung & Verifikation — PENDING

1. `cmake --build` — Linkfehler beheben.
2. ThreadSanitizer-Run — keine Data Races.
3. FPS-Baseline — Leistung gleich oder besser.
4. Validierungsprotokoll `09_validation_protocol.md` durchlaufen.

---

## Verification & Testing
1. **Compilation & Linkage**: Ensure all extracted modules compile and link correctly.
2. **Thread Safety Audit**: Run with ThreadSanitizer (TSAN) to ensure no data races exist around the new Blackboard.
3. **Performance Baseline**: Verify that FPS remains identical or improves compared to the monolithic architecture. Ensure no dynamic allocations occur in the `update()` loops.
4. **Military-Grade Validation**: Pass the `09_validation_protocol.md` to ensure determinism and integrity are maintained.

## Migration & Rollback
- Development will happen on a dedicated feature branch.
- Atomic commits will be used for each phase.
- If performance degrades, we can isolate the bottleneck in the Blackboard implementation (e.g., moving from fine-grained mutexes to lock-free buffers) without rolling back the entire structural change.
