#ifndef UIMANAGER_HPP
#define UIMANAGER_HPP

#include "IModule.hpp"
#include "Blackboard.hpp"
#include "VideoRenderer.hpp"
#include "HUD.hpp"
#include "ROIManager.hpp"
#include "DataLogger.hpp"
#include "AudioEngine.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <memory>
#include <deque>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <atomic>

#include "DevConsolePanel.hpp"
#include "AnalyzerPanel.hpp"
#include "AudioVisualizerPanel.hpp"
#include "ReplayPanel.hpp"
#include "DossierArchivePanel.hpp"
#include "UpdateChecker.hpp"

class UIManager : public IModule {
public:
    using LogFn = std::function<void(LogLevel, const std::string&)>;

    UIManager(Blackboard& blackboard, ROIManager& roiManager,
              DataLogger& dataLogger, AudioEngine& audioEngine,
              GLFWwindow* window, const std::string& settingsPath,
              LogFn logFn);
    ~UIManager() override;

    // Must be called on the main thread after ImGui is initialised
    bool initRenderers();

    void start() override;
    void stop() override;

    void setDossierDatabase(DossierDatabase* db);
    void setUpdateChecker(UpdateChecker* uc);

    // Runs one full ImGui frame — call from Application::run()
    void update() override;

    // Thread-safe log entry (callable from any thread)
    void appendLog(LogLevel level, const std::string& msg, float appSeconds);

    void loadPersistedSettings(const std::string& path);
    void savePersistedSettings() const;

    // Settings & Injection

private:
    // ── Render helpers ───────────────────────────────────────────────────
    void renderCameraView(const cv::Mat& currentFrame,
                          const std::vector<TrackedObject>& tracked,
                          const TrackedTarget& locked,
                          const std::vector<cv::Rect>& motionRegions,
                          const VisionState& vision);
    void renderDataPanel(const std::vector<TrackedObject>& tracked,
                         const TrackedTarget& locked,
                         float cameraFps, float renderFps);
    void renderZoomWindow(const cv::Mat& zoomFrame,
                          const TrackedTarget& locked);
    void renderDevConsole(const VisionState& vision,
                          const TrackingStateData& tracking,
                          const AppStatusState& status);
    void renderSettingsWindow();
    void renderTargetAnalyzer(const TrackingStateData& tracking);
    void renderDossierPanel(const DossierState& dossier);

    // ── Export / utility ─────────────────────────────────────────────────
    bool exportTarget(const UniqueTargetRecord& record);
    bool exportTargetHistory(const std::vector<UniqueTargetRecord>& history);
    void takeScreenshot();
    bool saveFeedback(const std::string& text, const std::string& category, 
                      const std::string& priority, const std::string& contact);

    // ── Settings presets ─────────────────────────────────────────────────
    void applyStandardPreset();
    void applyPresetPerformance();
    void applyPresetBalanced();
    void applyPresetPrecision();
    void applyPresetLowLight();
    void syncColorEditorsFromSettings();
    void pushSettingsToBlackboard();

    // ── External Dependencies ────────────────────────────────────────────
    Blackboard&   m_blackboard;
    ROIManager&   m_roiManager;
    DataLogger&   m_dataLogger;
    AudioEngine&  m_audioEngine;
    DossierDatabase* m_dossierDb = nullptr;
    UpdateChecker*   m_updateChecker = nullptr;
    GLFWwindow*   m_window;
    std::string   m_settingsPath;
    LogFn         m_log;

    // ── VideoRenderers ───────────────────────────────────────────────────
    std::unique_ptr<VideoRenderer> m_renderer;
    std::unique_ptr<VideoRenderer> m_heatmapRenderer;
    std::unique_ptr<VideoRenderer> m_zoomRenderer;
    std::unique_ptr<VideoRenderer> m_subZoomRenderers[4];
    std::unique_ptr<HUD>           m_hud;

    // ── Panels (Maintainability Refactoring) ─────────────────────────────
    std::unique_ptr<DevConsolePanel> m_devConsolePanel;
    std::unique_ptr<AnalyzerPanel>   m_analyzerPanel;
    std::unique_ptr<AudioVisualizerPanel> m_audioVisualizerPanel;
    std::unique_ptr<ReplayPanel>     m_replayPanel;
    std::unique_ptr<DossierArchivePanel> m_dossierArchivePanel;

    // ── Settings (local; pushed to Blackboard on change) ─────────────────
    SystemSettings m_settings;

    // ── Per-frame display data (snapshot from Blackboard at start of update) ─
    std::vector<TrackedObject>     m_trackedObjects;
    TrackedTarget                  m_lockedTarget;
    std::vector<Detection>         m_detections;
    std::vector<UniqueTargetRecord> m_targetHistory;
    std::vector<cv::Rect>          m_motionRegions;
    float m_cameraFps = 0.0f;
    int   m_cameraWidth = 0, m_cameraHeight = 0;
    int   m_trackingWidth = 0, m_trackingHeight = 0;
    int   m_zoomWidth = 0, m_zoomHeight = 0;
    PerformanceMetrics m_perfMetrics;

    // ── Sub-zoom local state ─────────────────────────────────────────────
    struct SubZoomLocal {
        bool active = false;
        int  motion_id = -1;
        cv::Rect box;
        cv::Mat  frame;
        bool isLost = false;
    };
    SubZoomLocal m_subZooms[4];

