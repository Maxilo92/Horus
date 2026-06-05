# Changelog

## [1.6.1] - 2026-06-05
### Added
- **Native macOS App Bundle Support:** Configured CMake to generate a `.app` bundle using `MACOSX_BUNDLE`.
- **Bundle Configuration:** Integrated `Info.plist` for application metadata, including Camera usage description.
- **Resource Bundling:** Automatisches Kopieren von KI-Modellen und Labels in das `Resources`-Verzeichnis des Bundles.
### Changed
- **Adaptive Path Resolution:** Refactored `Application.cpp` to prioritize resource loading from the macOS bundle structure while maintaining development fallbacks.

## [1.6.0] - 2026-06-05
### Added
- **Architectural Refactoring:** Organized source files into logical subdirectories (`core`, `vision`, `tracking`, `ui`) for improved maintainability and "Military-Grade" clarity.
- **Resource Management:** Moved AI models and labels to a dedicated `assets/models` directory.
### Changed
- **CMake Modernization:** Updated `CMakeLists.txt` to support the new directory structure and modular include paths.
- **Path Resolution:** Refactored `Application.cpp` to use the new relative and absolute paths for model loading.

## [1.5.2] - 2026-06-05

### Changed
- **Viewport-Isolated HUD:** Implemented strict clipping for the tactical HUD using `PushClipRect`, ensuring all overlays (crosshairs, brackets, trails) are confined to the video area, even when letterboxed.
- **Refactored Status Windows:** Replaced ImGui-based status windows with direct `ImDrawList` primitives. This eliminates global window management overhead and ensures UI elements respect the HUD's coordinate system and clipping.
- **Enhanced Telemetry Layout:** Repositioned "Data" (FPS, TRK) to the top-left and "SysLog" (Status, Active Threat) to the bottom-left of the viewport with a high-contrast, semi-transparent HUD aesthetic.

## [1.5.4] - 2026-06-05
### Added
- **Independent Camera Telemetry:** Implemented high-precision camera FPS calculation in the worker loop, decoupled from the main UI framerate.
- **Improved HUD Telemetry Accuracy:** The tactical HUD now displays actual camera processing performance instead of ImGui render speed.
- **Thread-Safe Telemetry Sync:** Established a new synchronization pattern for worker-to-UI telemetry data using the existing mutex architecture.

## [1.5.3] - 2026-06-05
### Fixed
- **Unit Test Build Errors:** Resolved build failures in `unit_tests.cpp` caused by the removal of `SingleTracker` from the core project.
- **MOT Test Integration:** Updated the automated test suite to verify `MultiTracker` functionality, including stable ID assignment, Kalman-based prediction, and track lifecycle management.
- **Googletest Compatibility:** Fixed a build-breaking conflict in `googlemock` related to C++17 `std::index_sequence` by disabling GMock (which was unused) in `CMakeLists.txt`.
- **Test Logic Refinement:** Corrected `MultiTracker` validation logic to account for the confirmation delay (tracks are confirmed on their first match, not creation).

## [1.5.2] - 2026-06-05
### Added
- **Formal Source Control Integration:** Initialized local Git repository with optimized `.gitignore` for C++ development.
- **Git Usage Mandates:** Updated `GEMINI.md` with binding rules for atomic commits and meaningful messages for all agents.
- **Enhanced Dev Console:** Added real-time sliders for Score Threshold, NMS Threshold, and a toggle for movement trails (MOT).
### Changed
- **MOT HUD Integration:** Refactored HUD rendering to use tracked objects vector instead of raw detections.
- **Version Management:** Synchronized `VERSION` and `CHANGELOG.md` with the new repository state.

## [1.5.1] - 2026-06-05
### Changed
- **Optimized MultiTracker Responsiveness:** Eliminated the 1-frame "confirmation lag" for new detections. Tracks are now displayed immediately upon first detection (if fresh), while retaining the confirmed/unconfirmed distinction for persistent dead-reckoning.
- **Enhanced HUD Interactions:** Brand new tracks are now searchable (`findNearestTrack`) and counted (`getActiveTrackCount`) instantly, improving the feel of interactive target locking.

## [1.5.0] - 2026-06-05
### Added
- **Automated Test Suite:** Integrated Google Test (GTest) framework. Implemented 11 unit tests covering `SingleTracker`, `ObjectDetector`, and `CameraModule` to ensure "watertight" reliability.
- **ABI Stability Fix:** Resolved complex linking issues between system GTest headers and FetchContent binaries by enforcing header priority and direct source compilation.
- **Failsafe Verification:** Added tests for handling invalid model paths, empty video frames, and out-of-bounds input parameters.
### Fixed
- **Tracker Parameter Sync:** Fixed `SingleTracker` to correctly respect `SystemSettings` (e.g., `maxLostFrames`) instead of using hardcoded values.
- **Application Member Alignment:** Synchronized `Application.cpp` with latest `Application.hpp` structure (SystemSettings integration).
- **HUD API Compatibility:** Fixed `HUD::render` call sites to match updated parameter signatures.

