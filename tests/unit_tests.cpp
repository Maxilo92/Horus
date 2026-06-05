#include "imgui.h"
#include <gtest/gtest.h>
#include "ObjectDetector.hpp"
#include "CameraModule.hpp"
#include "SingleTracker.hpp"
#include <opencv2/opencv.hpp>
#include <filesystem>

namespace fs = std::filesystem;

// --- SingleTracker Tests ---

TEST(SingleTrackerTest, InitialState) {
    SingleTracker tracker;
    EXPECT_FALSE(tracker.isLocked());
    EXPECT_EQ(tracker.getTarget().state, TrackingState::SEARCHING);
}

TEST(SingleTrackerTest, LockOnAndRelease) {
    SingleTracker tracker;
    Detection d;
    d.class_id = 1;
    d.box = cv::Rect(10, 10, 100, 100);
    d.confidence = 0.9f;
    d.className = "test";

    tracker.lockOn(d);
    EXPECT_TRUE(tracker.isLocked());
    EXPECT_EQ(tracker.getTarget().state, TrackingState::LOCKED);
    EXPECT_EQ(tracker.getTarget().box.x, 10);

    tracker.releaseLock();
    EXPECT_FALSE(tracker.isLocked());
}

TEST(SingleTrackerTest, SuccessfulTracking) {
    SingleTracker tracker;
    Detection d1;
    d1.class_id = 1; d1.box = cv::Rect(10, 10, 100, 100); d1.confidence = 0.9f;
    tracker.lockOn(d1);

    // Target moves
    std::vector<Detection> detections;
    Detection d2;
    d2.class_id = 1; d2.box = cv::Rect(15, 15, 100, 100); d2.confidence = 0.85f;
    detections.push_back(d2);

    SystemSettings settings;
    tracker.update(detections, settings);
    EXPECT_EQ(tracker.getTarget().state, TrackingState::LOCKED);
    EXPECT_EQ(tracker.getTarget().box.x, 15);
    EXPECT_EQ(tracker.getTarget().lost_frames, 0);
}

TEST(SingleTrackerTest, PredictionOnSignalLoss) {
    SingleTracker tracker;
    Detection d1;
    d1.class_id = 1; d1.box = cv::Rect(100, 100, 50, 50);
    tracker.lockOn(d1);

    // Give it some velocity
    std::vector<Detection> detections;
    Detection d2;
    d2.class_id = 1; d2.box = cv::Rect(110, 100, 50, 50);
    detections.push_back(d2);
    SystemSettings settings;
    tracker.update(detections, settings); 

    // Signal loss
    tracker.update({}, settings); // No detections
    EXPECT_EQ(tracker.getTarget().state, TrackingState::LOCKED);
    EXPECT_EQ(tracker.getTarget().lost_frames, 1);
    EXPECT_GT(tracker.getTarget().box.x, 110);
}

TEST(SingleTrackerTest, SignalLostAfterTimeout) {
    SingleTracker tracker;
    Detection d;
    d.class_id = 1; d.box = cv::Rect(10, 10, 100, 100);
    tracker.lockOn(d);

    SystemSettings settings;
    settings.trackerMaxLostFrames = 10;
    for (int i = 0; i < 15; ++i) {
        tracker.update({}, settings);
    }
    
    EXPECT_EQ(tracker.getTarget().state, TrackingState::LOST);
}

// --- ObjectDetector Tests ---

TEST(ObjectDetectorTest, HandleInvalidModelPath) {
    EXPECT_ANY_THROW({
        ObjectDetector detector("non_existent_model.onnx", "non_existent_labels.txt");
    });
}

TEST(ObjectDetectorTest, DetectionOnEmptyFrame) {
    std::string modelPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/models/yolov8n.onnx";
    std::string labelsPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/models/coco.txt";
    
    if (!fs::exists(modelPath)) {
        GTEST_SKIP() << "Model file not found, skipping test.";
    }

    ObjectDetector detector(modelPath, labelsPath);
    cv::Mat emptyFrame;
    SystemSettings settings;
    
    EXPECT_ANY_THROW({
        detector.detect(emptyFrame, settings);
    });
}

TEST(ObjectDetectorTest, DetectionOnSolidColorFrame) {
    std::string modelPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/models/yolov8n.onnx";
    std::string labelsPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/models/coco.txt";

    if (!fs::exists(modelPath)) {
        GTEST_SKIP() << "Model file not found, skipping test.";
    }

    ObjectDetector detector(modelPath, labelsPath);
    cv::Mat blackFrame = cv::Mat::zeros(640, 640, CV_8UC3);
    SystemSettings settings;
    
    auto detections = detector.detect(blackFrame, settings);
    EXPECT_EQ(detections.size(), 0);
}

// --- CameraModule Tests ---

TEST(CameraModuleTest, OpenInvalidAddress) {
    CameraModule camera;
    EXPECT_FALSE(camera.open("999"));
}

TEST(CameraModuleTest, IsOpenedState) {
    CameraModule camera;
    EXPECT_FALSE(camera.isOpened());
}

TEST(CameraModuleTest, ReadOnClosedCamera) {
    CameraModule camera;
    cv::Mat frame;
    EXPECT_FALSE(camera.read(frame));
}
