#ifndef ROIMANAGER_HPP
#define ROIMANAGER_HPP

#include "Common.hpp"
#include <vector>
#include <string>
#include <array>
#include <mutex>

// -----------------------------------------------------------------------
// ROIFunction: The purpose/type of the ROI zone.
// -----------------------------------------------------------------------
enum class ROIFunction {
    DETECTION = 0, // Keep only detections inside this zone (Default/Include)
    EXCLUDE   = 1, // Filter out/ignore all detections inside this zone (Exclude)
    ALARM     = 2  // Raise alarm/log when detection enters this zone
};

// -----------------------------------------------------------------------
// ROIZone: A single axis-aligned rectangular surveillance zone.
// -----------------------------------------------------------------------
struct ROIZone {
    int         id       = -1;
    cv::Rect    rect;              // Pixel-space rectangle (video coordinates)
    bool        active   = true;    // Zone is enabled/disabled
    std::string label;            // Optional user-supplied label
    ROIFunction function = ROIFunction::DETECTION;
};

// -----------------------------------------------------------------------
// ROIManager: Manages up to kMaxZones interactive rectangular ROIs.
//
// Design constraints (military-grade):
//   - Max 4 simultaneous zones (bounded resource).
//   - Thread-safe: filter call from worker; edit calls from UI thread.
//   - No heap allocation during filter hot-path (operates on existing vectors).
// -----------------------------------------------------------------------
class ROIManager {
public:
    static constexpr int kMaxZones = 4;

    ROIManager() = default;

    // --- Zone management (UI thread) ------------------------------------

    // Add a new zone. Returns assigned ID, or -1 if at capacity.
    int  addROI(const cv::Rect& rect, const std::string& label = "");

    // Remove a zone by ID.
    void removeROI(int id);

    // Toggle active state for a zone.
    void toggleROI(int id);

    // Update the label of a zone.
    void setLabel(int id, const std::string& label);

    // Update the function/purpose of a zone.
    void setFunction(int id, ROIFunction function);

    // Update the rectangle coordinates of a zone.
    void updateRect(int id, const cv::Rect& rect);

    // Returns a snapshot of all zones (safe to call from UI thread).
    std::vector<ROIZone> getROIs() const;

    // Returns true if any zone is currently active.
    bool hasActiveROI() const;

    // --- Interactive editing (UI thread) ---------------------------------

    // Begin a new drag gesture (video-space coordinates).
    void beginDrag(const cv::Point& start);

    // Update drag end point (video-space coordinates).
    void updateDrag(const cv::Point& current);

    // Commit the current drag as a new ROI zone.
    // Returns the new zone's ID, or -1 if nothing was committed.
    int  commitDrag();

    // Cancel an in-progress drag.
    void cancelDrag();

    // Returns true if a drag is currently in progress.
    bool isDragging() const { return m_isDragging; }

    // Returns the current drag rectangle (video-space), or empty Rect.
    cv::Rect getDragRect() const;

    // --- Worker-thread filter -------------------------------------------

    // Removes detections whose center lies outside ALL active ROIs.
    // If no active ROI exists, all detections pass through unchanged.
    void filterDetections(std::vector<Detection>& detections) const;

private:
    cv::Rect normalizeRect(const cv::Point& a, const cv::Point& b) const;

    mutable std::mutex    m_mutex;
    std::vector<ROIZone>  m_zones;
    int                   m_nextId   = 0;

    // Drag state
    bool     m_isDragging   = false;
    cv::Point m_dragStart;
    cv::Point m_dragCurrent;
};

#endif // ROIMANAGER_HPP
