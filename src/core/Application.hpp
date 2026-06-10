#ifndef APPLICATION_HPP
#define APPLICATION_HPP

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <string>
#include <memory>
#include <chrono>

#include "Common.hpp"
#include "Blackboard.hpp"
#include "DataLogger.hpp"
#include "ROIManager.hpp"
#include "AudioEngine.hpp"
#include "AudioCapture.hpp"
#include "DossierDatabase.hpp"
#include "UpdateChecker.hpp"
#include "../vision/AIAgent.hpp"

// Forward declarations — headers included only in .cpp
class VisionSystem;
class TrackingSystem;
class UIManager;

class Application {
public:
    Application();
    ~Application();

    bool init(int argc, char** argv);
    void run();

    // Thread-safe log appender (callable from worker threads via LogFn lambda)
    void log(LogLevel level, const std::string& msg);

private:
    bool initGLFW();
    bool initImGui();
    void drawSplashFrame();          // re-renders the boot terminal to the screen
    void bootBegin(const std::string& msg);  // add pending line + render
    void bootEnd(bool ok = true);            // resolve last line + render
    void cleanup();

    // ── Boot log ──────────────────────────────────────────────────────────
    enum class BootStatus { PENDING, OK, FAIL };
    struct BootLine { std::string msg; BootStatus status = BootStatus::PENDING; };
    std::vector<BootLine> m_bootLog;

    Blackboard  m_blackboard;
    GLFWwindow* m_window = nullptr;
    int         m_width  = 1280;
    int         m_height = 720;
    std::string m_title;
    std::string m_settingsPath;

    std::unique_ptr<ROIManager>     m_roiManager;
    std::unique_ptr<DataLogger>     m_dataLogger;
    AudioEngine                     m_audioEngine;
    AudioCapture                    m_audioCapture;

    std::unique_ptr<DossierDatabase> m_dossierDb;
    std::unique_ptr<AIAgent>         m_aiAgent;

    std::unique_ptr<UpdateChecker>  m_updateChecker;
    std::unique_ptr<VisionSystem>   m_visionSystem;
    std::unique_ptr<TrackingSystem> m_trackingSystem;
    std::unique_ptr<UIManager>      m_uiManager;

    std::chrono::steady_clock::time_point m_appStart;
};

#endif // APPLICATION_HPP
