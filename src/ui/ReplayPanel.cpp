#include "ReplayPanel.hpp"
#include <imgui.h>
#include <cstdio>

void ReplayPanel::render(Blackboard& blackboard) {
    ReplayState state = blackboard.getReplayState();
    if (!state.isFile) return;

    ImGui::Begin("Replay Control", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    // Timeline Slider
    int currentFrame = state.currentFrame;
    if (ImGui::SliderInt("##Timeline", &currentFrame, 0, state.totalFrames)) {
        ReplayCommand cmd;
        cmd.seekRequested = true;
        cmd.seekFrame = currentFrame;
        blackboard.setReplayCommand(cmd);
    }

    // Controls
    if (ImGui::Button(state.isPlaying ? "Pause" : "Play")) {
        ReplayCommand cmd;
        if (state.isPlaying) cmd.pauseRequested = true;
        else cmd.playRequested = true;
        blackboard.setReplayCommand(cmd);
    }
    ImGui::SameLine();
    if (ImGui::Button("|<")) {
        ReplayCommand cmd;
        cmd.stepRequested = true;
        cmd.stepDirection = -1;
        blackboard.setReplayCommand(cmd);
    }
    ImGui::SameLine();
    if (ImGui::Button(">|")) {
        ReplayCommand cmd;
        cmd.stepRequested = true;
        cmd.stepDirection = 1;
        blackboard.setReplayCommand(cmd);
    }

    // Status Info
    double currentTime = (state.fps > 0) ? (double)state.currentFrame / state.fps : 0;
    double totalTime = (state.fps > 0) ? (double)state.totalFrames / state.fps : 0;

    ImGui::Text("Time: %s / %s", formatTime(currentTime).c_str(), formatTime(totalTime).c_str());
    ImGui::Text("Frame: %d / %d (@%.2f FPS)", state.currentFrame, state.totalFrames, state.fps);

    ImGui::End();
}

std::string ReplayPanel::formatTime(double seconds) {
    int h = (int)(seconds / 3600);
    int m = (int)((seconds - h * 3600) / 60);
    int s = (int)(seconds - h * 3600 - m * 60);
    int ms = (int)((seconds - (int)seconds) * 1000);

    char buf[64];
    if (h > 0) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", h, m, s, ms);
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d.%03d", m, s, ms);
    }
    return std::string(buf);
}
