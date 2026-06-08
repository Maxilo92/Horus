#include "ROIManager.hpp"
#include <algorithm>

// -----------------------------------------------------------------------
// addROI
// -----------------------------------------------------------------------
int ROIManager::addROI(const cv::Rect& rect, const std::string& label) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (static_cast<int>(m_zones.size()) >= kMaxZones) return -1;
    if (rect.width < 4 || rect.height < 4) return -1;  // Degenerate rect guard

    ROIZone zone;
    zone.id     = m_nextId++;
    zone.rect   = rect;
    zone.active = true;
    zone.label  = label.empty() ? ("Zone " + std::to_string(zone.id)) : label;
    m_zones.push_back(zone);
    return zone.id;
}

// -----------------------------------------------------------------------
// removeROI
// -----------------------------------------------------------------------
void ROIManager::removeROI(int id) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_zones.erase(
        std::remove_if(m_zones.begin(), m_zones.end(),
                       [id](const ROIZone& z){ return z.id == id; }),
        m_zones.end());
}

void ROIManager::clearAll() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_zones.clear();
}

// -----------------------------------------------------------------------
// toggleROI
// -----------------------------------------------------------------------
void ROIManager::toggleROI(int id) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& z : m_zones) {
        if (z.id == id) { z.active = !z.active; return; }
    }
}

// -----------------------------------------------------------------------
// setLabel
// -----------------------------------------------------------------------
void ROIManager::setLabel(int id, const std::string& label) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& z : m_zones) {
        if (z.id == id) { z.label = label; return; }
    }
}

// -----------------------------------------------------------------------
// setFunction
// -----------------------------------------------------------------------
void ROIManager::setFunction(int id, ROIFunction function) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& z : m_zones) {
        if (z.id == id) { z.function = function; return; }
    }
}

// -----------------------------------------------------------------------
// updateRect
// -----------------------------------------------------------------------
void ROIManager::updateRect(int id, const cv::Rect& rect) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& z : m_zones) {
        if (z.id == id) { z.rect = rect; return; }
    }
}

// -----------------------------------------------------------------------
// getROIs — returns a snapshot copy (safe from UI thread)
// -----------------------------------------------------------------------
std::vector<ROIZone> ROIManager::getROIs() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_zones;
}

// -----------------------------------------------------------------------
// hasActiveROI
// -----------------------------------------------------------------------
bool ROIManager::hasActiveROI() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& z : m_zones) {
        if (z.active) return true;
    }
    return false;
}

// -----------------------------------------------------------------------
// Drag management
// -----------------------------------------------------------------------
void ROIManager::beginDrag(const cv::Point& start) {
    m_dragStart   = start;
    m_dragCurrent = start;
    m_isDragging  = true;
}

void ROIManager::updateDrag(const cv::Point& current) {
    m_dragCurrent = current;
}

int ROIManager::commitDrag() {
    if (!m_isDragging) return -1;
    m_isDragging = false;
    cv::Rect r = normalizeRect(m_dragStart, m_dragCurrent);
    return addROI(r);
}

void ROIManager::cancelDrag() {
    m_isDragging = false;
}

cv::Rect ROIManager::getDragRect() const {
    if (!m_isDragging) return cv::Rect{};
    return normalizeRect(m_dragStart, m_dragCurrent);
}

// -----------------------------------------------------------------------
// normalizeRect — ensures positive width/height regardless of drag direction
// -----------------------------------------------------------------------
cv::Rect ROIManager::normalizeRect(const cv::Point& a, const cv::Point& b) const {
    int x = std::min(a.x, b.x);
    int y = std::min(a.y, b.y);
    int w = std::abs(a.x - b.x);
    int h = std::abs(a.y - b.y);
    return cv::Rect(x, y, w, h);
}

// -----------------------------------------------------------------------
// filterDetections — worker-thread hot-path
// Removes detections whose centroid is NOT inside any active ROI zone.
// If no active zone exists, all detections pass through unchanged.
// -----------------------------------------------------------------------
void ROIManager::filterDetections(std::vector<Detection>& detections) const {
    // Take a shared snapshot of active rects classified by function to minimize lock time
    std::vector<cv::Rect> detectionRects;
    std::vector<cv::Rect> excludeRects;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const auto& z : m_zones) {
            if (z.active) {
                if (z.function == ROIFunction::DETECTION || z.function == ROIFunction::ALARM) {
                    detectionRects.push_back(z.rect);
                } else if (z.function == ROIFunction::EXCLUDE) {
                    excludeRects.push_back(z.rect);
                }
            }
        }
    }

    if (detectionRects.empty() && excludeRects.empty()) return;  // No active filters

    detections.erase(
        std::remove_if(detections.begin(), detections.end(),
            [&detectionRects, &excludeRects](const Detection& det) -> bool {
                // Compute centroid of detection box
                cv::Point center(
                    det.box.x + det.box.width  / 2,
                    det.box.y + det.box.height / 2);

                // 1. Exclude if inside any active EXCLUDE zone
                for (const auto& r : excludeRects) {
                    if (r.contains(center)) return true; // discard
                }

                // 2. If there are active inclusion zones (DETECTION/ALARM), it must be inside at least one
                if (!detectionRects.empty()) {
                    bool insideAny = false;
                    for (const auto& r : detectionRects) {
                        if (r.contains(center)) {
                            insideAny = true;
                            break;
                        }
                    }
                    if (!insideAny) return true; // discard: outside all inclusions
                }

                return false;  // keep
            }),
        detections.end());
}