    // ── Render timing ─────────────────────────────────────────────────────
    std::chrono::steady_clock::time_point m_lastRenderTime;
    float m_renderFps  = 0.0f;
    float m_frameTimeMs = 0.0f;
    std::chrono::steady_clock::time_point m_appStart;

    // ── Splash screen ─────────────────────────────────────────────────────
    bool  m_splashActive   = true;
    float m_splashMinSec   = 3.0f;
    bool  m_splashTimerSet = false;
    std::chrono::steady_clock::time_point m_splashShownAt;
    void  renderSplashScreen();

    // ── Setup wizard (first-run) ──────────────────────────────────────────
    bool m_setupWizardActive = false;
    int  m_setupWizardStep   = 0;
    char m_wizardCameraInput[64]  = {"1"};
    int  m_wizardAudioIdx         = -1;
    int  m_wizardModelIdx         = 0;   // 0 = yolov8s (genau), 1 = yolov8n (schnell)

    struct ModelStatus {
        std::string name;
        std::string filename;
        std::string url;
        bool        exists = false;
        bool        downloading = false;
        float       progress = 0.0f;
    };
    std::vector<ModelStatus> m_modelStatuses;
    void checkModelsExist();
    void startModelDownload(int idx);
    bool allModelsPresent();

    void renderSetupWizard();

    // ── UI state booleans ────────────────────────────────────────────────
    bool m_showSettingsWindow = false;
    bool m_showDataPanel      = true;
    bool m_showZoomWindow     = true;
    bool m_showDevConsole     = false;
    bool m_showTargetAnalyzer = true;
    bool m_showShortcutHelp   = false;
    bool m_showDossierPanel   = true;
    bool m_showDossierArchive = false;
    int  m_devConsoleTab      = 0;
    char m_dataPanelFilter[128] = {0};
    bool m_showUpdateDialog     = false;
    bool m_updateDismissed      = false;
    void renderUpdateDialog();
    bool m_showFeedbackWindow   = false;
    char m_feedbackBuf[1024]    = {0};
    int  m_feedbackCategoryIdx  = 0;
    int  m_feedbackPriorityIdx  = 2; // Normal
    char m_feedbackContactBuf[128] = {0};
    std::string m_feedbackStatus;
    bool m_screenshotPending      = false;
    bool m_resetWindowsPending    = false;

    // ── Target Analyzer Selection ────────────────────────────────────────
    int m_selectedAnalyzerTargetId = -1;

    // ── ROI editor state ─────────────────────────────────────────────────
    enum class ROIEditState {
        NONE, DRAWING, MOVING,
        RESIZING_TL, RESIZING_TR, RESIZING_BL, RESIZING_BR,
        RESIZING_L, RESIZING_R, RESIZING_T, RESIZING_B
    };
    bool         m_roiEditMode       = false;
    ROIEditState m_editState         = ROIEditState::NONE;
    int          m_editZoneId        = -1;
    cv::Point    m_editDragStartMouse;
    cv::Rect     m_editDragStartRect;
    char         m_roiLabelBuf[ROIManager::kMaxZones][64] = {};

    // ── Camera hot-swap UI ───────────────────────────────────────────────
    char         m_cameraInputBuf[256] = {0};
    std::string  m_cameraAddress;

    // ── Cached detector class names (from Blackboard AppStatus) ──────────
    std::vector<std::string> m_classNames;

    // ── Color arrays for ImGui ColorEdit ────────────────────────────────
    float m_hudColorF[4]           = {0.0f, 0.784f, 0.392f, 0.863f};
    float m_targetColorF[4]        = {1.0f, 0.706f, 0.0f,  1.0f};
    float m_motionOverlayColorF[4] = {1.0f, 0.35f,  0.0f,  0.65f};

    // ── Motion precision search cache ────────────────────────────────────
    std::unordered_map<int, std::pair<std::string, float>> m_motionPrecisionCache;
    std::mutex m_motionPrecisionMutex;

    // ── Export / background IO ───────────────────────────────────────────
    struct ExportTask {
        enum class Type { SCREENSHOT, TARGET, HISTORY, FEEDBACK };
        Type type;
        cv::Mat image;                                  // for screenshots
        UniqueTargetRecord targetRecord;                // for single target export
        std::vector<UniqueTargetRecord> history;         // for history export
        std::string text;                               // for feedback
        std::string category;                            // feedback category
        std::string priority;                            // feedback priority
        std::string contact;                             // feedback contact (optional)
        std::string timestamp;
    };
    std::deque<ExportTask>  m_exportQueue;
    std::mutex              m_exportMutex;
    std::condition_variable m_exportCv;
    std::thread             m_exportWorker;
    std::atomic<bool>       m_exportWorkerRunning{false};
    void exportWorkerLoop();
    void queueExportTask(ExportTask&& task);

    // ── Optimized History state ──────────────────────────────────────────
    std::vector<UniqueTargetRecord> m_filteredHistory;
    std::string                     m_lastFilterText;
    int                             m_lastSortCol     = -1;
    bool                            m_lastSortDesc    = false;
    size_t                          m_lastHistorySize = 0;
    void updateFilteredHistory(const std::string& filter, int sortCol, bool sortDesc);

    // ── Helpers used by render functions ────────────────────────────────
    void updateGLTexture(const cv::Mat& img, uint32_t& tex_id, int& tex_version, int current_version);
};

#endif // UIMANAGER_HPP
