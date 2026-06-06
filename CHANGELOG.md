# Changelog

## [1.13.0] - 2026-06-06

### Added
- **High-Sensitivity Tracking Re-acquisition (Plan 11)**: Massive Verbesserung der Zielverfolgung bei schnellen Bewegungen oder kurzzeitigem Signalverlust.
- **ROI-Search-Mode**: Wenn ein Ziel verloren geht, wird automatisch ein lokaler Hochauflösungs-Scan (Zoom) um die vorhergesagte Position durchgeführt, um die Detektions-Sensitivity drastisch zu erhöhen.
- **Motion-Guided Fallback**: Nutze Roh-Bewegungsdaten als temporäre Stütze, falls die Objekterkennung (YOLO) ein Ziel kurzzeitig nicht identifizieren kann.
- **Adaptive Matching Distances**: Dynamische Erhöhung des Suchradius für "Lost"-Tracks, um Re-acquisition über größere Distanzen zu ermöglichen.
- **HUD Integration**: Neuer Bereich "Tracking Recovery" im Settings-Fenster zur Steuerung von Scan-Zoom und Fallback-Strategien.

## [1.12.7] - 2026-06-06

### Fixed
- **Target Analyzer History Bug**: Ein kritischer Synchronisationsfehler wurde behoben, bei dem der Target Analyzer keine Objektdaten anzeigte. Die Ursache war eine fehlerhafte Zuweisung, bei der das Austausch-Objekt `m_sharedTargetHistory` kontinuierlich mit der veralteten UI-Kopie `m_targetHistory` überschrieben wurde, anstatt die aktualisierten Daten aus `m_internalTargetHistory` (Tracking-Thread) zu übernehmen.

## [1.12.6] - 2026-06-06

### Fixed
- **Objekt-Markierung korrigiert**: Fehler behoben, bei dem keine Objekte im HUD markiert wurden. Die Ursache war eine unvollständige Migration der Tracking-Logik in einen separaten Thread, der niemals gestartet oder getriggert wurde.

### Changed
- **Pipeline-Refactoring**: Die Tracking-Pipeline (inkl. Multi-Tracking, Pixel-Locking, Alarm-Zonen-Prüfung und Daten-Logging) wurde vollständig in den dedizierten `trackingWorkerLoop` verschoben.
- `Application.cpp`: Tracking-Thread wird nun in `init()` korrekt gestartet und über `m_trackingCv` synchron getriggert. Redundante Tracking-Logik aus dem `workerLoop` entfernt.
- `Application.hpp`: Erweiterung der Synchronisations-Struktur um `m_trackingSessionMs` und `m_trackingMotionTracksCopy`.

## [1.12.5] - 2026-06-06

### Added
- **Erweiterte Heatmap-Einstellungen**: Einführung von Sensitivitäts- und Transparenz-Reglern für das optische Fluss-Heatmap-Overlay.
- **Heatmap-Sensitivität**: Neuer Parameter `motionHeatmapSensitivity` zur Feinjustierung der Intensitäts-Normalisierung. Ermöglicht die Erkennung kleinster Bewegungen durch Senkung des Schwellenwerts.
- **Heatmap-Transparenz**: Neuer Parameter `motionHeatmapAlpha` für eine globale Deckkraft-Steuerung des Overlays, unabhängig von der Bewegungsintensität.
- **Persistente Konfiguration**: Vollständige Integration der neuen Parameter in die Speicher/Lade-Logik (`config.ini`) und das HUD-Einstellungsmenü.

### Changed
- `MotionDetector.cpp`: Die Heatmap-Berechnung nutzt nun dynamische Parameter anstelle von Hardcoded-Werten für Normalisierung und Alpha-Blending.
- `Application.cpp`: HUD-UI um Slidder für Heatmap-Sensitivität und Alpha erweitert (unter "Motion Detection > Heatmap").

## [1.12.4] - 2026-06-06

### Added
- **Sekunden-Intervall-Snapshots**: Während ein Ziel aktiv getrackt wird, wird jede Sekunde automatisch ein Snapshot erfasst und im `periodic_snapshots`-Buffer gespeichert. Dies stellt sicher, dass stets der optimale Moment für jedes der drei Keyframes verfügbar ist.
- **Verbessertes Keyframe-Auswahlverfahren**: Beim Verlust eines Ziels wird aus dem Buffer automatisch das beste Discovery (erster Snap), Mid-Track (mittlerer Index) und Last-Seen (letzter Snap) Bild ausgewählt — keine manuellen oder zufälligen Aufnahmen mehr.
- **Galerie-Navigation im Target Analyzer**: Interaktiver Vor/Zurück-Durchklick durch alle drei Keyframe-Meilensteine. Standard-Ansicht zeigt "Mid-Track (Best)" zuerst.
- **Bild-Metadaten pro Milestone**: Jeder Keyframe zeigt Aufnahmezeitstempel, Begrenzungsrahmen (X/Y/W/H), Pixel-Fläche und Detektionswahrscheinlichkeit inkl. farbigem Fortschrittsbalken.
- **Speicher-Optimierung**: Nach Finalisierung eines Ziels wird der `periodic_snapshots`-Buffer geleert und freigegeben — nur die 3 finalen Keyframes bleiben im Speicher.

