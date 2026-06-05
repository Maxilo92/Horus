#include "Application.hpp"
#include <iostream>
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

Application::Application() 
    : m_window(nullptr), m_width(1280), m_height(720), m_title("Project Horus - Target Viewer"), 
      m_lockRequested(false), m_running(false), m_newDataAvailable(false) {
    m_camera = std::make_unique<CameraModule>();
    m_renderer = std::make_unique<VideoRenderer>();
    m_hud = std::make_unique<HUD>();
    m_tracker = std::make_unique<MultiTracker>();
}

Application::~Application() {
    cleanup();
}

bool Application::init(int argc, char** argv) {
    if (argc > 1) m_cameraAddress = argv[1];
    else m_cameraAddress = "1";

    if (!m_camera->open(m_cameraAddress)) {
        std::cerr << "[ERROR] Camera fail: " << m_cameraAddress << std::endl;
        return false;
    }

    try {
        // macOS App Bundle path check (relative to Contents/MacOS/)
        std::string modelPath = "../Resources/yolov8n.onnx";
        std::string labelsPath = "../Resources/coco.txt";
        
        FILE* f = fopen(modelPath.c_str(), "r");
        if (!f) {
            // Fallback for development/standalone
            modelPath = "../assets/models/yolov8n.onnx";
            labelsPath = "../assets/models/coco.txt";
            f = fopen(modelPath.c_str(), "r");
        }
        
        if (!f) {
            modelPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/assets/models/yolov8n.onnx";
            labelsPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/assets/models/coco.txt";
            f = fopen(modelPath.c_str(), "r");
        }
        
        if (f) fclose(f);
        else {
            modelPath = "assets/models/yolov8n.onnx";
            labelsPath = "assets/models/coco.txt";
        }
        m_detector = std::make_unique<ObjectDetector>(modelPath, labelsPath);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Detector fail: " << e.what() << std::endl;
        return false;
    }

    if (!initGLFW()) return false;
    if (!initImGui()) return false;

    m_running = true;
    m_workerThread = std::thread(&Application::workerLoop, this);
    return true;
}

bool Application::initGLFW() {
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    m_window = glfwCreateWindow(m_width, m_height, m_title.c_str(), NULL, NULL);
    if (!m_window) return false;
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);
    if (glewInit() != GLEW_OK) return false;
    return true;
}

bool Application::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows
    
    ImGui_ImplGlfw_InitForOpenGL(m_window, true);
    ImGui_ImplOpenGL3_Init("#version 150");
    return true;
}

void Application::workerLoop() {
    cv::Mat frame;
    auto lastTime = std::chrono::steady_clock::now();
    while (m_running) {
        if (m_camera->read(frame)) {
            auto currentTime = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(currentTime - lastTime).count();
            lastTime = currentTime;
            float currentFps = (dt > 0) ? 1.0f / dt : 0.0f;

            SystemSettings currentSettings;
            {
                std::lock_guard<std::mutex> lock(m_dataMutex);
                currentSettings = m_sharedSettings;
            }
            auto detections = m_detector->detect(frame, currentSettings);
            
            m_tracker->update(detections, currentSettings);
            auto tracked = m_tracker->getTrackedObjects(currentSettings.trackerMaxTrailLength);

            std::lock_guard<std::mutex> lock(m_dataMutex);
            m_sharedFrame = frame.clone();
            m_sharedDetections = detections;
            m_sharedTrackedObjects = tracked;
            m_sharedCameraFps = currentFps;
            m_newDataAvailable = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void Application::handleTargetLocking(const ViewportInfo& view) {
    if (!ImGui::GetIO().WantCaptureMouse && ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        float videoX = (mousePos.x - view.pos_x) / view.scale;
        float videoY = (mousePos.y - view.pos_y) / view.scale;
        cv::Point clickPoint(static_cast<int>(videoX), static_cast<int>(videoY));
        for (const auto& det : m_detections) {
            if (det.box.contains(clickPoint)) {
                m_requestedLockTarget = det;
                m_lockRequested.store(true);
                break;
            }
        }
    }
}

void Application::run() {
    cv::Mat currentFrame;
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            if (m_newDataAvailable) {
                currentFrame = m_sharedFrame.clone();
                m_detections = m_sharedDetections;
                m_trackedObjects = m_sharedTrackedObjects;
                m_lockedTarget = m_sharedLockedTarget;
                m_cameraFps = m_sharedCameraFps;
                m_renderer->updateTexture(currentFrame);
                m_newDataAvailable = false;
            }
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        int display_w, display_h;
        glfwGetFramebufferSize(m_window, &display_w, &display_h);
        ViewportInfo view = {0};
        if (!currentFrame.empty()) {
            float frame_aspect = (float)currentFrame.cols / (float)currentFrame.rows;
            float window_aspect = (float)display_w / (float)display_h;
            if (window_aspect > frame_aspect) {
                view.target_h = (float)display_h; view.target_w = view.target_h * frame_aspect;
                view.pos_x = (display_w - view.target_w) / 2.0f; view.pos_y = 0;
                view.scale = view.target_h / (float)currentFrame.rows;
            } else {
                view.target_w = (float)display_w; view.target_h = view.target_w / frame_aspect;
                view.pos_y = (display_h - view.target_h) / 2.0f; view.pos_x = 0;
                view.scale = view.target_w / (float)currentFrame.cols;
            }
            handleTargetLocking(view);
            m_renderer->drawBackground(display_w, display_h, currentFrame);
        }
        m_hud->render(ImGui::GetBackgroundDrawList(), display_w, display_h, m_cameraFps, m_trackedObjects, view, m_settings);
        
        ImGui::Begin("Dev Console");
        bool changed = false;
        changed |= ImGui::SliderFloat("Conf", &m_settings.detectorConfThreshold, 0.1f, 1.0f);
        changed |= ImGui::SliderFloat("Score", &m_settings.detectorScoreThreshold, 0.1f, 1.0f);
        changed |= ImGui::SliderFloat("NMS", &m_settings.detectorNmsThreshold, 0.1f, 1.0f);
        changed |= ImGui::Checkbox("Trails", &m_settings.showTrails);

        if (changed) {
             std::lock_guard<std::mutex> lock(m_dataMutex);
             m_sharedSettings = m_settings;
        }
        if (ImGui::Button("Quit")) glfwSetWindowShouldClose(m_window, true);
        ImGui::End();
        ImGui::Render();
        glViewport(0, 0, display_w, display_h);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(m_window);
    }
}

void Application::cleanup() {
    m_running = false;
    if (m_workerThread.joinable()) m_workerThread.join();
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
}
