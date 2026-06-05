#include "DataLogger.hpp"
#include <ctime>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>

// -----------------------------------------------------------------------
// Destructor — ensure file is properly closed on teardown
// -----------------------------------------------------------------------
DataLogger::~DataLogger() {
    close();
}

// -----------------------------------------------------------------------
// open() — generates a timestamped filename and writes the file header
// -----------------------------------------------------------------------
bool DataLogger::open(const std::string& basePath, LogFormat fmt) {
    if (m_file.is_open()) close();

    m_format      = fmt;
    m_rowsWritten  = 0;
    m_bytesWritten = 0;
    m_jsonFirst    = true;

    // Build timestamped filename: horus_log_YYYYMMDD_HHMMSS.{csv|json}
    std::time_t t = std::time(nullptr);
    std::tm*    tm_info = std::localtime(&t);
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y%m%d_%H%M%S", tm_info);

    const std::string ext = (fmt == LogFormat::CSV) ? ".csv" : ".jsonl";
    std::string dir = basePath;
    if (!dir.empty() && dir.back() != '/') dir += '/';

    m_currentPath = dir + "horus_log_" + timebuf + ext;

    m_file.open(m_currentPath, std::ios::out | std::ios::trunc);
    if (!m_file.is_open()) return false;

    // Write header/preamble
    if (fmt == LogFormat::CSV) {
        writeCSVHeader();
    }
    // JSON-Lines: no global header; each line is a self-contained JSON object

    return true;
}

// -----------------------------------------------------------------------
// close() — flush and close the file stream
// -----------------------------------------------------------------------
void DataLogger::close() {
    if (!m_file.is_open()) return;
    m_file.flush();
    m_file.close();
    m_currentPath.clear();
}

// -----------------------------------------------------------------------
// logFrame() — write one frame's worth of tracked objects
// -----------------------------------------------------------------------
void DataLogger::logFrame(double timestampMs,
                          const std::vector<TrackedObject>& objects,
                          double pixelToMeter) {
    if (!m_file.is_open()) return;

    for (const auto& obj : objects) {
        // Only log active, confirmed tracks to avoid noise
        if (!obj.is_active) continue;

        if (m_format == LogFormat::CSV) {
            writeCSVRow(timestampMs, obj, pixelToMeter);
        } else {
            writeJSONRow(timestampMs, obj, pixelToMeter);
        }
    }
    m_file.flush();  // Atomic flush after each frame — deterministic persistence
}

// -----------------------------------------------------------------------
// CSV helpers
// -----------------------------------------------------------------------
void DataLogger::writeCSVHeader() {
    const std::string header =
        "timestamp_ms,track_id,class_name,confidence,"
        "x_px,y_px,w_px,h_px,"
        "cx_px,cy_px,"
        "vx_px,vy_px,"
        "x_m,y_m\n";
    m_file << header;
    m_bytesWritten += header.size();
}

void DataLogger::writeCSVRow(double ts, const TrackedObject& obj, double p2m) {
    // Compute centroid
    float cx = static_cast<float>(obj.box.x) + obj.box.width  * 0.5f;
    float cy = static_cast<float>(obj.box.y) + obj.box.height * 0.5f;

    // Velocity estimate from trail (px/frame): last two trail points
    float vx = 0.0f, vy = 0.0f;
    if (obj.trail.size() >= 2) {
        const auto& p1 = obj.trail[obj.trail.size() - 1];
        const auto& p0 = obj.trail[obj.trail.size() - 2];
        vx = static_cast<float>(p1.x - p0.x);
        vy = static_cast<float>(p1.y - p0.y);
    }

    // Real-world coordinates (only valid if calibration active)
    double x_m = (p2m > 0.0) ? cx * p2m : 0.0;
    double y_m = (p2m > 0.0) ? cy * p2m : 0.0;

    // Escape class name for CSV (replace commas)
    std::string cname = obj.className;
    for (char& c : cname) if (c == ',') c = '_';

    char buf[256];
    int n = std::snprintf(buf, sizeof(buf),
        "%.3f,%d,%s,%.4f,"
        "%d,%d,%d,%d,"
        "%.1f,%.1f,"
        "%.2f,%.2f,"
        "%.6f,%.6f\n",
        ts, obj.track_id, cname.c_str(), obj.confidence,
        obj.box.x, obj.box.y, obj.box.width, obj.box.height,
        cx, cy,
        vx, vy,
        x_m, y_m);

    if (n > 0) {
        m_file.write(buf, n);
        m_bytesWritten += static_cast<uint64_t>(n);
        m_rowsWritten++;
    }
}

// -----------------------------------------------------------------------
// JSON-Lines helpers
// Each line: one JSON object with all fields for a single tracked object
// -----------------------------------------------------------------------
void DataLogger::writeJSONRow(double ts, const TrackedObject& obj, double p2m) {
    float cx = static_cast<float>(obj.box.x) + obj.box.width  * 0.5f;
    float cy = static_cast<float>(obj.box.y) + obj.box.height * 0.5f;

    float vx = 0.0f, vy = 0.0f;
    if (obj.trail.size() >= 2) {
        const auto& p1 = obj.trail[obj.trail.size() - 1];
        const auto& p0 = obj.trail[obj.trail.size() - 2];
        vx = static_cast<float>(p1.x - p0.x);
        vy = static_cast<float>(p1.y - p0.y);
    }

    double x_m = (p2m > 0.0) ? cx * p2m : 0.0;
    double y_m = (p2m > 0.0) ? cy * p2m : 0.0;

    // Escape class name for JSON string (handle backslash and quotes)
    std::string cname = obj.className;
    std::string escaped;
    escaped.reserve(cname.size());
    for (char c : cname) {
        if (c == '"')  { escaped += "\\\""; }
        else if (c == '\\') { escaped += "\\\\"; }
        else { escaped += c; }
    }

    char buf[512];
    int n = std::snprintf(buf, sizeof(buf),
        "{\"ts\":%.3f,\"id\":%d,\"cls\":\"%s\",\"conf\":%.4f,"
        "\"box\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
        "\"cx\":%.1f,\"cy\":%.1f,"
        "\"vx\":%.2f,\"vy\":%.2f,"
        "\"x_m\":%.6f,\"y_m\":%.6f}\n",
        ts, obj.track_id, escaped.c_str(), obj.confidence,
        obj.box.x, obj.box.y, obj.box.width, obj.box.height,
        cx, cy,
        vx, vy,
        x_m, y_m);

    if (n > 0) {
        m_file.write(buf, n);
        m_bytesWritten += static_cast<uint64_t>(n);
        m_rowsWritten++;
    }
}
