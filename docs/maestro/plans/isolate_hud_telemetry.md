# Plan: Isolate Camera HUD and Fix FPS Telemetry

## Objective
Restrict the HUD overlay to the camera feed viewport area and ensure the FPS display reflects the camera frame rate instead of the UI frame rate.

## Key Files & Context
- **src/Application.hpp/cpp**: Worker loop for camera timing and shared state management.
- **src/HUD.hpp/cpp**: HUD rendering logic and coordinate management.
- **src/Common.hpp**: shared data structures.

## Implementation Steps

### 1. Camera FPS Calculation (Application)
- [ ] Add `m_cameraFps` and `m_sharedCameraFps` (float) to `Application` class.
- [ ] In `Application::workerLoop`, implement a sliding average or simple delta-time calculation for camera frames.
- [ ] Share the calculated `m_cameraFps` via the `m_dataMutex`.
- [ ] In `Application::run`, retrieve the camera FPS from shared state and pass it to `m_hud->render`.

### 2. HUD Viewport Isolation (HUD)
- [ ] In `HUD::render`, implement `drawList->PushClipRect` using the `ViewportInfo` bounds (pos_x, pos_y to pos_x + target_w, pos_y + target_h).
- [ ] Call `drawList->PopClipRect` at the end of `render`.
- [ ] Ensure `drawTacticalOverlay` and `drawCornerBrackets` use the viewport bounds correctly (this is already partially done, but needs verification).

### 3. Refactor Status Windows (HUD)
- [ ] Modify `HUD::drawStatusWindows` to stop using `ImGui::Begin`/`End`.
- [ ] Instead, use `drawList->AddRectFilled` (for background) and `drawList->AddText` (for telemetry) directly.
- [ ] Position these "Status Blocks" relative to the `ViewportInfo` (e.g., bottom-left corner of the video).
- [ ] Update `HUD::render` to pass the `drawList` to `drawStatusWindows`.

### 4. Verification & Testing
- [ ] **Viewport Check:** Move/resize the window and ensure the HUD elements never "leak" outside the black bars or onto the Dev Console.
- [ ] **FPS Check:** Verify that the FPS number changes based on camera speed (e.g., if processing slows down, the number should drop even if the UI remains smooth).
- [ ] **Visual Consistency:** Ensure the new text overlays match the previous "SysLog" and "Data" style.

## Migration & Versioning
- [ ] Bump version to `1.6.0` in `VERSION`.
- [ ] Update `CHANGELOG.md`.
