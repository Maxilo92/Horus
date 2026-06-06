# Plan 10 — Motion Detection Module

**Status**: IMPLEMENTED (v1.11.0)
**Erstellt**: 2026-06-06
**Priorität**: HOCH

## Ziel

Eigenständiges Motion-Detection-Modul, das Bewegung im Bild erkennt —
unabhängig von YOLO-Objekterkennung und MultiTracker.

## Anforderungen

1. Personen erkennen ohne vorheriges Targeting
2. Mindestflächen-Filter: Regionen < N Pixel werden verworfen
3. Bewegung visuell im HUD sichtbar machen (Overlay)
4. Alle Parameter zur Laufzeit einstellbar (UI + persistent)

## Architektur

```
CameraModule::read(frame)
        │
        ▼
MotionDetector::process(frame, settings)   ← NEU
        │  std::vector<cv::Rect> motionRegions
        ▼
ObjectDetector::detect(frame)              ← unverändert
        │
        ▼
MultiTracker::update(detections)           ← unverändert
        │
        ▼
HUD::drawMotionOverlay(regions)            ← NEU (vor render())
HUD::render(trackedObjects)                ← unverändert
```

## Algorithmus

1. **Gaussian Blur** (optional, `motionBlurKernel`) — Rauschunterdrückung
2. **MOG2 Background Subtraction** (`cv::BackgroundSubtractorMOG2`)
   - `varThreshold` ← `motionSensitivity`
   - `detectShadows` ← `motionDetectShadows`
   - `learningRate` ← `motionLearningRate` (-1 = auto)
3. **Binarisierung** — Shadow-Pixel (127) → Hintergrund
4. **Morphological Opening** (MORPH_ELLIPSE, 3×3) — Rauschpixel entfernen
5. **findContours** + `contourArea() >= motionMinArea` → Bounding Rects

## Neue Dateien

- `src/vision/MotionDetector.hpp`
- `src/vision/MotionDetector.cpp`

## Geänderte Dateien

- `src/core/Common.hpp` — 10 neue Felder in `SystemSettings`
- `src/core/Application.hpp` — Member `m_motionDetector`, Shared State, Color Array
- `src/core/Application.cpp` — Pipeline + Settings UI + Persist + Hotswap-Reset
- `src/ui/HUD.hpp` — `drawMotionOverlay()` Deklaration + `m_motionColor`
- `src/ui/HUD.cpp` — `drawMotionOverlay()` Implementierung
- `CMakeLists.txt` — `MotionDetector.cpp` zu `CORE_SRCS`

## Konfigurierbare Parameter

| Parameter | Default | Beschreibung |
|---|---|---|
| `motionDetectionEnabled` | false | Master-Toggle |
| `motionShowOverlay` | true | Overlay An/Aus |
| `motionSensitivity` | 30.0 | MOG2 varThreshold [5–100] |
| `motionMinArea` | 50 px | Mindestfläche Kontur [1–5000] |
| `motionBlurKernel` | 5 | Gauss-Blur Kernel [1–21, odd] |
| `motionOverlayAlpha` | 0.35 | Fill-Transparenz [0–1] |
| `motionOverlayColor` | orange-red | RGBA Farbe |
| `motionDetectShadows` | false | MOG2 Schatten-Erkennung |
| `motionLearningRate` | -1 (auto) | Lernrate [-1, 0–100] |

## Visualisierung

**Option C** (Outline + Fill):
- Fill: semi-transparent, Farbe `motionOverlayColor`, Alpha `motionOverlayAlpha`
- Outline: `motionOverlayColor` mit fester Opazität (220/255)
- Z-Order: unter Tracking-Bounding-Boxes (wird zuerst gezeichnet)
- Koordinatentransformation: Frame-Space → Viewport-Space via `ViewportInfo`

## Safe States

- `motionDetectionEnabled = false` → Modul vollständig deaktiviert, kein CPU
- Kamera-Hotswap → `m_motionDetector.reset()` automatisch
- Leerer Frame → früher Rücksprung ohne Absturz
- Kontur außerhalb Frame-Bounds → geclampte Rect, kein UB
