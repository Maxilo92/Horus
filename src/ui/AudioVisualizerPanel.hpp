#ifndef AUDIO_VISUALIZER_PANEL_HPP
#define AUDIO_VISUALIZER_PANEL_HPP

#include <imgui.h>
#include <chrono>
#include "../core/Common.hpp"

class AudioEngine;
class Blackboard;

class AudioVisualizerPanel {
public:
    AudioVisualizerPanel();
    void draw(const AudioEngine& engine, Blackboard& blackboard, const ViewportInfo* view = nullptr);

private:
    float m_smoothIntensity = 0.0f;
    float m_peakLevel = 0.01f;
    float m_history[40];
    int   m_historyOffset = 0;
    std::chrono::steady_clock::time_point m_lastUpdate;
    bool  m_isVisible = false;
    float m_visibilityAlpha = 0.0f;
};

#endif // AUDIO_VISUALIZER_PANEL_HPP
