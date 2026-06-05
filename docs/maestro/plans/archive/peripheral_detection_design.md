# Design Document: Peripheral Detection Optimization

## Problem Statement
The current system fails to reliably detect people in the peripheral areas ('corners') of the video feed. This is due to:
1.  **Image Squashing:** 16:9 input is squashed to 1:1 square for AI, distorting features.
2.  **High Thresholds:** Hardcoded confidence/score thresholds (0.45) filter out distant/small objects.
3.  **Tracker Lag:** 2-frame stability requirement delays visualization.
4.  **UI Limitations:** Only the main target is shown on the HUD, obscuring other detections.

## Proposed Solution
Transition to a full Multi-Object Tracking (MOT) pipeline with Optimized Preprocessing:
1.  **Letterboxing:** Preserve aspect ratio during AI preprocessing.
2.  **Lower Thresholds:** Move default score threshold to 0.25 and expose it to the UI.
3.  **Instant Tracking:** Show tracks immediately upon detection.
4.  **MOT HUD:** Display all tracks with stable IDs and trails.

## Technical Details
- Model: YOLOv8n (640x640)
- Input: 1280x720 (16:9)
- Tracking Algorithm: Kalman-Filter based Multi-Object Tracking.
- Framework: C++ / OpenCV / ImGui.
