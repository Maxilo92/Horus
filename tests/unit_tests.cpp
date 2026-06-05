#include "imgui.h"
#include <gtest/gtest.h>
#include "ObjectDetector.hpp"
#include "CameraModule.hpp"
#include "MultiTracker.hpp"
#include <opencv2/opencv.hpp>
#include <filesystem>

namespace fs = std::filesystem;

// --- MultiTracker Tests ---

TEST(MultiTrackerTest, InitialState) {
    MultiTracker tracker;
    EXPECT_EQ(tracker.getActiveTrackCount(), 0);
}

TEST(MultiTrackerTest, StartsTrackingOnDetection) {
    MultiTracker tracker;
    Detection d;
    d.class_id = 1;
    d.box = cv::Rect(10, 10, 100, 100);
    d.confidence = 0.9f;
    d.className = "test";

    SystemSettings settings;
    tracker.update({d}, settings);
    
    EXPECT_EQ(tracker.getActiveTrackCount(), 1);
    auto objects = tracker.getTrackedObjects(10);
    ASSERT_EQ(objects.size(), 1);
    EXPECT_EQ(objects[0].class_id, 1);
    EXPECT_FALSE(objects[0].is_confirmed); // Initially false

    // Second update with same detection should confirm it
    tracker.update({d}, settings);
    objects = tracker.getTrackedObjects(10);
    ASSERT_EQ(objects.size(), 1);
    EXPECT_TRUE(objects[0].is_confirmed); 
}

TEST(MultiTrackerTest, SuccessfulTracking) {
    MultiTracker tracker;
    SystemSettings settings;
    
    // First detection
    Detection d1;
    d1.class_id = 1; d1.box = cv::Rect(10, 10, 100, 100); d1.confidence = 0.9f; d1.className = "test";
    tracker.update({d1}, settings);
    
    auto objects1 = tracker.getTrackedObjects(10);
    ASSERT_EQ(objects1.size(), 1);
    int track_id = objects1[0].track_id;

    // Target moves
    Detection d2;
    d2.class_id = 1; d2.box = cv::Rect(15, 15, 100, 100); d2.confidence = 0.85f; d2.className = "test";
    
    tracker.update({d2}, settings);
    
    auto objects2 = tracker.getTrackedObjects(10);
    ASSERT_EQ(objects2.size(), 1);
    EXPECT_EQ(objects2[0].track_id, track_id); // Stable ID
    EXPECT_EQ(objects2[0].box.x, 15);
    EXPECT_EQ(objects2[0].lost_frames, 0);
    EXPECT_TRUE(objects2[0].is_active);
}

TEST(MultiTrackerTest, PredictionOnSignalLoss) {
    MultiTracker tracker;
    SystemSettings settings;
    
    // Initialize track with velocity
    Detection d1; d1.class_id = 1; d1.box = cv::Rect(100, 100, 50, 50); d1.className = "test";
    tracker.update({d1}, settings);
    
    Detection d2; d2.class_id = 1; d2.box = cv::Rect(110, 100, 50, 50); d2.className = "test";
    tracker.update({d2}, settings); 

    // Signal loss (empty detections)
    tracker.update({}, settings);
    
    auto objects = tracker.getTrackedObjects(10);
    ASSERT_EQ(objects.size(), 1);
    EXPECT_EQ(objects[0].lost_frames, 1);
    EXPECT_FALSE(objects[0].is_active);
    // Kalman should predict it moving further in x direction
    EXPECT_GT(objects[0].box.x, 110);
}

TEST(MultiTrackerTest, TrackRemovalAfterTimeout) {
    MultiTracker tracker;
    SystemSettings settings;
    settings.trackerMaxLostFrames = 5;
    
    Detection d; d.class_id = 1; d.box = cv::Rect(10, 10, 100, 100); d.className = "test";
    tracker.update({d}, settings);
    
    EXPECT_EQ(tracker.getActiveTrackCount(), 1);

    // Update with empty detections until it's removed
    for (int i = 0; i < 6; ++i) {
        tracker.update({}, settings);
    }
    
    EXPECT_EQ(tracker.getTrackedObjects(10).size(), 0);
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
