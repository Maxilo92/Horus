#ifndef SINGLETRACKER_HPP
#define SINGLETRACKER_HPP

#include "Common.hpp"
#include <vector>

class SingleTracker {
public:
    SingleTracker();
    ~SingleTracker() = default;

    void lockOn(const Detection& detection);
    void releaseLock();
    void update(const std::vector<Detection>& detections, const SystemSettings& settings);

    const TrackedTarget& getTarget() const { return m_target; }
    bool isLocked() const { return m_target.state != TrackingState::SEARCHING; }

private:
    float calculateIOU(const cv::Rect& box1, const cv::Rect& box2) const;

    TrackedTarget m_target;
    float m_vx, m_vy, m_vw, m_vh;
};

#endif // SINGLETRACKER_HPP