### Changed
- `Common.hpp`: `TargetSnapshot`-Struktur (mit `image`, `timestamp`, `box`, `confidence`) ersetzt die alten `cv::Mat`-Felder. `UniqueTargetRecord` enthält nun `periodic_snapshots`-Buffer und `last_snapshot_time`.
- `Application.cpp`: `updateTargetHistory` implementiert vollständige 1-Sekunden-Sampling-Logik und automatische Keyframe-Auswahl bei Target-Verlust. `renderTargetAnalyzer` vollständig überarbeitet mit Tab-Navigation und Metadaten-Anzeige.

## [1.12.3] - 2026-06-06


### Added
- **Dediziertes Export-Verzeichnis**: Einführung der Einstellung `exportOutputDir` (Standard: `exports/`), um manuelle Exporte (Screenshots, Target-Details, Historie) sauber von den kontinuierlichen Log-Dateien zu trennen.
- **Automatisierte Ordnererstellung**: Das Export-Verzeichnis wird nun bei Bedarf automatisch erstellt (`std::filesystem::create_directories`), was den Workflow beschleunigt und Fehler durch fehlende Zielordner eliminiert.
- **Erweiterte UI-Konfiguration**: Das "Export & Logging" Panel im Data Panel wurde um ein Eingabefeld für das Export-Verzeichnis erweitert.

### Changed
- `Application.cpp`: `exportTarget`, `exportTargetHistory` und `takeScreenshot` nutzen nun das dedizierte Export-Verzeichnis.
- Menü "Tools": "Open Data Directory" wurde zu "Open Export Directory" aktualisiert.

## [1.12.2] - 2026-06-06

### Added
- **Global Screenshot Functionality**: Added "Take Screenshot" to a new "Tools" menu in the Main Menu Bar and as a button in the "Export & Logging" tab. Uses `glReadPixels` to capture the entire application window including all ImGui overlays.
- **Full History Export**: Added "Export Entire History (JSON)" to save all tracked targets from the session to a single timestamped JSON file.
- **New "Tools" and "View" Menus**: Expanded the Main Menu Bar with dedicated menus for file operations, window visibility, and analysis tools.
- **Export & Logging Tab**: Moved and enhanced the data logging controls into the "Data Panel" for better accessibility as per the UI design specification.
- **Open Data Directory**: Added a shortcut to open the current output directory in the system file explorer (supports Windows, macOS, and Linux).
- **Keyboard Shortcuts**:
  - `Space`: Toggle Data Logging Start/Stop.
  - `PrintScreen`: Take a global screenshot.
  - `C`: Toggle Settings Window.

### Changed
- `Application.hpp`: Added methods for screenshot and history export, and atomic flags for UI state.
- `Application.cpp`: Implemented `exportTargetHistory`, `takeScreenshot`, and restructured the Main Menu Bar and Data Panel. Removed redundant logging controls from the Settings window.

## [1.12.1] - 2026-06-06

### Added

- **Interaktive Galerie im Target Analyzer**: Der Target Analyzer zeigt nun standardmäßig das beste Bild (Mid-Track / Best) zuerst. Der Benutzer kann über die Schaltflächen "< Prev", "Next >" oder Tabs ("Discovery", "Mid-Track (Best)", "Last Seen") durch die drei Meilensteine navigieren.
- **Milestone Snapshot Metadaten**: Jedes Bild in der Galerie wird nun mit seinen spezifischen Metadaten angezeigt: Aufnahmezeitstempel, Begrenzungsrahmen (X, Y, Breite, Höhe), Pixel-Fläche und Detektionskonfidenz (inklusive visueller High-Contrast Fortschrittsbalken).

### Changed

- `Common.hpp`: Die drei Meilensteine im `UniqueTargetRecord` wurden auf `TargetSnapshot` Strukturen umgestellt, um die Metadaten direkt beim Bild zu speichern.
- `Application.cpp`:
  - `renderTargetAnalyzer` komplett überarbeitet für die interaktive Galerie und Metadaten-Anzeige.
  - Snapshotting-Logik in `updateTargetHistory` und Bild-Export in `exportTarget` angepasst.
- `tests/unit_tests.cpp`: Unit-Tests auf die neue Struktur angepasst.

