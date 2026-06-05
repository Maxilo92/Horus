#ifndef DATALOGGER_HPP
#define DATALOGGER_HPP

#include "Common.hpp"
#include <string>
#include <fstream>
#include <vector>
#include <cstdint>
#include <atomic>

// -----------------------------------------------------------------------
// DataLogger: Thread-safe, atomic tracker-data logger.
// Writes one record per call to logFrame(). Supports CSV and JSON formats.
// The JSON output is a JSON-Lines file (one JSON object per line) for
// streaming compatibility and deterministic line-by-line recovery.
// -----------------------------------------------------------------------

enum class LogFormat {
    CSV  = 0,
    JSON = 1
};

class DataLogger {
public:
    DataLogger() = default;
    ~DataLogger();

    // Opens a new log file. Returns true on success.
    // basePath: directory to write into (empty = CWD).
    // A timestamped filename is auto-generated.
    bool open(const std::string& basePath, LogFormat fmt);

    // Closes and flushes the current log file.
    void close();

    // Logs one frame's worth of tracked objects.
    // timestampMs: monotonic timestamp in milliseconds since session start.
    // pixelToMeter: scale factor (0.0 if calibration not active).
    void logFrame(double timestampMs,
                  const std::vector<TrackedObject>& objects,
                  double pixelToMeter = 0.0);

    bool        isOpen()          const { return m_file.is_open(); }
    LogFormat   getFormat()       const { return m_format; }
    std::string getCurrentPath()  const { return m_currentPath; }
    uint64_t    getRowsWritten()  const { return m_rowsWritten; }
    uint64_t    getBytesWritten() const { return m_bytesWritten; }

private:
    void writeCSVHeader();
    void writeCSVRow(double ts, const TrackedObject& obj, double p2m);
    void writeJSONRow(double ts, const TrackedObject& obj, double p2m);

    std::ofstream m_file;
    LogFormat     m_format       = LogFormat::CSV;
    std::string   m_currentPath;
    uint64_t      m_rowsWritten  = 0;
    uint64_t      m_bytesWritten = 0;
    bool          m_jsonFirst    = true;  // track if we need a comma separator for JSON array
};

#endif // DATALOGGER_HPP
