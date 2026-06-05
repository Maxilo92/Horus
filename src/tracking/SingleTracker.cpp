#include "SingleTracker.hpp"
#include <algorithm>
#include <cmath>

SingleTracker::SingleTracker() : m_vx(0), m_vy(0), m_vw(0), m_vh(0) {
    m_target.state = TrackingState::SEARCHING;
}

void SingleTracker::lockOn(const Detection& detection) {
    m_target.state = TrackingState::LOCKED;
    m_target.class_id = detection.class_id;
    m_target.box = detection.box;
    m_target.className = detection.className;
    m_target.confidence = detection.confidence;
    m_target.lost_frames = 0;
    m_vx = m_vy = m_vw = m_vh = 0;
}

void SingleTracker::releaseLock() {
    m_target.state = TrackingState::SEARCHING;
}

void SingleTracker::update(const std::vector<Detection>& detections, const SystemSettings& settings) {
    if (m_target.state == TrackingState::SEARCHING) return;
    float bestIOU = 0.0f;
    const Detection* bestMatch = nullptr;
    cv::Rect predictedBox = m_target.box;
    predictedBox.x += static_cast<int>(std::round(m_vx));
    predictedBox.y += static_cast<int>(std::round(m_vy));
    predictedBox.width += static_cast<int>(std::round(m_vw));
    predictedBox.height += static_cast<int>(std::round(m_vh));
    for (const auto& det : detections) {
        if (det.class_id != m_target.class_id) continue;
        float iou = calculateIOU(predictedBox, det.box);
        if (iou > bestIOU && iou > 0.2f) {
            bestIOU = iou;
            bestMatch = &det;
        }
    }
    if (bestMatch) {
        float alpha = 0.6f;
        float new_vx = (float)(bestMatch->box.x - m_target.box.x);
        float new_vy = (float)(bestMatch->box.y - m_target.box.y);
        m_vx = alpha * new_vx + (1.0f - alpha) * m_vx;
        m_vy = alpha * new_vy + (1.0f - alpha) * m_vy;
        m_target.box = bestMatch->box;
        m_target.confidence = bestMatch->confidence;
        m_target.state = TrackingState::LOCKED;
        m_target.lost_frames = 0;
    } else {
        m_target.lost_frames++;
        if (m_target.lost_frames > settings.trackerMaxLostFrames) m_target.state = TrackingState::LOST;
        else {
            m_target.box = predictedBox;
            m_vx *= 0.9f; m_vy *= 0.9f;
        }
    }
}

float SingleTracker::calculateIOU(const cv::Rect& box1, const cv::Rect& box2) const {
    int x1 = std::max(box1.x, box2.x), y1 = std::max(box1.y, box2.y), x2 = std::min(box1.x + box1.width, box2.x + box2.width), y2 = std::min(box1.y + box1.height, box2.y + box2.height);
    if (x1 >= x2 || y1 >= y2) return 0.0f;
    float intersectionArea = (float)((x2 - x1) * (y2 - y1)), area1 = (float)(box1.width * box1.height), area2 = (float)(box2.width * box2.height);
    return intersectionArea / (area1 + area2 - intersectionArea);
}