## [1.12.0] - 2026-06-06

### Added

- **Motion Heatmap (Optical Flow)**: Implementierung eines Heatmap-Overlays, das Bewegungsgeschwindigkeit visualisiert. Nutzt OpenCV Dense Optical Flow (`calcOpticalFlowFarneback`) auf einer skalierten Maske für Echtzeit-Performance.
- **Geschwindigkeitsabhängige Farbgebung**: Je schneller die Bewegung, desto roter das Signal (Mapping via `COLORMAP_JET`). Bereiche ohne Bewegung bleiben transparent.
- **Heatmap-Akkumulation & Decay**: Bewegungen hinterlassen Spuren, die mit einer einstellbaren Decay-Rate (`motionHeatmapDecay`) verblassen.
- **Echtzeit-Optimierung**: Berechnung des Optical Flows auf 1/4 der Originalauflösung zur Einhaltung der Military-Grade Performance-Vorgaben.
- **HUD-Integration**: Nahtlose Überlagerung des Heatmap-Textur-Layers über den Haupt-Kamerastream im HUD.
- **Konfigurations-Panel**: Neue Steuerelemente im Motion-Settings-Bereich für Heatmap-Toggle und Decay-Rate (Persistence).

### Changed

- `VideoRenderer`: Unterstützung für 4-Kanal-Texturen (RGBA/BGRA) für transparente Overlays hinzugefügt.
- `Common.hpp`: `SystemSettings` um `motionHeatmapOverlay` und `motionHeatmapDecay` erweitert.
- `Application.hpp/cpp`: Pipeline-Integration, Texture-Sync (Worker → Render Thread) und Einstellungs-Persistenz implementiert.

## [1.12.0] - 2026-06-06

### Added

- **Drei-Punkt-Chronologie im Target Analyzer**: Der Target Analyzer zeigt nun eine visuelle Chronologie mit mindestens 3 Schlüsselbildern (Entdeckung, Mittendrin, Letzte Sichtung) statt nur eines einzelnen Thumbnails.
- **Periodische Snapshots**: Während der aktiven Verfolgung wird jede Sekunde ein hochauflösender Bildausschnitt des Objekts erfasst und im Hintergrund gepuffert. Der Puffer wird dynamisch zur Aktualisierung des Mittendrin- und Letzte-Sichtung-Bildes ausgewertet.
- **Speicheroptimierung**: Bei Verlust oder Archivierung des Targets werden alle zwischenzeitlich gepufferten periodischen Snapshots aus dem Arbeitsspeicher freigegeben, sodass nur die drei Schlüsselbilder dauerhaft im Speicher bleiben.
- **Drei-Wege Bild-Export**: Der Target-Export speichert nun alle drei Schlüsselbilder separat (`target_XXX_discovery.png`, `target_XXX_midtrack.png`, `target_XXX_lastseen.png`) zusammen mit dem JSON-Report.
- **Unit Testing**: Neuer Test `TargetHistoryTest.VisualChronologySnapshottingAndFinalizing` zur Validierung des periodischen Snapshot-Puffers und der Speicherbereinigung.

### Changed

- `Common.hpp`: `UniqueTargetRecord` um Strukturen für Visual Chronology erweitert (`cropped_image_first`, `cropped_image_mid`, `cropped_image_last`, `periodic_snapshots`, etc.).
- `Application.hpp`: `TextureInfo` für drei GPU-Texturen und deren Cache-Versionierung angepasst.
- `Application.cpp`: Texture-Upload-Loop, Target-Historien-Update, Details-Export, Target Analyzer UI und Aufräumfunktionen für das Drei-Texturen-System implementiert.

## [1.11.12] - 2026-06-06

### Added

- **Revamped Acoustic Feedback System**: Overhauled all five feedback channels with procedurally generated waves instead of flat pure sines:
  - **Motion Alert**: Chirping sonar radar ping (downward frequency sweep with high second-harmonic content and exponential decay).
  - **Alarm Zone Entry**: Urgent pulsing siren using base + tritone frequencies ($1.414 \times f_0$) with 20 Hz amplitude tremolo.
  - **Alarm Zone Exit**: clearance downward sweep with odd harmonics and linear decay.
  - **Target Lock Acquired**: futuristic lock-acquired confirm double-pip ("bip-BIP").
  - **Target Lock Lost**: descending buzzing odd-harmonic square-like telemetry loss sound.
- **Trigger Optimization**: Decoupled motion alert trigger from sub-zooms enabling. Pings are now played only when a *new* moving entity (track) is registered by the background motion detector, avoiding continuous beep loops.
- **Stability Fixes & Verification**: Fixed potential out-of-bounds array access in sub-zoom initialization when adding motion objects, and added test coverage for `AudioEngine` in the automated test suite.

