#include "Application.hpp"
#include <iostream>
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

Application::Application() 
    : m_window(nullptr), m_width(1280), m_height(720), m_title("Project Horus - Target Viewer"), 
      m_lockRequested(false), m_requestedLockId(-1), m_releaseLockRequested(false),
      m_running(false), m_newDataAvailable(false) {
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
    else m_cameraAddress = "0";

    std::cout << "[INFO] Initializing Project Horus..." << std::endl;
    std::cout << "[INFO] Camera Address: " << m_cameraAddress << std::endl;

    if (!m_camera->open(m_cameraAddress)) {
        std::cerr << "[ERROR] Camera fail: " << m_cameraAddress << std::endl;
        return false;
    }
    std::cout << "[INFO] Camera opened successfully." << std::endl;

    try {
        // macOS App Bundle path check (relative to Contents/MacOS/)
        std::string modelPath = "../Resources/yolov8n.onnx";
        std::string labelsPath = "../Resources/coco.txt";
        
        std::cout << "[INFO] Checking for models in App Bundle..." << std::endl;
        FILE* f = fopen(modelPath.c_str(), "r");
        if (!f) {
            std::cout << "[INFO] Models not found in bundle, trying assets/models..." << std::endl;
            // Fallback for development/standalone
            modelPath = "../assets/models/yolov8n.onnx";
            labelsPath = "../assets/models/coco.txt";
            f = fopen(modelPath.c_str(), "r");
        }
        
        if (!f) {
            modelPath = "assets/models/yolov8n.onnx";
            labelsPath = "assets/models/coco.txt";
            f = fopen(modelPath.c_str(), "r");
        }
        
        if (f) {
            std::cout << "[INFO] Found models at: " << modelPath << std::endl;
            fclose(f);
        } else {
            std::cerr << "[ERROR] Could not find models anywhere." << std::endl;
            return false;
        }

        m_detector = std::make_unique<ObjectDetector>(modelPath, labelsPath);
        std::cout << "[INFO] ObjectDetector initialized." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Detector fail: " << e.what() << std::endl;
        return false;
    }

    if (!initGLFW()) {
        std::cerr << "[ERROR] GLFW initialization failed." << std::endl;
        return false;
    }
    std::cout << "[INFO] GLFW initialized." << std::endl;

    if (!initImGui()) {
        std::cerr << "[ERROR] ImGui initialization failed." << std::endl;
        return false;
    }
    std::cout << "[INFO] ImGui initialized." << std::endl;

    m_running = true;
    m_workerThread = std::thread(&Application::workerLoop, this);
    std::cout << "[INFO] Worker thread started." << std::endl;
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
    float currentFps = 0.0f;
    auto lastTime = std::chrono::steady_clock::now();
    int currentLockedId = -1;

    while (m_running) {
        if (m_camera->read(frame)) {
            auto now = std::chrono::steady_clock::now();
            currentFps = 1.0f / std::chrono::duration<float>(now - lastTime).count();
            lastTime = now;

            SystemSettings currentSettings;
            {
                std::lock_guard<std::mutex> lock(m_dataMutex);
                currentSettings = m_sharedSettings;
            }
            auto detections = m_detector->detect(frame, currentSettings);
            
            m_tracker->update(detections, currentSettings);
            auto tracked = m_tracker->getTrackedObjects(currentSettings.trackerMaxTrailLength);

            // Handle lock requests
            if (m_releaseLockRequested.exchange(false)) {
                currentLockedId = -1;
            }
            if (m_lockRequested.exchange(false)) {
                currentLockedId = m_requestedLockId.load();
            }

            TrackedTarget lockedTarget;
            lockedTarget.state = TrackingState::SEARCHING;
            if (currentLockedId != -1) {
                const auto* track = m_tracker->getTrackById(currentLockedId);
                if (track) {
                    lockedTarget.track_id = track->track_id;
                    lockedTarget.class_id = track->class_id;
                    lockedTarget.className = track->className;
                    lockedTarget.box = track->getBoundingBox();
                    lockedTarget.confidence = track->confidence;
                    lockedTarget.lost_frames = track->lost_frames;
                    lockedTarget.state = (track->lost_frames == 0) ? TrackingState::LOCKED : TrackingState::LOST;
                } else {
                    currentLockedId = -1; 
                }
            }

            std::lock_guard<std::mutex> lock(m_dataMutex);
            m_sharedFrame = frame.clone();
            m_sharedDetections = detections;
            m_sharedTrackedObjects = tracked;
            m_sharedLockedTarget = lockedTarget;
            m_sharedCameraFps = currentFps;
            m_newDataAvailable = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void Application::handleTargetLocking(const ViewportInfo& view) {
    if (!ImGui::GetIO().WantCaptureMouse) {
        // Left-click to lock
        if (ImGui::IsMouseClicked(0)) {
            ImVec2 mousePos = ImGui::GetMousePos();
            float videoX = (mousePos.x - view.pos_x) / view.scale;
            float videoY = (mousePos.y - view.pos_y) / view.scale;
            cv::Point clickPoint(static_cast<int>(videoX), static_cast<int>(videoY));
            
            for (const auto& obj : m_trackedObjects) {
                if (obj.box.contains(clickPoint)) {
                    m_requestedLockId.store(obj.track_id);
                    m_lockRequested.store(true);
                    log(LogLevel::INFO, "Lock requested for ID: " + std::to_string(obj.track_id));
                    break;
                }
            }
        }
        // Right-click to release
        if (ImGui::IsMouseClicked(1)) {
            m_releaseLockRequested.store(true);
            log(LogLevel::INFO, "Lock release requested");
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

        // Enable Docking
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        int display_w, display_h;
        glfwGetFramebufferSize(m_window, &display_w, &display_h);
        
        // --- 1. Camera View Window ---
        ImGui::Begin("Camera View", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ViewportInfo view = {0};
        if (!currentFrame.empty()) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            
            float frame_aspect = (float)currentFrame.cols / (float)currentFrame.rows;
            float window_aspect = avail.x / avail.y;
            
            if (window_aspect > frame_aspect) {
                view.target_h = avail.y; 
                view.target_w = view.target_h * frame_aspect;
                view.pos_x = pos.x + (avail.x - view.target_w) / 2.0f; 
                view.pos_y = pos.y;
                view.scale = view.target_h / (float)currentFrame.rows;
            } else {
                view.target_w = avail.x; 
                view.target_h = view.target_w / frame_aspect;
                view.pos_y = pos.y + (avail.y - view.target_h) / 2.0f; 
                view.pos_x = pos.x;
                view.scale = view.target_w / (float)currentFrame.cols;
            }

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddImage((void*)(intptr_t)m_renderer->getTextureID(), 
                               ImVec2(view.pos_x, view.pos_y), 
                               ImVec2(view.pos_x + view.target_w, view.pos_y + view.target_h));
            
            handleTargetLocking(view);
            m_hud->render(drawList, (int)avail.x, (int)avail.y, m_cameraFps, m_trackedObjects, view, m_settings);
        }
        ImGui::End();

        // --- 2. Data Panel ---
        ImGui::Begin("Data Panel");
        if (ImGui::BeginTable("Tracks", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Class");
            ImGui::TableSetupColumn("Pos (X,Y)");
            ImGui::TableSetupColumn("Conf", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableHeadersRow();

            for (const auto& obj : m_trackedObjects) {
                ImGui::TableNextRow();
                
                bool isSelected = (m_lockedTarget.state != TrackingState::SEARCHING && m_lockedTarget.track_id == obj.track_id);
                
                ImGui::TableSetColumnIndex(0);
                char label[32];
                sprintf(label, "%03d", obj.track_id); // Padded ID for alignment
                if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                    if (isSelected) {
                        m_releaseLockRequested.store(true);
                    } else {
                        m_requestedLockId.store(obj.track_id);
                        m_lockRequested.store(true);
                    }
                }

                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", obj.className.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f, %.1f", (float)obj.box.x + obj.box.width/2.0f, (float)obj.box.y + obj.box.height/2.0f);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f", obj.confidence);
            }
            ImGui::EndTable();
        }
        if (m_lockedTarget.state != TrackingState::SEARCHING) {
            ImGui::Separator();
            if (ImGui::Button("Release Lock", ImVec2(-FLT_MIN, 0))) {
                m_releaseLockRequested.store(true);
            }
        }
        ImGui::End();

        // --- 3. Dev Console ---
        ImGui::Begin("Dev Console");
        bool changed = false;
        changed |= ImGui::SliderFloat("Conf", &m_settings.detectorConfThreshold, 0.1f, 1.0f);
        changed |= ImGui::SliderFloat("Score", &m_settings.detectorScoreThreshold, 0.1f, 1.0f);
        changed |= ImGui::SliderFloat("NMS", &m_settings.detectorNmsThreshold, 0.1f, 1.0f);
        changed |= ImGui::Checkbox("Trails", &m_settings.showTrails);
        changed |= ImGui::Checkbox("Crosshair", &m_settings.showCrosshair);
        changed |= ImGui::Checkbox("Overlay", &m_settings.showTacticalOverlay);

        if (changed) {
             std::lock_guard<std::mutex> lock(m_dataMutex);
             m_sharedSettings = m_settings;
        }
        ImGui::Separator();
        if (ImGui::Button("Quit", ImVec2(120, 0))) glfwSetWindowShouldClose(m_window, true);
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, display_w, display_h);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }

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
