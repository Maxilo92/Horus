#ifndef MOTION_DETECTOR_HPP
#define MOTION_DETECTOR_HPP

#include <opencv2/opencv.hpp>
#include <opencv2/video/background_segm.hpp>
#include <vector>
#include "Common.hpp"

// ============================================================
// MotionDetector — Plan 10
// ============================================================
// Stateless-from-caller perspective: caller passes a frame each
// iteration and reads results via getMotionRegions() / getMask().
// Runs entirely in the worker thread. All internal buffers are
// pre-allocated to avoid heap allocation in the hot path.
// ============================================================

class MotionDetector {
public:
    MotionDetector();

    // Process one frame.
    // Returns true if at least one motion region (above minArea) was detected.
    // Thread-safety: NOT thread-safe — call only from the worker thread.
    bool process(const cv::Mat& frame, const SystemSettings& settings);

    // Bounding rectangles of detected motion regions (filtered by minArea).
    // Valid until the next call to process().
    const std::vector<cv::Rect>& getMotionRegions() const { return m_regions; }

    // Single-channel binary mask (CV_8UC1, same size as input frame) showing
    // motion pixels after morphological cleanup. Used by HUD for fill overlay.
    // Valid until the next call to process().
    const cv::Mat& getMask() const { return m_cleanMask; }

    // Reset the background model (e.g. on camera change).
    // The subtractor will re-learn the background over the next few frames.
    void reset();

private:
    // Applies morphological opening to remove noise pixels.
    void applyMorphCleanup(int blurKernel);

    cv::Ptr<cv::BackgroundSubtractorMOG2> m_subtractor;

    // Pre-allocated processing buffers (no heap in hot path)
    cv::Mat              m_blurred;      // Gauss-blurred input
    cv::Mat              m_fgMask;       // Raw foreground mask from MOG2
    cv::Mat              m_cleanMask;    // After morphological opening
    cv::Mat              m_morphKernel;  // Structuring element (pre-built)

    std::vector<cv::Rect>              m_regions;   // Output: filtered bounding boxes
    std::vector<std::vector<cv::Point>> m_contours; // Reused across frames

    int  m_lastBlurKernel = -1; // Track kernel changes to avoid rebuilding
    bool m_initialized    = false;
};

#endif // MOTION_DETECTOR_HPP