## [1.11.11] - 2026-06-06

### Added

- **Dynamic Subzoom Placement**: Subzoom PiP overlays now dynamically slide along the borders of the viewport to avoid overlapping with tracked objects, the locked target, and HUD elements.
- **Closest-Border Leader Lines**: Leader lines now attach to the closest border point of the subzoom inset instead of its center, making tracking visualization much cleaner.
- **Outline Target Markers**: Target markers for subzooms now consist of an outline box and circle (no filled regions) at the target location.
- **Outline Locked Target Reticle**: Added a matching outline circle to the locked target bounding box, providing a clean, military-grade target locking reticle.

## [1.11.10] - 2026-06-06

### Added

- **Military-Grade Tactical Audio revamps**: revamping procedural wave synthesis for all five feedback channels inside `AudioEngine`:
  - **Sonar Radar Ping (Motion Alert)**: Downward frequency sweep (chirp) with high-pitched second harmonics and an exponential decay envelope.
  - **Threat warning (Alarm Zone Entry)**: Pulsing dual-frequency warning using tritone harmonics ($1.414 \times f_0$) and 20 Hz amplitude tremolo.
  - **Clearance chime (Alarm Zone Exit)**: Descending sweep with odd harmonics and linear decay.
  - **Lock confirmation (Target Lock Acquired)**: Futuristic double-pip ("bip-BIP") with an upward pitch sweep.
  - **Telemetry drop (Target Lock Lost)**: descending rough odd-harmonic square-like buzzer.
- **Improved Motion Alert trigger logic**: The motion regions tracker `m_workerMotionTracks` now runs whenever motion detection is enabled (even if sub-zooms are disabled). Pings are only triggered when a *new* moving track is spawned rather than continuously beeping on active movement.
- **Sub-zooms bug fix**: Fixed a potential out-of-bounds array access in `Application::workerLoop` by keeping the `trackMatched` vector sized in sync with `m_workerMotionTracks` when new motion regions are appended.

## [1.11.9] - 2026-06-06

### Added

- **Präziseres lokales Vision-Modell**: `assets/models/yolov8s.onnx` ergänzt das bisherige `yolov8n`-Modell und wird beim Start bevorzugt geladen.
- **Umfangreiche Unit-Tests für Latenzkompensation**: Neuer Testfall `Kalman6DStateAndLagCompensation` in `unit_tests.cpp` prüft die Positions-Extrapolation unter simulierter Detektor-Latenz und die Dimensionsstabilität der Boxen.

### Changed

- **Kalman-Filter auf 6D umgestellt**: Das Zustandsmodell des MultiTrackers wurde von 8D auf 6D (`[cx, cy, w, h, vx, vy]`) reduziert. Die Modellierung der Größen-Änderungsgeschwindigkeit (`vw`, `vh`) wurde entfernt. Die Breite und Höhe des Track-Objekts werden nun als stabilere Random Walks ohne Trägheitsmoment geschätzt, was das erratische Schrumpfen (extrem dünne Boxen) oder Überwuchern der Boxen bei Signalverlust/Rauschen vollständig behebt.
- **Asynchrone Latenz-Kompensation (Lag Compensation)**: Die Verzögerung des asynchronen Detektor-Threads wird nun in Frames gemessen. Bei Ankunft neuer YOLO-Ergebnisse interpoliert der Tracker die Detektionsboxen anhand der geschätzten Track-Geschwindigkeit vorwärts, wodurch die Bounding Boxen in Echtzeit dem aktuellen Bild folgen und das Hinterherhinken ("Lag") sowie Rückwärts-Ruckeln eliminiert wird.
- **Fehlende Zustands-Kopierung im Dead-Reckoning korrigiert**: Falls ein Track in einem Frame nicht zugeordnet wird, wird das System-Dead-Reckoning durch explizite Synchronisation von `statePre` in `statePost` fortgesetzt.
- **Unvollständige target-analyser Header-Änderungen aufgeräumt**: Die unvollständig implementierten TextureInfo/Milestone-Änderungen in den Headern (Common/Application) wurden zurückgesetzt, um ein fehlerfreies Kompilieren der Target-Details zu gewährleisten.
- **Detektions-Defaults verschärft**: Confidence-, Score- und NMS-Schwellenwerte wurden angehoben bzw. gestrafft, um Fehlalarme zu reduzieren.
- **Bundle-Ressourcen aktualisiert**: Das macOS-Bundle enthält jetzt sowohl `yolov8s.onnx` als auch das bisherige Fallback-Modell.
- **YOLOv8-Parser korrigiert**: Der Detector richtet sich jetzt an der echten Modell-Outputbreite aus statt an der längeren Label-Liste, damit Konfidenzen und Boxen korrekt interpretiert werden.

