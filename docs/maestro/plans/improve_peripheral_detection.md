# Plan: Improve Peripheral Detection (Optimization A)

## Objective
Enhance motion detection sensitivity in peripheral areas ('the last corner') by fixing image distortion during preprocessing, lowering detection thresholds, and enabling full multi-object tracking visualization.

## Key Files & Context
- **src/ObjectDetector.hpp/cpp**: Core detection logic (YOLOv8 preprocessing).
- **src/Common.hpp**: System-wide settings and thresholds.
- **src/MultiTracker.hpp/cpp**: Multi-object tracking logic and stability requirements.
- **src/Application.hpp/cpp**: Application glue code, threading, and UI.
- **src/HUD.hpp/cpp**: Head-Up Display rendering.

## Implementation Steps

### 1. Research & Preprocessing Fix (ObjectDetector)
- [ ] Modify `ObjectDetector::detect` to implement **Letterbox Resizing**.
    - Instead of squashing 16:9 to 1:1, add black padding to maintain aspect ratio.
    - This ensures people in corners aren't distorted and are easier for the AI to recognize.
- [ ] Implement inverse coordinate mapping to translate bounding boxes from the padded 640x640 input back to the original 1280x720 frame coordinates.

### 2. Threshold Optimization (Common & Detector)
- [ ] In `Common.hpp`, reduce default `detectorScoreThreshold` from `0.45f` to `0.25f`.
- [ ] Ensure `detectorScoreThreshold` is used as a filter in `ObjectDetector.cpp`.
- [ ] Expose `detectorScoreThreshold` and `detectorNmsThreshold` in the Dev Console (ImGui) within `Application.cpp`.

### 3. Tracking Responsiveness (MultiTracker)
- [ ] Modify `MultiTracker::getTrackedObjects` to allow displaying tracks immediately upon creation.
- [ ] Remove the requirement for `is_confirmed == true` for the initial rendering pass to reduce "detection lag" in peripheral areas.

### 4. Integration & Multi-Target Display (Application & HUD)
- [ ] Update `Application.hpp` to replace `SingleTracker` with `MultiTracker`.
- [ ] Update `Application.cpp` to use `MultiTracker` in the worker loop.
- [ ] Modify `HUD::render` signature and implementation:
    - Change it to accept `const std::vector<TrackedObject>& trackedObjects`.
    - Update rendering logic to draw persistent IDs and trails (if enabled).
- [ ] Update `Application::run` to pass the actual list of tracked objects to the HUD.

### 5. Documentation & Versioning
- [ ] Bump version to `1.3.2` in `VERSION`.
- [ ] Update `CHANGELOG.md` with the new improvements.

## Verification & Testing
- **Visual Check:** Verify that objects in the extreme corners of the 1280x720 frame are correctly detected and tracked.
- **HUD Check:** Confirm that multiple objects can be tracked simultaneously and that their IDs remain stable.
- **Sensitivity Check:** Use the new sliders in the Dev Console to confirm that lowering the Score threshold increases detection frequency in distant areas.
- **Regression Test:** Ensure the "Single Target Lock" feature (if still desired) still functions by interacting with one of the multi-tracks.