## [1.4.1] - 2026-06-05
### Added
- **Letterbox Resizing:** Implemented aspect-ratio-preserving resize with padding for YOLOv8 input in `ObjectDetector`. Improves detection accuracy for objects near frame edges by eliminating image distortion (squashing).
### Changed
- **Detector Sensitivity:** Lowered default `detectorScoreThreshold` from 0.45 to 0.25 to improve recall for small or partially occluded objects.

## [1.4.0] - 2026-06-05
### Added
- **Multi-Object-Tracker (MOT):** Neuer `MultiTracker` mit Kalman-Filter (8D-State: Position + Geschwindigkeit) und hybridem Matching-Algorithmus. Verfolgt mehrere Objekte gleichzeitig mit stabilen Track-IDs.
- **Hybrides IoU + Distanz-Matching:** Bei schnell-bewegten Objekten (Personen, Autos, Fahrräder) greift ein normalisierter Zentrum-Distanz-Score als Fallback wenn IoU ≈ 0. Adaptiver Gate skaliert mit Objektgröße.
- **Klassen-spezifische Farbkodierung:** Person=Amber, Fahrrad=Cyan, Auto=Grün, Motorrad=Orange, Bus=Lime, Truck=Gelbgrün.
- **Bewegungspfad (Trail):** Centroid-History mit Alpha-Fade für jedes getrackte Objekt. Konfigurierbare Trail-Länge (0–60 Punkte).
- **Track-ID Badge:** Stabile, monoton steigende ID über jeder Bounding Box (`PRS #042`).
- **Confidence-Balken:** Visueller Balken am unteren Rand jeder Box zeigt Detektor-Konfidenz.
- **Priority-Class-Filter:** Nur relevante Verkehrsklassen (Person/Bicycle/Car/Motorcycle/Bus/Truck) aktiv. Einzeln konfigurierbar per Checkbox.
- **Rechtsklick Lock-Release:** Rechtsklick im Camera-View gibt den Single-Target-Lock frei.
### Fixed
- **coco.txt Parser-Bug:** Alle 80 COCO-Klassen lagen in einer einzigen Zeile. `getline()` las nur einen String → alle Bounding Boxes zeigten leere Klassennamen. Jetzt eine Klasse pro Zeile.
- **Bounds-Check in ObjectDetector:** Verhindert undefined behavior bei class_id außerhalb des Klassen-Arrays.
- **Kalman-Prozessrauschen:** Hohe Unsicherheit für Geschwindigkeitskomponenten (vx, vy: 5.0) ermöglicht schnelle Anpassung an Richtungsänderungen (Fußgänger, Fahrräder).
### Changed
- `HUD::render()` nimmt jetzt `std::vector<TrackedObject>` statt raw Detections.
- Dev Console erweitert: MOT-Einstellungen, Trail-Länge, Klassen-Filter-Checkboxen.

## [1.3.1] - 2026-06-05
### Fixed
- **Camera Initialization:** Fixed an issue where the camera was not being opened during application initialization.
### Added
- **Camera Control Section:** Added a new section to the Dev Console to monitor camera status and change the camera source/ID dynamically.

## [1.3.0] - 2026-06-05
### Added
- **Centralized System Settings:** Introduced a unified `SystemSettings` architecture to control all system parameters in real-time.
- **Advanced Dev Console:** Overhauled the Dev Console with collapsible sections for Detector, Tracker, and HUD settings.
- **Detector Tuning:** Added sliders for Confidence, Score, and NMS thresholds.
- **Tracker Tuning:** Added controls for Max Lost Frames, Velocity Smoothing, Dead Reckoning Damping, and Min Match IOU.
- **HUD Customization:** Added toggles for all HUD elements (Overlay, Crosshair, Brackets, etc.) and real-time color pickers for HUD and Target visuals.
- **Manual Lock Release:** Added a button to manually release the current target lock.
### Changed
- Thread-safe settings synchronization between UI and processing threads.

## [1.2.1] - 2026-06-05
### Changed
- **Windowed Camera Feed:** The camera feed is no longer rendered as the application background. It now resides in a dedicated "Camera View" ImGui window.
- **Viewport-Relative HUD:** Refactored the tactical HUD and status windows to be relative to the camera viewport instead of the global window.
- **Improved Target Locking:** Interaction logic updated to ensure target selection only occurs when clicking directly on the camera feed.

## [1.2.0] - 2026-06-05
### Added
- **Single-Target-Lock (High Precision):** New `SingleTracker` module combining YOLO detections with a linear velocity prediction model (Dead Reckoning).
- **Interactive Target Selection:** Added mouse-based locking mechanism. Clicking on a YOLO bounding box "locks" the target.
- **Robustness Features:** Implemented IOU-based validation to prevent lock-jumping and automated prediction during brief signal loss (occlusion).
- **HUD Target Overlay:** Distinctive red UI visuals for the locked target, including detailed tracking telemetry.
### Changed
- Refactored `Application` and `HUD` to support persistent tracking states (`LOCKED`, `SEARCHING`, `LOST`).
- Optimized `CMakeLists.txt` and core source structure.
- Removed unused/broken ImGui Docking remnants to ensure build stability.

## [1.1.0] - 2026-06-05