## [1.11.8] - 2026-06-06

### Added

- **Manual Crop Capture**: Added an **Update Visual Crop (Manual)** button to the Target Analyzer panel (active when selected targets are in-view) that captures the current frame's crop as the target representation.
- **Dynamic Crop Updates**: Enabled automatic crop updates when a target is observed with a larger bounding box (higher resolution) and decent confidence (`>= 0.4`), ensuring close-up shots are preferred.

- **Surveillance Classes**: Added several surveillance-oriented class labels to the model labels file (`assets/models/coco.txt`) to improve detection coverage for monitoring scenarios (examples: `helmet`, `traffic cone`, `license plate`, `stroller`, `wheelchair`, `scooter`, `electric scooter`, `trash can`, `traffic sign`, `unattended bag`, `suspicious bag`, `face`).

### Changed

- `Common.hpp`: Added version tracking to target records.
- `Application.hpp`: Added version tracking to target textures, and an atomic manual capture request field.
- `Application.cpp`:
  - Updated `updateTargetHistory` to process manual capture requests and larger bounding box area checks, incrementing crop versions on changes.
  - Updated OpenGL texture binding checks to verify crop versions, ensuring immediate texture updates on the render thread.
  - Changed the Motion Detection `Min. Area` control to a logarithmic slider so very small regions can be tuned more precisely.
  - Added a configurable `Sub Zoom Magnification` control and applied it to sub-zoom crops.

## [1.11.7] - 2026-06-06

### Added

- **Multiple Sub Zooms for Motion Tracking**: Added up to 4 automatic sub zooms that crop and track detected movements.
  - **Dynamic Overlay mode**: Shows sub zooms as PiP inserts on the corners/margins of the main camera view with leader lines pointing to the movements.
  - **Separate Windows mode**: Shows sub zooms in 4 separate dockable/floating ImGui windows, toggled via settings.
  - **2-Second Hold Time**: Preserves the crop and display of the sub zooms for 2.0 seconds after the movement disappears, with visual indicators (e.g. orange dashed leader lines and "HOLD" tags) when holding.
  - **Independent Motion Logging**: Logs motion events separately (under class name `"Motion"` and track ID offset by 10000) unless they overlap with existing tracked targets (overlap ratio > 0.2).
  - **Persistence & Presets**: Configured `settings.ini` to persist sub zoom settings across app launches.

## [1.11.5] - 2026-06-06

### Added

- **Chronological Pixel Target IDs**: Changed the pixel target locking mechanism (establishing a lock on empty space) to assign sequential chronological track IDs from the tracker's ID pool instead of a static `999` ID.
- **Pixel Target Data Logging**: Enabled logging for pixel targets in `DataLogger`. They are now saved in the CSV/JSON records under their assigned chronological IDs.

### Changed

- `MultiTracker`: Added `getNextIdAndIncrement()` helper method to retrieve and increment the tracker's internal ID counter.
- `Application`:
  - Updated vision thread lock requests to assign and maintain chronological IDs for pixel targets.
  - Refactored ROI Editor hit-testing and HUD drawing to identify pixel targets by checking `className == "Pixel Target"` instead of hardcoded `track_id == 999`, avoiding conflicts with standard ROI zones.
  - Formatted the HUD pixel target label to display the actual chronological ID.

## [1.11.5] - 2026-06-06

### Added

- **Asynchronous Detection Thread**: Decoupled object detection from the main real-time tracking thread, allowing the camera reader and UI rendering to run at the source's native 30 FPS without stutter or lag.
- **Remote GPU Inference Server**: Added `scripts/server.py` containing a lightweight Python FastAPI server using PyTorch + CUDA to run YOLOv8m (Medium) on the RTX 3050.
- **Remote GPU Inference Client**: Integrated a `libcurl`-based network inference client in `ObjectDetector` to compress, upload, and map object detections over LAN.
- **Remote Inference UI Controls**: Added Server IP, Port, and Activation controls to the "Detection & Tracking" tab in the Settings window.
- **Resilient Network Dead-Reckoning**: Configured the MultiTracker to persist tracks for up to 90 frames (approx 3 seconds) without updates when remote inference is active, letting Kalman prediction bridge network jitter smoothly.
- **JSON Parsing Tests**: Added unit tests in `tests/unit_tests.cpp` to verify parsing of remote JSON detection arrays.

## [1.11.4] - 2026-06-06

### Added

- **Target Zoom Release Control**: Added a highly visible, styled "UNLOCK TARGET / RELEASE LOCK" button at the top of the "Target Zoom" window whenever a target is locked, enabling direct target release.
- **Tabbed Settings Window**: Restructured the Settings window layout into 5 separate tabs for clean organization: Display & HUD, Camera & Zoom, Detection & Tracking, Audio Alerts, and System & Admin.

