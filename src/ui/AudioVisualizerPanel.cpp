#include "AudioVisualizerPanel.hpp"
#include "../core/AudioEngine.hpp"
#include "../core/Blackboard.hpp"
#include <algorithm>
#include <cmath>

AudioVisualizerPanel::AudioVisualizerPanel() {
    for (int i = 0; i < 40; ++i) m_history[i] = 0.0f;
    m_lastUpdate = std::chrono::steady_clock::now();
}

void AudioVisualizerPanel::draw(const AudioEngine& engine, Blackboard& blackboard, const ViewportInfo* view) {
    (void)engine;
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - m_lastUpdate).count();
    m_lastUpdate = now;

    float rawIntensity = blackboard.getAudioState().intensity;

    // Smoothing: rapid attack, slower decay
    if (rawIntensity > m_smoothIntensity) {
        m_smoothIntensity = rawIntensity;
    } else {
        m_smoothIntensity -= dt * 3.0f;
    }
    if (m_smoothIntensity < 0.0f) m_smoothIntensity = 0.0f;

    // Auto-scale: track peak with slow decay so bars never clip
    if (m_smoothIntensity > m_peakLevel) {
        m_peakLevel = m_smoothIntensity;
    } else {
        m_peakLevel -= dt * 0.05f;
    }
    m_peakLevel = std::max(m_peakLevel, 0.01f);

    // History update (normalized)
    float normalized = m_smoothIntensity / m_peakLevel;
    m_history[m_historyOffset] = normalized;
    m_historyOffset = (m_historyOffset + 1) % 40;

    // Visibility logic (fade in/out)
    bool active = (m_smoothIntensity > 0.001f);
    if (active) {
        m_visibilityAlpha += dt * 5.0f; // Rapid fade in
    } else {
        m_visibilityAlpha -= dt * 1.0f; // Slower fade out
    }
    m_visibilityAlpha = std::max(0.0f, std::min(m_visibilityAlpha, 1.0f));

    if (m_visibilityAlpha <= 0.0f) return;

    // Position calculation
    ImVec2 windowSize = ImVec2(240, 70);
    ImVec2 windowPos;

    if (view && view->target_w > 0) {
        // Bottom-right of camera image
        float margin = 10.0f;
        windowPos = ImVec2(view->pos_x + view->target_w - windowSize.x - margin,
                           view->pos_y + view->target_h - windowSize.y - margin);
    } else {
        // Default to bottom-right of entire screen if no view provided
        ImGuiIO& io = ImGui::GetIO();
        float margin = 20.0f;
        windowPos = ImVec2(io.DisplaySize.x - windowSize.x - margin, 
                           io.DisplaySize.y - windowSize.y - margin);
    }
    
    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(windowSize);
    ImGui::SetNextWindowBgAlpha(0.6f * m_visibilityAlpha);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | 
                            ImGuiWindowFlags_NoInputs | 
                            ImGuiWindowFlags_NoNav | 
                            ImGuiWindowFlags_NoFocusOnAppearing | 
                            ImGuiWindowFlags_NoSavedSettings |
                            ImGuiWindowFlags_NoBackground; // Make it more transparent/integrated

    // If we are inside the camera view, we should not use Begin() as it creates a new top-level window
    // BUT we want it to sit "on top" and be a separate window for ease of development for now.
    // To make it truly "inside" without a separate window, we'd use the drawlist directly.
    // Let's use the drawlist directly if view is provided for better integration.

    if (view && view->target_w > 0) {
        ImDrawList* dl = ImGui::GetForegroundDrawList(); // Use foreground to stay on top
        
        // Background rect
        ImVec2 p1 = ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y);
        dl->AddRectFilled(windowPos, p1, ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.05f, 0.6f * m_visibilityAlpha)), 4.0f);
        dl->AddRect(windowPos, p1, ImGui::GetColorU32(ImVec4(0.0f, 0.9f, 0.5f, 0.4f * m_visibilityAlpha)), 4.0f);

        ImVec2 p0 = ImVec2(windowPos.x + 5, windowPos.y + 5);
        
        // Render animated bars
        float availWidth = windowSize.x - 10.0f;
        float availHeight = windowSize.y - 25.0f;
        float barWidth = availWidth / 40.0f;
        
        for (int i = 0; i < 40; ++i) {
            int idx = (m_historyOffset + i) % 40;
            float h = m_history[idx] * availHeight;
            
            ImVec2 b0 = ImVec2(p0.x + i * barWidth, p0.y + availHeight - h);
            ImVec2 b1 = ImVec2(p0.x + (i + 1) * barWidth - 1.0f, p0.y + availHeight);
            
            ImU32 color = ImGui::GetColorU32(ImVec4(0.0f, 0.9f, 0.5f, m_visibilityAlpha * (0.4f + 0.6f * (i/40.0f))));
            dl->AddRectFilled(b0, b1, color);
        }
        
        // Status text
        char label[] = "AUDIO SIGNAL ACTIVE";
        dl->AddText(ImVec2(windowPos.x + 10, windowPos.y + windowSize.y - 18), 
                    ImGui::GetColorU32(ImVec4(0.0f, 1.0f, 0.6f, m_visibilityAlpha)), label);
    } else {
        if (ImGui::Begin("Audio Visualizer", nullptr, flags)) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            p0.x += 5; p0.y += 5;
            
            // Render animated bars
            float availWidth = windowSize.x - 10.0f;
            float availHeight = windowSize.y - 25.0f;
            float barWidth = availWidth / 40.0f;
            
            for (int i = 0; i < 40; ++i) {
                int idx = (m_historyOffset + i) % 40;
                float h = m_history[idx] * availHeight;
                
                ImVec2 b0 = ImVec2(p0.x + i * barWidth, p0.y + availHeight - h);
                ImVec2 b1 = ImVec2(p0.x + (i + 1) * barWidth - 1.0f, p0.y + availHeight);
                
                ImU32 color = ImGui::GetColorU32(ImVec4(0.0f, 0.9f, 0.5f, m_visibilityAlpha * (0.4f + 0.6f * (i/40.0f))));
                drawList->AddRectFilled(b0, b1, color);
            }
            
            ImGui::SetCursorPos(ImVec2(10, windowSize.y - 18));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.6f, m_visibilityAlpha));
            ImGui::Text("AUDIBLE FEEDBACK ACTIVE");
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }
}
