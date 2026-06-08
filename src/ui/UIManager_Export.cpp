#include "UIManager.hpp"
#include "UIManager_internal.hpp"

#include <GLFW/glfw3.h>   // brings in platform GL headers
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::string MakeTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    struct tm buf {};
#ifdef _WIN32
    localtime_s(&buf, &tt);
#else
    localtime_r(&tt, &buf);
#endif
    char s[64];
    std::strftime(s, sizeof(s), "%Y%m%d_%H%M%S", &buf);
    return s;
}

std::string PadId(int id) {
    std::string s = std::to_string(id);
    while (s.size() < 3) s = "0" + s;
    return s;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Export Background Worker
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::queueExportTask(ExportTask&& task) {
    {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        m_exportQueue.push_back(std::move(task));
    }
    m_exportCv.notify_one();
}

void UIManager::exportWorkerLoop() {
    while (m_exportWorkerRunning) {
        ExportTask task;
        {
            std::unique_lock<std::mutex> lock(m_exportMutex);
            m_exportCv.wait(lock, [this] {
                return !m_exportQueue.empty() || !m_exportWorkerRunning;
            });
            if (!m_exportWorkerRunning && m_exportQueue.empty()) break;
            task = std::move(m_exportQueue.front());
            m_exportQueue.pop_front();
        }

        switch (task.type) {
            case ExportTask::Type::SCREENSHOT: {
                cv::Mat shot;
                cv::flip(task.image, shot, 0);
                cv::cvtColor(shot, shot, cv::COLOR_RGB2BGR);
                
                std::string baseDir = m_settings.exportOutputDir.empty() ? "." : m_settings.exportOutputDir;
                std::filesystem::create_directories(baseDir);
                std::string path = baseDir + "/horus_screenshot_" + task.timestamp + ".png";
                
                if (cv::imwrite(path, shot))
                    m_log(LogLevel::INFO, "Screenshot saved asynchronously to " + path);
                else
                    m_log(LogLevel::ERR, "Failed to save screenshot asynchronously: " + path);
                break;
            }
            case ExportTask::Type::TARGET: {
                const auto& record = task.targetRecord;
                std::string baseDir = m_settings.exportOutputDir.empty() ? "." : m_settings.exportOutputDir;
                std::filesystem::create_directories(baseDir);

                const std::string id      = PadId(record.track_id);
                const std::string jsonPath = baseDir + "/target_" + id + "_details.json";
                const std::string imgFirst = baseDir + "/target_" + id + "_discovery.png";
                const std::string imgMid   = baseDir + "/target_" + id + "_midtrack.png";
                const std::string imgLast  = baseDir + "/target_" + id + "_lastseen.png";

                std::ofstream file(jsonPath);
                if (file.is_open()) {
                    file << "{\n";
                    file << "  \"track_id\": "       << record.track_id        << ",\n";
                    file << "  \"class_id\": "       << record.class_id        << ",\n";
                    file << "  \"className\": \""    << EscapeJsonString(record.className) << "\",\n";
                    file << "  \"max_confidence\": " << record.max_confidence   << ",\n";
                    file << "  \"first_seen\": \""   << record.first_seen_timestamp << "\",\n";
                    file << "  \"last_seen\": \""    << record.last_seen_timestamp  << "\",\n";
                    file << "  \"first_box\": {\"x\": " << record.first_box.x
                         << ", \"y\": " << record.first_box.y
                         << ", \"w\": " << record.first_box.width
                         << ", \"h\": " << record.first_box.height << "},\n";
                    file << "  \"last_box\": {\"x\": "  << record.last_box.x
                         << ", \"y\": " << record.last_box.y
                         << ", \"w\": " << record.last_box.width
                         << ", \"h\": " << record.last_box.height << "},\n";
                    file << "  \"trail\": [\n";
                    for (size_t i = 0; i < record.trail.size(); ++i) {
                        file << "    {\"x\": " << record.trail[i].x
                             << ", \"y\": " << record.trail[i].y << "}";
                        if (i + 1 < record.trail.size()) file << ",";
                        file << "\n";
                    }
                    file << "  ]\n";
                    file << "}\n";
                    file.close();
                }

                auto writeKeyframe = [&](const std::string& path, const cv::Mat& img, const char* label) {
                    if (!img.empty()) cv::imwrite(path, img);
                };
                writeKeyframe(imgFirst, record.snapshot_first.image, "discovery");
                writeKeyframe(imgMid,   record.snapshot_mid.image,   "midtrack");
                writeKeyframe(imgLast,  record.snapshot_last.image,  "lastseen");

                m_log(LogLevel::INFO, "Exported target " + id + " details asynchronously.");
                break;
            }
            case ExportTask::Type::HISTORY: {
                std::string baseDir = m_settings.exportOutputDir.empty() ? "." : m_settings.exportOutputDir;
                std::filesystem::create_directories(baseDir);
                const std::string jsonPath = baseDir + "/horus_history_" + task.timestamp + ".json";

                std::ofstream file(jsonPath);
                if (file.is_open()) {
                    file << "{\n";
                    file << "  \"export_time\": \"" << task.timestamp << "\",\n";
                    file << "  \"targets\": [\n";
                    for (size_t i = 0; i < task.history.size(); ++i) {
                        const auto& rec = task.history[i];
                        file << "    {\n";
                        file << "      \"track_id\": "       << rec.track_id        << ",\n";
                        file << "      \"class_id\": "       << rec.class_id        << ",\n";
                        file << "      \"className\": \""    << EscapeJsonString(rec.className) << "\",\n";
                        file << "      \"max_confidence\": " << rec.max_confidence   << ",\n";
                        file << "      \"first_seen\": \""   << rec.first_seen_timestamp << "\",\n";
                        file << "      \"last_seen\": \""    << rec.last_seen_timestamp  << "\",\n";
                        file << "      \"trail_len\": "      << rec.trail.size()     << "\n";
                        file << "    }";
                        if (i + 1 < task.history.size()) file << ",";
                        file << "\n";
                    }
                    file << "  ]\n";
                    file << "}\n";
                    file.close();
                    m_log(LogLevel::INFO, "Exported full history (" + std::to_string(task.history.size()) + " entries) asynchronously.");
                }
                break;
            }
            case ExportTask::Type::FEEDBACK: {
                namespace fs = std::filesystem;
                // Use the settings directory (already writable on all platforms) so
                // feedback lands in a predictable, user-accessible location rather
                // than the compile-time source tree.
#ifdef _WIN32
                const char* base = std::getenv("APPDATA");
                fs::path feedbackDir = fs::path(base ? base : ".") / "Tactileviewer" / "feedback";
#else
                const char* base = std::getenv("HOME");
                fs::path feedbackDir = fs::path(base ? base : ".") / ".tactileviewer" / "feedback";
#endif
                fs::create_directories(feedbackDir);
                fs::path outPath = feedbackDir / (std::string("feedback_") + task.timestamp + ".json");
                
                std::ofstream file(outPath);
                if (file.is_open()) {
                    file << "{\n";
                    file << "  \"timestamp\": \"" << task.timestamp << "\",\n";
                    file << "  \"category\": \""  << task.category  << "\",\n";
                    file << "  \"priority\": \""  << task.priority  << "\",\n";
                    if (!task.contact.empty())
                        file << "  \"contact\": \""   << EscapeJsonString(task.contact) << "\",\n";
                    file << "  \"feedback\": \""  << EscapeJsonString(task.text) << "\"\n";
                    file << "}\n";
                    file.close();
                    m_log(LogLevel::INFO, "Feedback saved asynchronously to " + outPath.string());
                }
                break;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// saveFeedback
// ─────────────────────────────────────────────────────────────────────────────

bool UIManager::saveFeedback(const std::string& feedback, const std::string& category, 
                             const std::string& priority, const std::string& contact) {
    ExportTask task;
    task.type = ExportTask::Type::FEEDBACK;
    task.text = feedback;
    task.category = category;
    task.priority = priority;
    task.contact = contact;
    task.timestamp = MakeTimestamp();
    queueExportTask(std::move(task));
    m_feedbackStatus = "Feedback queued for background save...";
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// exportTarget
// ─────────────────────────────────────────────────────────────────────────────

bool UIManager::exportTarget(const UniqueTargetRecord& record) {
    ExportTask task;
    task.type = ExportTask::Type::TARGET;
    task.targetRecord = record;
    task.timestamp = MakeTimestamp();
    queueExportTask(std::move(task));
    m_log(LogLevel::INFO, "Queued target export for ID " + std::to_string(record.track_id));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// exportTargetHistory
// ─────────────────────────────────────────────────────────────────────────────

bool UIManager::exportTargetHistory(const std::vector<UniqueTargetRecord>& history) {
    ExportTask task;
    task.type = ExportTask::Type::HISTORY;
    task.history = history;
    task.timestamp = MakeTimestamp();
    queueExportTask(std::move(task));
    m_log(LogLevel::INFO, "Queued full history export (" + std::to_string(history.size()) + " entries)");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// takeScreenshot
// ─────────────────────────────────────────────────────────────────────────────

void UIManager::takeScreenshot() {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    if (w <= 0 || h <= 0) return;

    // Capture MUST happen on main thread
    std::vector<uint8_t> pixels(3 * w * h);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    ExportTask task;
    task.type = ExportTask::Type::SCREENSHOT;
    task.image = cv::Mat(h, w, CV_8UC3, pixels.data()).clone(); // Deep copy for thread safety
    task.timestamp = MakeTimestamp();
    queueExportTask(std::move(task));
    m_log(LogLevel::INFO, "Screenshot captured, saving in background...");
}