### Changed

- `Application.cpp`: Redesigned `renderZoomWindow` and `renderSettingsWindow` for tabs and Zoom Window unlock actions.

## [1.11.3] - 2026-06-06

### Added

- **Audio Feedback System**: Neues `AudioEngine`-Modul (`src/core/AudioEngine.hpp/.cpp`) implementiert akkustisches Echtzeit-Feedback für taktische Ereignisse via macOS `AudioToolbox` (`AudioServicesPlaySystemSound`). Töne werden als synthetisierte Sinus-PCM-Bursts (AIFF-Format, 44100 Hz, 16-Bit mono) im Speicher erzeugt — keine externen Audiodateien erforderlich.
- **Fünf konfigurierbare Alarm-Kanäle**:
  - **Motion Alert** (880 Hz / 80 ms): Beep bei erkannter Bewegung (Plan 10 Motion Detection), gedrosselt durch einstellbaren Cooldown (~1 s Standard).
  - **Alarm Zone Entry** (1200 Hz / 120 ms): Scharfer Ton beim Eintreten eines Tracks in eine Alarm-Zone.
  - **Alarm Zone Exit** (440 Hz / 80 ms): Tiefer Ton beim Verlassen einer Alarm-Zone.
  - **Target Lock Acquired** (1000 Hz / 150 ms): Bestätigungston beim Einrasten des Target Locks (SEARCHING/LOST → LOCKED Transition).
  - **Target Lock Lost** (300 Hz / 200 ms): Warn-Ton beim Verlust des Target Locks (LOCKED → LOST Transition).
- **Zero-Heap Hot Path**: PCM-Synthese und AIFF-Registrierung erfolgt ausschließlich bei `init()`/`applyConfig()`, nie im Worker-Loop. `AudioServicesPlaySystemSound()` ist non-blocking und verursacht keine Heap-Allokation.
- **Settings-Panel „Audio Feedback"**: Neuer `CollapsingHeader` im Settings-Fenster mit:
  - Master-Enable-Toggle und Volume-Slider
  - Pro Kanal: Enable-Checkbox, Frequenz-Slider (100–4000 Hz), Dauer-Slider (20–500 ms)
  - Motion-Cooldown-Slider (0.1–10 s)
  - `TEST`-Button pro Kanal für sofortiges Vorhören
  - `TEST ALL`-Button spielt den Motion-Ton zur Systemverifikation
- **Persistenz**: Alle 18 Audio-Parameter werden in `~/.tactileviewer/settings.ini` gespeichert und beim nächsten Start automatisch geladen.
- **Live-Reload**: Parameteränderungen im Settings-Panel lösen sofort `applyConfig()` aus und re-synthetisieren die Sounds — kein Neustart erforderlich.

### Changed

- `Common.hpp`: `SystemSettings` um 18 neue Audio-Felder erweitert.
- `Application.hpp`: `AudioEngine m_audioEngine` Member, `m_prevLockState` für Lock-Transitionen.
- `CMakeLists.txt`: `AudioEngine.cpp` zu `CORE_SRCS` hinzugefügt; `-framework AudioToolbox` und `-framework CoreFoundation` für beide Targets gelinkt.

## [1.11.2] - 2026-06-06


### Added

- **Rigorose Validierung für Pixeltarget-Tracking**: Zwei neue Unit-Tests (`SubPixelInterpolationMath` und `SearchWindowClampingShift`) in `unit_tests.cpp` prüfen die mathematische Korrektheit der Peak-Fitting-Gleichungen und die randgesteuerte Suchfensterverschiebung.

### Fixed

- **Aktives Dead-Reckoning (Coast Phase)**: Bei Signalverlust des Pixeltargets (Template-Match-Konfidenz <= 0.5) wird die Position nun aktiv entlang des Geschwindigkeitsvektors vorhergesagt und die Ziel-Box aktualisiert, statt an der letzten Position einzufrieren.
- **Sub-Pixel-Präzision & Interpolation**: Das Tracking speichert nun die subpixelgenauen Fließkomma-Koordinaten (`m_pixelCenterX`/`m_pixelCenterY`). Mittels parabolischer Peak-Interpolation in der Korrelationsmatrix wird die Position mit Sub-Pixel-Genauigkeit bestimmt und geglättet, um Rundungsfehler bei kleinen Geschwindigkeiten zu eliminieren.
- **Randanpassung des Suchfensters**: Das Suchfenster (`searchRect`) wird an den Bildrändern nun verschoben (geklemmt) statt abgeschnitten, um ein Schrumpfen unter die Template-Größe zu verhindern.
- **Synchronisation mit Tracker-Settings**: Die Geschwindigkeitsglättung und -dämpfung des Pixeltargets greift nun dynamisch auf die in den Systemeinstellungen hinterlegten Werte (`trackerVelocitySmoothing`/`trackerDeadReckoningDamping`) zu, anstatt hartcodierte Werte zu nutzen.

