#pragma once

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <functional>
#include "Common.hpp"
#include "Blackboard.hpp"
#include "DossierDatabase.hpp"

struct AIRequest {
    std::string entityUuid;
    std::string type;
    cv::Mat crop;
    std::string currentDossier;
    bool isUpdate = false;
};

class AIAgent {
public:
    AIAgent(Blackboard& blackboard, DossierDatabase& db, LogFn logFn);
    ~AIAgent();

    void start();
    void stop();

    /**
     * @brief Queue a request for AI dossier generation.
     * Respects stability and rate limits.
     */
    void queueRequest(const AIRequest& request);

private:
    Blackboard& m_blackboard;
    DossierDatabase& m_db;
    LogFn m_log;

    std::atomic<bool> m_running{false};
    std::thread m_workerThread;
    
    std::queue<AIRequest> m_queue;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;

    // Rate Limiting
    std::mutex m_limitMutex;
    std::vector<std::chrono::steady_clock::time_point> m_requestHistory;

    void workerLoop();
    void processRequest(const AIRequest& req);
    bool checkRateLimit();

    static std::string base64Encode(const std::vector<uchar>& data);
};
