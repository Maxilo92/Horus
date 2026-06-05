# Design Document: Camera HUD Isolation and Telemetry Fix

## Problem
Currently, the camera HUD (Status Windows, Tactical Overlay, etc.) is rendered using `ImGui::GetForegroundDrawList()` and `ImGui::Begin()`, which makes it a global window covering the entire application. Furthermore, the FPS display uses the UI framerate, which does not represent the actual camera processing speed.

## Proposed Solution
1.  **Restrict HUD to Viewport:**
    *   Use `ImDrawList::PushClipRect` to ensure no HUD elements are drawn outside the video viewport.
    *   Convert `ImGui::Begin()` based status windows into manual `ImDrawList` text/rect overlays.
2.  **Camera-Side Telemetry:**
    *   Implement frame timing in `Application::workerLoop`.
    *   Calculate 'Camera FPS' based on processing time per frame.
    *   Display this 'Camera FPS' in the HUD instead of the global app FPS.

## Technical Details
- Coordinate System: `ViewportInfo` (pos_x, pos_y, target_w, target_h).
- Drawing API: `ImDrawList` (manual text/rect rendering).
- Telemetry: Sliding average or delta-time calculation in the processing thread.
