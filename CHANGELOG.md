# Changelog

## [1.10.7] - 2026-06-05
### Fixed
- **Optimized 4K Camera Request (FPS & Format compatibility)**: Added MJPEG format request (`CAP_PROP_FOURCC`) to bypass USB bandwidth limitations for high-resolution streams on macOS/UVC. Reduced requested framerate from 60 FPS to 30 FPS for 4K streams to match hardware support, preventing the UVC camera driver from silently falling back to lower resolutions.
- **Explicit Backend Preference**: Forced `cv::CAP_AVFOUNDATION` backend explicitly when opening cameras on macOS to guarantee high-resolution settings support.
- **Live Camera Resolution Monitoring**: Added a "Camera Resolution" metric in the developer console System metrics table, rendering the actual feed width and height retrieved from the active camera source.

## [1.10.6] - 2026-06-05
### Changed
- **Default Camera Source**: Swapped the default camera UVC device index from `"0"` to `"1"` (representing USB camera #1) and ensured the UI text input displays this default on startup.

## [1.10.5] - 2026-06-05
### Added
- **HUD Bounding Box for Pixel Targets**: Custom pixel targets (ID 999) are now appended to the tracked objects list. This ensures the target's bounding box is rendered directly on the main Camera View HUD just like any standard YOLO-detected object.
- **First-Class Data Panel Integration**: Added custom pixel targets to the Developer Console Data Panel table, allowing users to monitor their live confidence, position, and status (e.g. `LOCKED`), and select or release them.

## [1.10.4] - 2026-06-05
### Added
- **High-Resolution Target Zoom**: Implemented a hybrid resolution pipeline. Configured the camera module to request 4K resolution (3840x2160) from hardware UVC camera indices (falling back to the maximum supported resolution). The processing thread resizes each raw camera frame to standard HD (1280x720) for the detection and tracking pipeline, but crops locked targets directly from the original high-resolution frame.
- **Zero-Heap Real-Time Copying**: Avoids heap allocation jitter by using persistent local cv::Mat buffers and `copyTo()` operations to transfer the cropped target and the HD view frame to the rendering thread.
- **Settings and Dev Console Toggles**: Integrated a "Request 4K camera resolution" toggle (which automatically hot-swaps/re-opens the camera feed when checked) and an "Enable 4K target zoom" toggle in both the floating Settings window and the System tab of the Developer Console.
- **Target Size Metrics Display**: Enhanced the Target Zoom overlay text to display the original HD bounding box coordinates alongside the high-resolution crop dimensions (e.g. `600x600 (HD: 200x200)`).
- **Test Discoverability Fix**: Increased GTest discovery timeout in CMake to 30 seconds to support library loading overhead on macOS.
- **Unit Testing**: Added a coordinate scaling math test verifying exact conversions and bounds clamping logic.

## [1.10.4] - 2026-06-05
### Added
- **Build and Run Automation:** Created `run.sh` in the project root to automate the build directory configuration (`cmake`), parallelized compilation (`make -j`), and application launch processes.

## [1.10.3] - 2026-06-05
### Added
- **Arbitrary Pixel Tracking (Custom Feature)**: Enabled user-defined feature tracking. Clicking on any blank/empty space in the Camera View (where no object bounding box is present) triggers a custom pixel tracker using OpenCV's Template Matching (`cv::matchTemplate` with `cv::TM_CCOEFF_NORMED`). Captures a 60x60 pixel template and tracks it frame-by-frame, updating its location, confidence score, and path history (trail) in real-time.
- **Full Zoom Support for Pixel Targets**: Custom pixel targets are fully integrated into the "Target Zoom" window, showing real-time zoom magnification and custom reticle overlay.

### Fixed
- **Improved Detection Recall**: Lowered default `detectorConfThreshold` and `detectorScoreThreshold` from `0.25f` to `0.15f` to successfully detect and track small, distant background vehicles (such as background cars on driveways or lanes).
- **Tracker Parameter Synchronization Bug**: Restored Developer Console slider control by updating `MultiTracker.cpp` to respect `trackerMinMatchScore` and `trackerMaxCenterDistPx` settings from the user interface instead of using hardcoded limits.

## [1.10.2] - 2026-06-05
### Added
- **Interactive Class Selection Grid**: Replaced the simple static class filter checkbox with a searchable grid (4 columns) showing all 80 COCO classes. Toggling a checkbox enables/disables the class in real-time.
- **Preset Selections**: Added preset options to quickly select/deselect classes: Select All, Clear All, Traffic & Pedestrians, Common (25 classes), Animals, and Indoor/Objects.
- **Expanded Default Priority Classes**: Increased default priority classes to a broader set of 25 common traffic/everyday objects (from 6).
- **Public Getter for Class Labels**: Added public `getClasses()` to the `ObjectDetector` to query loaded class labels dynamically.

### Fixed
- **Unit Test Path Stability**: Corrected paths to models/labels in `unit_tests.cpp` to refer to `assets/models/` instead of `models/`, resolving skipped tests in the GTest suite.

## [1.10.1] - 2026-06-05
### Fixed
- **Target Zoom View Distortion:** Resolved visual skewing, tiling, and horizontal tearing in the target zoom window when viewing locked targets of arbitrary size. Handled non-contiguous cv::Mat crop sub-matrices by specifying `GL_UNPACK_ROW_LENGTH` in OpenGL, and prevented skewing from unaligned row width sizes by setting `GL_UNPACK_ALIGNMENT` to 1.

## [1.10.0] - 2026-06-05
### Added
- **ROI Zone Functions:** Added three selectable functions/types for ROI zones:
  - *Detection (Include)*: Restricts tracking/detection to objects inside these zones (Default).
  - *Exclude (Ignore)*: Discards all detections/objects inside these zones, reducing noise from wind or traffic.
  - *Alarm*: Generates console warnings (LogLevel::WARN) when tracked objects cross into the zone, and info logs on exit.
- **Interactive Move & Resize:** Complete mouse gesture support inside the Camera View:
  - Hit testing for corners, edges, or center of any zone in edit mode.
  - Context-aware cursors (NWSE, NESW, EW, NS, Move) based on hover position.
  - Smooth drag-resizing and moving of zones, with frame-boundary clamping and minimum size bounds (4x4 px).
- **Styled Overlays & Handles:** Green borders for Detection zones, orange for Exclude zones, and bright/pulsing red borders + alarm badge for active Alarm zones with objects inside. Corner handle boxes are rendered in edit mode.
- **Dev Console Upgrades:** Expanded ROI tab table to 5 columns with inline label edits and a dropdown Combo to choose/modify the zone's function.
- **ROIManager Unit Tests:** 5 new test cases added to GTest suite covering setFunction, updateRect, and detection filtering for Inclusion, Exclusion, and Alarm zones.

## [1.9.1] - 2026-06-05 — HOTFIX
### Fixed
- **CRASH on target lock (SIGSEGV):** `m_zoomRenderer` was declared as a `unique_ptr<VideoRenderer>` in `Application.hpp` but was never initialized in the constructor, leaving it as `nullptr`. Any call to `renderZoomWindow()` (triggered on locking a target) would immediately dereference the null pointer and crash with `EXC_BAD_ACCESS KERN_INVALID_ADDRESS at 0x0`.
  - **Fix:** Added `m_zoomRenderer = std::make_unique<VideoRenderer>()` to the constructor alongside `m_renderer`.
- **Defensive bbox validation in `renderZoomWindow()`:** Added a pre-crop check that validates the locked target's bounding box (non-zero size, fully inside frame dimensions). If the box is degenerate or stale, the function renders an "INVALID TARGET BOUNDS" message instead of attempting an out-of-bounds `cv::Mat` crop.

## [1.9.0] - 2026-06-05
### Fixed
- **Target Locking:** Fixed mouse click targeting inside the Camera View window by replacing `!WantCaptureMouse` with `ImGui::IsWindowHovered()`, allowing targets to be correctly locked by clicking on their bounding boxes.

## [1.9.0] - 2026-06-05
### Added
- **ROI Management (Plan 04):** New `ROIManager` module implementing up to 4 independent rectangular surveillance zones.
- **Drag-to-Draw:** In Camera View, activate Edit Mode in the ROI tab and drag to define zones. Right-click cancels an in-progress drag.
- **Detection Filtering:** Worker thread filters all YOLO detections against active ROI zones. Detections whose centroid falls outside all active zones are discarded before tracking.
- **ROI Overlay:** Active zones rendered as semi-transparent green rectangles with labels directly on the Camera View. Inactive zones shown in gray. Controlled by `showROIOverlay` toggle.
- **ROI Dev Console Tab:** New "ROI" tab with Edit Mode toggle, zone table (ID/Label/Rect/Enable/Delete), "Clear All" button, and zone label editing.
- **Auto-Exit Edit Mode:** Automatically exits edit mode when 4 zones are drawn (maximum capacity reached).

## [1.8.0] - 2026-06-05
### Added
- **Data-Logging (Plan 03):** New `DataLogger` module for automatic, session-based export of all tracking data.
- **CSV Format:** One row per tracked object per frame with fields: `timestamp_ms, track_id, class_name, confidence, x_px, y_px, w_px, h_px, cx_px, cy_px, vx_px, vy_px, x_m, y_m`.
- **JSON-Lines Format:** One JSON object per line for streaming-compatible machine-readable output.
- **Atomic Persistence:** `m_file.flush()` called after every logged frame to guarantee data integrity even on unexpected termination.
- **Configurable Frequency:** Log every N frames (1 = every frame, up to 30 for reduced I/O overhead).
- **Logging Dev Console Tab:** New "Logging" tab with START/STOP button (color-coded), format combo, frequency slider, output directory field, and live session status (file path, row count, KB written).
- **SystemSettings Extensions:** `dataLoggingEnabled`, `dataLoggingFormat`, `dataLoggingFreqFrames`, `dataLoggingOutputDir`, `showROIOverlay` added to `SystemSettings`.

## [1.7.2] - 2026-06-05
### Added
- **Target Zoom Window:** Added a dedicated "Target Zoom" window in ImGui that displays a magnified view of the currently locked target with a target crosshair, tactical corner brackets, and details like track ID, class, confidence, and size. Displays a standby grid and status when no target is locked.
### Fixed
- **Build System Restoration:** Added missing `DataLogger.cpp` and `ROIManager.cpp` to `CMakeLists.txt`'s `CORE_SRCS` to resolve undefined symbol errors.
- **Syntax Error Fix:** Cleaned up a corrupted copy-pasted block with literal `\n` character escape sequences in `Application.cpp`.
- **Include Cinttypes:** Added `#include <cinttypes>` to `Application.cpp` to properly support the `PRIu64` formatting macro.

## [1.7.1] - 2026-06-05
### Added
- **UI Safety Hardening:** Implemented a mandatory "Enable Admin Actions" checkbox for destructive operations (Reset/Quit) to prevent accidental closure from phantom input or glitches.
### Fixed
- **HUD Synchronization:** Restored the `lockedTarget` reference to the `HUD::render` loop and updated the status window to display real-time lock status (LOCKED/LOST/ONLINE).
- **Type Stability:** Corrected `ImVec` type mismatches in the Dev Console.

## [1.7.0] - 2026-06-05

### Fixed
- **UI Interaction Reconstruction:** Fully restored the interactive locking mechanism in the Data Panel. Clicking a row now correctly toggles the target lock.
- **Worker Synchronization:** Fixed a race condition where UI-thread lock requests were being overwritten or ignored by the tracking worker thread.
- **HUD Telemetry Restoration:** Restored the "TARGET" status block in the HUD, providing real-time data for the currently locked track.
- **Visual Highlighting:** Re-implemented distinctive red highlighting for locked targets in both the Camera View and Data Panel.

## [1.7.1] - 2026-06-05
### Added
- **Live Camera Selection UI:** System tab now contains a full camera-source panel with:
  - Status badge showing current active source (green = active, red = failed)
  - Quick-select combo dropdown (Camera 0–5, RTSP, HTTP presets)
  - Manual `InputText` field for any numeric ID, device path (`/dev/video0`), or full network URI (`rtsp://...`, `http://...`)
  - `Apply` button (disabled during pending swap) that triggers a thread-safe hot-swap
  - `Switching...` indicator while the worker thread is reconnecting
- **`CameraModule::close()`:** New public method to safely release the current capture without destroying the object, enabling hot-swap.
### Changed
- Camera hot-swap is fully thread-safe: the UI sets an atomic flag + mutex-protected address; the worker thread performs the actual `close()`/`open()` sequence and writes the result status back.

## [1.6.8] - 2026-06-05
### Changed
- **Extended Target Telemetry:** Added `trail` support to the `TrackedTarget` structure, ensuring movement history is preserved and rendered correctly when a target is locked.
### Fixed
- **Type Mismatch Resolution:** Resolved a build error caused by missing members in the legacy `TrackedTarget` structure during its transition to the `MultiTracker` framework.

## [1.6.7] - 2026-06-05
### Changed
- **Target Locking Architecture:** Completely refactored the target locking mechanism to align with the `MultiTracker` system. The application now locks on stable Track IDs instead of transient raw detections.
- **Worker-Thread State Management:** Integrated atomic lock/release request handling into the worker loop for synchronized tracking state updates.
### Fixed
- **Build Compatibility:** Resolved undeclared identifier and type mismatch errors in `Application.cpp`.

## [1.6.6] - 2026-06-05
### Fixed
- **Linker Error Resolution:** Re-implemented the `Application::log` function to ensure correct symbol resolution and cross-thread diagnostic stability.
- **Improved Console Mirroring:** Diagnostic logs are now mirrored to stdout/stderr for easier debugging during App Bundle execution.

## [1.6.4] - 2026-06-05

### Added
- **Tabbed Dev Console:** Complete overhaul with 5 tabs — System, Detector, Tracker, HUD, Console.
- **Live FPS Graph:** Ring-buffer plot of render frame rate in the System tab (128-sample history).
- **System Metrics:** Two-column metrics table showing Render FPS, Frame Time (ms), Camera FPS, Active Tracks, Total Detections, and Total Frames Processed.
- **Detector Presets:** One-click "Fast", "Balanced", "Precise" preset buttons in the Detector tab.
- **Advanced Tracker Controls:** New sliders for Min Match Score, Max Center Distance, Confirm Frames, and per-object trail fade alpha.
- **HUD Color Pickers:** Full RGBA color pickers for HUD Color and Target Color with hue-wheel picker (ImGuiColorEditFlags_PickerHueWheel).
- **HUD Style Controls:** New sliders for HUD Brightness, Crosshair Scale, and Box Line Width.
- **Visibility Toggles:** Show/hide Track IDs and Confidence labels independently.
- **In-App Console Log:** Scrollable message log with per-entry severity coloring, min-level combo filter, text search/filter, and auto-scroll toggle.
- **Floating Settings Window:** Dedicated "Settings..." overlay window with collapsible sections (Display, HUD Elements, Detector, Tracker, Logging).
- **Pipeline Toggles:** Enable/disable detection and tracking pipelines independently; skip-frame control for the detector.
- **Grayscale Input Mode:** Option to convert frames to grayscale before detection for a speed boost.
- **Reset All Settings:** Single-click reset to default SystemSettings.
- **Thread-safe Logging:** `Application::log()` method usable from any thread; entries timestamped relative to app start.
- **Enhanced Data Panel:** Added State column (LOCK / LOST / INIT), color-coded confidence column (green/amber/red), render FPS overlay in header.
- **Fading Trail Alpha:** Trail segments now fade from opaque (recent) to transparent (oldest) when enabled.
- **Status Window Accent Borders:** HUD status windows now have a thin accent-colored border.
- **Major/Minor Tick Marks:** Tactical overlay grid now uses larger ticks every 5th increment.
### Changed
- `HUD::render()` resolves color from `SystemSettings` at runtime, making color picker changes instant without restart.
- Worker thread now respects `enableDetection`, `enableTracking`, `detectionSkipFrames`, and `grayscaleInput` settings.
- `SystemSettings` significantly expanded with new fields for tracker tuning, HUD styling, and logging.
- Application split into `renderCameraView()`, `renderDataPanel()`, `renderDevConsole()`, `renderSettingsWindow()` helpers for maintainability.

## [1.6.5] - 2026-06-05
### Added
- **Interactive Data Panel:** Clicking a row in the Data Panel now locks or unlocks the corresponding object.
- **Persistent Target Locking:** Re-implemented the locking mechanism to use stable Track IDs. The system now maintains a lock even if the object is briefly occluded.
- **Enhanced HUD Feedback:** Locked targets are now highlighted in red with thicker lines and persistent telemetry blocks.
### Fixed
- **Locking Logic Bug:** Resolved an issue where the locking request was never processed by the background worker thread.
- **Right-Click Release:** Re-implemented the right-click gesture to release the current target lock.

## [1.6.4] - 2026-06-05
### Added
- **Verbose Diagnostic Logging:** Added console output for each initialization stage (Camera, Models, GLFW, ImGui) to simplify crash analysis.
### Changed
- **Default Camera Selection:** Updated default camera ID from 1 to 0 for better compatibility with single-camera systems.
### Fixed
- **Initialization Stability:** Improved error reporting during application startup.

## [1.6.3] - 2026-06-05
### Fixed
- **Critical Resource Loading Fix:** Re-implemented the resource path resolution logic that was lost during previous refactorings. The application now correctly identifies model paths within the macOS App Bundle structure and supports local development fallbacks.

## [1.6.2] - 2026-06-05

+### Added
+- **UI Restoration:** Re-implemented ImGui Docking and the "Dashboard" layout.
+- **Dedicated Windows:** Restored "Camera View" and added a new "Data Panel" for real-time object telemetry.
+### Fixed
+- **Camera Config:** Set default camera ID to 1 as requested by the user.
+- **Architecture Regression:** Resolved the regression where the camera feed was rendered as a background instead of a windowed component.
+
 ## [1.5.3] - 2026-06-05
### Fixed
- **ImGui API Compatibility:** Fixed a build error where `DockSpaceOverViewport` was called with incorrect parameters. Corrected the signature to match the Dear ImGui `docking` branch.

## [1.6.1] - 2026-06-05

### Added
- **Independent Camera FPS telemetry:** Measures actual processing performance (FPS), decoupled from the main UI framerate.
- **Architectural Refactoring:** Organized source files into logical subdirectories (`core`, `vision`, `tracking`, `ui`) for improved maintainability and "Military-Grade" clarity.
- **Resource Management:** Moved AI models and labels to a dedicated `assets/models` directory.
### Changed
- **HUD isolation:** Implemented strict clipping using `ImDrawList` primitives, ensuring all overlays (crosshairs, brackets, trails) are locked to the video viewport.
- **Refactored Status Windows:** Integrated SysLog and Data windows into viewport overlays, removing global ImGui windows and reducing overhead.
- **CMake Modernization:** Updated `CMakeLists.txt` to support the new modular directory structure and modular include paths.
### Fixed
- **Telemetry interference:** Resolved issues where tactical overlays conflicted with or were obscured by global UI elements.

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
