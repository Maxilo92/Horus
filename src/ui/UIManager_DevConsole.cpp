#include "UIManager.hpp"
#include "DevConsolePanel.hpp"

void UIManager::renderDevConsole(const VisionState& vision,
                                 const TrackingStateData& tracking,
                                 const AppStatusState& status) {
    if (!m_devConsolePanel) return;

    bool settingsChanged = false;
    FaceDebugState faceDbg = m_blackboard.getFaceDebugState();

    m_devConsolePanel->render(
        m_showDevConsole,
        m_renderFps, m_frameTimeMs, m_cameraFps,
        m_perfMetrics.inferenceTimeMs, m_perfMetrics.trackingTimeMs, m_perfMetrics.dataLoggerQueue,
        m_cameraWidth, m_cameraHeight, m_trackingWidth, m_trackingHeight, m_zoomWidth, m_zoomHeight,
        (int)m_trackedObjects.size(), (int)m_detections.size(), (uint64_t)status.totalFramesProcessed,
        m_classNames,
        m_cameraAddress,
        status,
        m_blackboard.isCameraChangePending(),
        [this](const std::string& addr) { m_blackboard.requestCameraChange(addr); },
        [this]() { m_blackboard.requestMotionDetectorReset(); },
        [this]() { applyStandardPreset(); },
        m_settings,
        settingsChanged,
        m_log,
        m_window,
        status.remoteInferenceRttMs,
        status.activeModelName,
        &tracking,
        &faceDbg
    );

    if (settingsChanged) {
        pushSettingsToBlackboard();
        savePersistedSettings();
    }
}
