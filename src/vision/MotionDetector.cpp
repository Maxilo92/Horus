#include "MotionDetector.hpp"
#include <algorithm>
#include <cmath>

// ============================================================
// MotionDetector — Plan 10
// ============================================================
// Algorithm summary:
//   1. Optional Gaussian blur (noise suppression)
//   2. MOG2 background subtraction → binary foreground mask
//   3. Morphological opening (MORPH_ELLIPSE) → remove noise pixels
//   4. findContours → filter by minArea → output bounding rects
// ============================================================

MotionDetector::MotionDetector() {
    // MOG2 defaults — overridden per-frame from SystemSettings
    // history=500 (frames to build background model), varThreshold set at runtime
    m_subtractor = cv::createBackgroundSubtractorMOG2(
        /*history=*/500,
        /*varThreshold=*/30.0,
        /*detectShadows=*/false
    );
    m_initialized = true;
}

void MotionDetector::reset() {
    // Recreate the subtractor to fully flush the background model.
    // Parameters will be re-applied on the next process() call.
    m_subtractor = cv::createBackgroundSubtractorMOG2(500, 30.0, false);
    m_regions.clear();
    m_fgMask.release();
    m_cleanMask.release();
    m_blurred.release();
}

void MotionDetector::applyMorphCleanup(int blurKernel) {
    // Rebuild structuring element only when kernel size changes.
    // Use an odd, minimum-1 size for the morph element.
    const int morphSize = 3;
    if (m_morphKernel.empty()) {
        m_morphKernel = cv::getStructuringElement(
            cv::MORPH_ELLIPSE, cv::Size(morphSize, morphSize));
    }
    (void)blurKernel; // Kernel used upstream in process()

    // Opening = Erosion followed by Dilation:
    // Removes small noise pixels while preserving larger motion blobs.
    cv::morphologyEx(m_fgMask, m_cleanMask, cv::MORPH_OPEN, m_morphKernel);
}

bool MotionDetector::process(const cv::Mat& frame, const SystemSettings& settings) {
    if (frame.empty() || !m_initialized) return false;

    // --- Step 1: Optional Gaussian blur ---
    // Odd kernel only; clamp to valid range [1, 21]
    int blurKernel = settings.motionBlurKernel;
    if (blurKernel < 1)  blurKernel = 1;
    if (blurKernel > 21) blurKernel = 21;
    if (blurKernel % 2 == 0) blurKernel += 1; // Must be odd

    if (blurKernel > 1) {
        cv::GaussianBlur(frame, m_blurred, cv::Size(blurKernel, blurKernel), 0);
    } else {
        m_blurred = frame; // No copy — just reference (read-only downstream)
    }

    // --- Step 2: Update MOG2 background model ---
    // varThreshold: lower = more sensitive (detects subtler changes)
    // detectShadows: shadow pixels are marked as 127 (gray), we treat them
    // as background (threshold to strict binary below).
    m_subtractor->setVarThreshold(static_cast<double>(settings.motionSensitivity));
    m_subtractor->setDetectShadows(settings.motionDetectShadows);

    double learningRate = (settings.motionLearningRate < 0)
        ? -1.0   // auto (MOG2 default: ~1/history)
        : std::clamp(settings.motionLearningRate / 100.0, 0.0, 1.0);

    m_subtractor->apply(m_blurred, m_fgMask, learningRate);

    // Binarize: shadow pixels (=127) → 0 (background), foreground (=255) → 255
    cv::threshold(m_fgMask, m_fgMask, 200, 255, cv::THRESH_BINARY);

    // --- Step 3: Morphological cleanup ---
    applyMorphCleanup(blurKernel);

    // --- Step 4: Find contours and filter by area ---
    m_contours.clear();
    m_regions.clear();

    cv::findContours(m_cleanMask, m_contours,
                     cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    for (const auto& contour : m_contours) {
        double area = cv::contourArea(contour);
        if (area < static_cast<double>(settings.motionMinArea)) continue;

        cv::Rect bbox = cv::boundingRect(contour);

        // Bounds safety: clamp rect to frame dimensions
        bbox.x      = std::max(0, bbox.x);
        bbox.y      = std::max(0, bbox.y);
        bbox.width  = std::min(bbox.width,  frame.cols - bbox.x);
        bbox.height = std::min(bbox.height, frame.rows - bbox.y);

        if (bbox.width > 0 && bbox.height > 0) {
            m_regions.push_back(bbox);
        }
    }

    return !m_regions.empty();
}
