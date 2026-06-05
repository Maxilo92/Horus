#include "imgui.h"
#include <gtest/gtest.h>
#include "ObjectDetector.hpp"
#include "CameraModule.hpp"
#include "MultiTracker.hpp"
#include "ROIManager.hpp"
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
    std::string modelPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/assets/models/yolov8n.onnx";
    std::string labelsPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/assets/models/coco.txt";
    
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
    std::string modelPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/assets/models/yolov8n.onnx";
    std::string labelsPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/assets/models/coco.txt";

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

// --- ROIManager Tests ---

TEST(ROIManagerTest, AddAndRemoveROI) {
    ROIManager rm;
    EXPECT_FALSE(rm.hasActiveROI());
    
    int id = rm.addROI(cv::Rect(10, 10, 100, 100), "Test Zone");
    EXPECT_GE(id, 0);
    EXPECT_TRUE(rm.hasActiveROI());
    
    auto rois = rm.getROIs();
    ASSERT_EQ(rois.size(), 1);
    EXPECT_EQ(rois[0].id, id);
    EXPECT_EQ(rois[0].label, "Test Zone");
    EXPECT_EQ(rois[0].function, ROIFunction::DETECTION);
    EXPECT_TRUE(rois[0].active);
    
    rm.removeROI(id);
    EXPECT_FALSE(rm.hasActiveROI());
    EXPECT_EQ(rm.getROIs().size(), 0);
}

TEST(ROIManagerTest, SetFunctionAndRect) {
    ROIManager rm;
    int id = rm.addROI(cv::Rect(10, 10, 100, 100));
    
    rm.setFunction(id, ROIFunction::EXCLUDE);
    rm.updateRect(id, cv::Rect(20, 20, 50, 50));
    rm.setLabel(id, "Excluded Zone");
    
    auto rois = rm.getROIs();
    ASSERT_EQ(rois.size(), 1);
    EXPECT_EQ(rois[0].function, ROIFunction::EXCLUDE);
    EXPECT_EQ(rois[0].rect.x, 20);
    EXPECT_EQ(rois[0].rect.width, 50);
    EXPECT_EQ(rois[0].label, "Excluded Zone");
}

TEST(ROIManagerTest, FilterDetectionsInclusion) {
    ROIManager rm;
    // Add active DETECTION zone
    int id = rm.addROI(cv::Rect(10, 10, 50, 50)); // Center x in [10, 60], y in [10, 60]
    rm.setFunction(id, ROIFunction::DETECTION);

    std::vector<Detection> detections;
    // Inside: centroid = (30, 30)
    Detection d1; d1.box = cv::Rect(20, 20, 20, 20); d1.className = "inside";
    // Outside: centroid = (100, 100)
    Detection d2; d2.box = cv::Rect(90, 90, 20, 20); d2.className = "outside";

    detections.push_back(d1);
    detections.push_back(d2);

    rm.filterDetections(detections);

    ASSERT_EQ(detections.size(), 1);
    EXPECT_EQ(detections[0].className, "inside");
}

TEST(ROIManagerTest, FilterDetectionsExclusion) {
    ROIManager rm;
    // Add active EXCLUDE zone
    int id = rm.addROI(cv::Rect(10, 10, 50, 50));
    rm.setFunction(id, ROIFunction::EXCLUDE);

    std::vector<Detection> detections;
    // Inside the exclude zone: centroid = (30, 30) - should be removed
    Detection d1; d1.box = cv::Rect(20, 20, 20, 20); d1.className = "inside";
    // Outside the exclude zone: centroid = (100, 100) - should be kept
    Detection d2; d2.box = cv::Rect(90, 90, 20, 20); d2.className = "outside";

    detections.push_back(d1);
    detections.push_back(d2);

    rm.filterDetections(detections);

    ASSERT_EQ(detections.size(), 1);
    EXPECT_EQ(detections[0].className, "outside");
}

TEST(ROIManagerTest, FilterDetectionsAlarm) {
    ROIManager rm;
    // Alarm zone acts as inclusion zone for filtering (so detections are kept)
    int id = rm.addROI(cv::Rect(10, 10, 50, 50));
    rm.setFunction(id, ROIFunction::ALARM);

    std::vector<Detection> detections;
    // Inside: centroid = (30, 30)
    Detection d1; d1.box = cv::Rect(20, 20, 20, 20); d1.className = "inside";
    // Outside: centroid = (100, 100)
    Detection d2; d2.box = cv::Rect(90, 90, 20, 20); d2.className = "outside";

    detections.push_back(d1);
    detections.push_back(d2);

    rm.filterDetections(detections);

    ASSERT_EQ(detections.size(), 1);
    EXPECT_EQ(detections[0].className, "inside");
}

TEST(CameraModuleTest, CoordinateScalingMath) {
    // Simulate raw 4K frame (3840x2160) and tracking HD frame (1280x720)
    int rawW = 3840;
    int rawH = 2160;
    int trackingW = 1280;
    int trackingH = 720;

    double scaleX = (double)rawW / (double)trackingW;
    double scaleY = (double)rawH / (double)trackingH;

    EXPECT_DOUBLE_EQ(scaleX, 3.0);
    EXPECT_DOUBLE_EQ(scaleY, 3.0);

    // Bounding box in HD space
    cv::Rect roiHD(100, 150, 50, 80);

    // Map to 4K space
    int highResX = static_cast<int>(std::round(roiHD.x * scaleX));
    int highResY = static_cast<int>(std::round(roiHD.y * scaleY));
    int highResW = static_cast<int>(std::round(roiHD.width * scaleX));
    int highResH = static_cast<int>(std::round(roiHD.height * scaleY));

    EXPECT_EQ(highResX, 300);
    EXPECT_EQ(highResY, 450);
    EXPECT_EQ(highResW, 150);
    EXPECT_EQ(highResH, 240);

    // Check bounds clamping/padding logic
    int pad_w = static_cast<int>(highResW * 0.15f); // 150 * 0.15 = 22
    int pad_h = static_cast<int>(highResH * 0.15f); // 240 * 0.15 = 36
    
    int x1 = std::max(0, highResX - pad_w);
    int y1 = std::max(0, highResY - pad_h);
    int x2 = std::min(rawW, highResX + highResW + pad_w);
    int y2 = std::min(rawH, highResY + highResH + pad_h);

    EXPECT_EQ(x1, 278);
    EXPECT_EQ(y1, 414);
    EXPECT_EQ(x2, 472);
    EXPECT_EQ(y2, 726);
}
