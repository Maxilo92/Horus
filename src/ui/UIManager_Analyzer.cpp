#include "UIManager.hpp"
#include "AnalyzerPanel.hpp"

void UIManager::renderTargetAnalyzer(const TrackingStateData& tracking) {
    if (!m_analyzerPanel) return;

    m_analyzerPanel->render(
        m_selectedAnalyzerTargetId,
        m_targetHistory,
        m_blackboard,
        m_settings,
        m_log,
        [this](const UniqueTargetRecord& rec) { exportTarget(rec); },
        [this](const cv::Mat& img, uint32_t& tid, int& tver, int cver) {
            updateGLTexture(img, tid, tver, cver);
        }
    );
}