## [1.11.1] - 2026-06-06

### Added

- **Unique Target Memory Persistence**: Added memory persistence for tracking history. Tracked objects remain in memory as a `UniqueTargetRecord` after their active tracks are lost/deleted.
- **Target Analyzer UI Panel**: Added a dedicated, dockable ImGui panel ("Target Analyzer") displaying detailed reports on selected targets: cropped thumbnail image, classification, status (Active vs. Lost), max confidence, lifespan timestamps, and initial/final coordinates with bounding box dimensions.
- **Target Export Functions**: Added context menu options (via right-click in Active Tracks and Target History tables) and a detail button in the Target Analyzer to export target data. Details are exported to a JSON file (`target_<ID>_details.json`) and the highest-confidence crop is saved to a PNG image (`target_<ID>_visual.png`).
- **Tabbed Data Panel**: Refactored the Data Panel to support a tabbed interface with "Active Tracks" and "Target History" tabs. Both tabs support sorting, filtering, and row selection.

### Changed

- `Common.hpp`: Defined the `UniqueTargetRecord` struct.
- `Application.hpp`: Added target history vectors, texture lookup maps, and helper methods.
- `Application.cpp`: Integrated history updates in `workerLoop`, texture management on the render thread, and refactored the Data Panel with tabbed controls, context menus, and the Target Analyzer UI.

## [1.11.0] - 2026-06-06

### Added

- **Motion Detection (Plan 10)**: Neues eigenständiges `MotionDetector`-Modul auf Basis von OpenCV MOG2 (Gaussian Mixture Background Subtraction). Erkennt Bewegung vollständig unabhängig vom YOLO-Detektor und Tracking-System — Personen und Objekte werden bereits erkannt bevor sie ein Tracking-Target besitzen.
- **Mindestflächen-Filter**: Konturen unter der konfigurierbaren Schwelle (`motionMinArea`, Standard 50 px) werden automatisch verworfen. Schützt vor Rausch-Triggern.
- **HUD Motion Overlay (Option C)**: Erkannte Bewegungsregionen werden im Camera View als semi-transparente Fill-Fläche + Kontur-Outline eingezeichnet. Z-Order unterhalb der Tracking-Bounding-Boxes.
- **Vollständig konfigurierbar**: Neues Settings-Panel mit 9 Parametern (Sensitivity, Min Area, Blur Kernel, Overlay Alpha, Farbe, Schatten-Erkennung, Learning Rate, Overlay-Toggle, Reset-Button).
- **Persistenz**: Alle Motion-Detection-Settings werden beim App-Start geladen und bei Änderungen automatisch in `~/.tactileviewer/settings.ini` gespeichert.
- **Kamera-Hotswap-Reset**: Bei Kamerawechsel wird das Hintergrundmodell automatisch zurückgesetzt, um Phantomdetektionen durch den veralteten Hintergrund zu vermeiden.
- **Plan-Dokument**: Neue Spezifikation unter `docs/plans/10_motion_detection.md`.

### Changed

- `Common.hpp`: `SystemSettings` um 10 neue Motion-Detection-Felder erweitert.
- `Application.hpp`: `MotionDetector`-Member, `m_sharedMotionRegions`-Shared-State, `m_motionOverlayColorF` Color-Array.
- `HUD.hpp`/`HUD.cpp`: Neue öffentliche Methode `drawMotionOverlay()` + `m_motionColor`-Initialisierung.
- `CMakeLists.txt`: `MotionDetector.cpp` zu `CORE_SRCS` hinzugefügt.

## [1.10.16] - 2026-06-06

### Fixed

- **Feedback wird jetzt zuverlässig gespeichert**: Der Speicherpfad wird robust auf den Projektordner aufgelöst, der Zielordner wird bei Bedarf angelegt und der Feedback-Text wird JSON-sicher escaped.

### Changed

- **Feedback-Formular verbessert**: Die Maske erklärt jetzt klarer, was eingetragen werden soll, bietet mehr Platz für längere Rückmeldungen und deaktiviert das Senden bei leerem Text.

## [1.10.15] - 2026-06-05

### Fixed

- **Kamerabild bleibt jetzt in nativer Auflösung**: Der Worker skaliert das Livebild nicht mehr hart auf 1280x720 herunter. Die Anzeige und das Tracking nutzen jetzt das Originalbild der Kamera, wodurch Streckung bei abweichendem Seitenverhältnis verschwindet und keine Auflösung mehr verschenkt wird.

## [1.10.14] - 2026-06-05

### Added

- **Persistente Einstellungen**: Die wichtigsten App-Settings werden jetzt beim Start aus `~/.tactileviewer/settings.ini` geladen und bei Änderungen automatisch gespeichert.
- **Menüleisten-Presets**: Eine neue `Settings`-Menüleiste bietet jetzt `Standard` sowie die Presets `Performance`, `Balanced`, `Precision` und `Low Light`.
- **Schneller Reset auf Standard**: Der Standardzustand kann jetzt direkt aus der Menüleiste oder über den Reset in den Settings zurückgesetzt werden.

## [1.10.13] - 2026-06-05

### Changed

- **Dev Console trennt jetzt Capture, Tracking und Zoom**: Die System-Metrik zeigt nun explizit die Capture-Auflösung der Kamera, die Tracking-Auflösung des HD-Pipelineschritts und die aktuelle Zoom-Auflösung des aktiven Ausschnitts.
- **Zoom-Auflösung an 4K-Status gekoppelt**: Die Dev Console zeigt jetzt zusätzlich die Zoom-Quelle getrennt vom Zoom-Crop; bei deaktiviertem 4K-Zoom fällt die Quelle auf die Tracking-Auflösung zurück.

## [1.10.12] - 2026-06-05

### Changed

- **Target Zoom Default auf neutral gesetzt**: `targetZoomMagnification` startet jetzt bei `1.0f`, sodass der Target-Zoom standardmäßig ohne zusätzliche digitale Vergrößerung arbeitet.
- **Pixel-Target-Crop konsistent editierbar**: Der Rechteck-Ausschnitt des Pixel Targets wird jetzt als eigene, synchronisierte Rect-Quelle behandelt und beim Drag/Resize wie eine ROI-Zone angepasst, bevor daraus der Template-Crop aktualisiert wird.

## [1.10.11] - 2026-06-05
### Added
- **Echte Zoom-Verstärkung für Target Zoom**: Ergänzt um `targetZoomMagnification` (Standard `1.8x`). Der Zoom-Ausschnitt wird nun um das Zielzentrum verkleinert berechnet, sodass bei aktivem 4K-Zoom ein deutlich stärkerer visueller Zoom entsteht statt nur gleicher Bildausschnitt in höherer Auflösung.
- **Neue UI-Regler für Zoom-Faktor**: In Dev Console und Settings gibt es jetzt einen Slider `Target Zoom Magnification` (`1.0x` bis `4.0x`) für direkte Live-Anpassung.
- **Zoom-Overlay-Status**: Das Target-Zoom-Overlay zeigt jetzt zusätzlich `4K ZOOM: ON/OFF` und den aktuellen Vergrößerungsfaktor an.

### Changed
- **4K-Aushandlung für Netzwerkquellen verbessert**: Kamera-Parameter (Breite, Höhe, FPS, Buffer) werden nun auch für nicht-numerische Quellen (z. B. RTSP/HTTP/iPhone-Stream) aktiv gesetzt.
- **Backend-Fallback für Streams**: Für URL-Quellen wird zuerst FFmpeg versucht und bei Bedarf auf das Standard-Backend zurückgefallen.
- **Klare Ist/Soll-Diagnose im Log**: Nach Kamera-Öffnung wird jetzt `requested` gegen `actual` inklusive Backend geloggt. Bei Fallback auf niedrigere Auflösung wird explizit eine Warnung ausgegeben.

## [1.10.10] - 2026-06-05
### Added
- **Feedback Mechanism**: Added a "Help -> Send Feedback" menu option in the main menu bar. Opens a modal window to capture feedback, which is then saved as a timestamped JSON file in `Project_Horus/feedback/`.
- **Data Panel Filtering and Sorting**: The Data Panel list can now be filtered with a text box and sorted by clicking the table headers, including ID, class, position, confidence, and state.

## [1.10.9] - 2026-06-05
### Added
- **Motion Prediction & Template Adaptation for Active Pixel Tracking**: Implemented a Constant Velocity Motion Model to predict the target's position and center the search region dynamically. Added template adaptation (5% blend rate on highly confident matches) to handle appearance variations due to rotation, scale, or lighting changes. Increased default search padding from 40 to 80 pixels.

## [1.10.8] - 2026-06-05
### Added
- **Interactive Pixel Zoom Box Adjustment**: Enabled dragging, resizing, and positioning of the active pixel target's template box (ID 999) directly in the viewport during ROI edit mode. Moving the box or resizing its corners/edges dynamically crops a new template from the frame and resumes tracking seamlessly.

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
