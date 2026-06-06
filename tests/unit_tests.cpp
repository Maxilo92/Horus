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

TEST(MultiTrackerTest, Kalman6DStateAndLagCompensation) {
    MultiTracker tracker;
    SystemSettings settings;

    // First detection to initialize target
    Detection d1;
    d1.class_id = 1;
    d1.box = cv::Rect(100, 100, 50, 50);
    d1.className = "test";
    d1.confidence = 0.9f;
    tracker.update({d1}, settings);

    // Get the object and check its size
    auto objects = tracker.getTrackedObjects(10);
    ASSERT_EQ(objects.size(), 1);
    EXPECT_EQ(objects[0].box.width, 50);
    EXPECT_EQ(objects[0].box.height, 50);

    // Frame N+1: detection at x = 110
    Detection d2;
    d2.class_id = 1;
    d2.box = cv::Rect(110, 100, 50, 50);
    d2.className = "test";
    d2.confidence = 0.9f;
    tracker.update({d2}, settings);

    // Frame N+2: detection at x = 120
    Detection d3;
    d3.class_id = 1;
    d3.box = cv::Rect(120, 100, 50, 50);
    d3.className = "test";
    d3.confidence = 0.9f;
    tracker.update({d3}, settings);

    // Now, at Frame N+5, we receive a delayed detection from Frame N+2 (lag = 3)
    // The delayed detection is at x = 120.
    // The tracker should extrapolate it by 3 * vx (~3 * 10 = 30)
    // So the final box.x should be close to 120 + 30 = 150.
    Detection d_lag;
    d_lag.class_id = 1;
    d_lag.box = cv::Rect(120, 100, 50, 50);
    d_lag.className = "test";
    d_lag.confidence = 0.9f;

    tracker.update({d_lag}, settings, 3);

    objects = tracker.getTrackedObjects(10);
    ASSERT_EQ(objects.size(), 1);
    EXPECT_NEAR(objects[0].box.x, 150, 15.0); // Allow some range for Kalman convergence
    
    // Also check that the box dimensions did NOT change/shrink
    EXPECT_NEAR(objects[0].box.width, 50, 5.0);
    EXPECT_NEAR(objects[0].box.height, 50, 5.0);
}

// --- ObjectDetector Tests ---

TEST(ObjectDetectorTest, HandleInvalidModelPath) {
    EXPECT_ANY_THROW({
        ObjectDetector detector("non_existent_model.onnx", "non_existent_labels.txt");
    });
}

TEST(ObjectDetectorTest, DetectionOnEmptyFrame) {
    std::string modelPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/assets/models/yolov8s.onnx";
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
    std::string modelPath = "/Users/maximilian/Documents/Code/Tactileviewer/Project_Horus/assets/models/yolov8s.onnx";
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

TEST(SystemSettingsTest, TargetZoomStartsNeutral) {
    SystemSettings settings;
    EXPECT_FLOAT_EQ(settings.targetZoomMagnification, 1.0f);
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

TEST(CameraModuleTest, LowLightEnhancementPipeline) {
    // Create a synthetic low-light HD frame (1280x720) with a small bright patch
    cv::Mat sample = cv::Mat::zeros(720, 1280, CV_8UC3);
    cv::circle(sample, cv::Point(200, 200), 50, cv::Scalar(80, 80, 80), -1);

    cv::Mat labBefore;
    std::vector<cv::Mat> chBefore;
    cv::cvtColor(sample, labBefore, cv::COLOR_BGR2Lab);
    cv::split(labBefore, chBefore);
    double meanLBefore = cv::mean(chBefore[0])[0];

    cv::Mat labBuf;
    std::vector<cv::Mat> channelsBuf;
    cv::Ptr<cv::CLAHE> clahePtr;

    // Should not throw and should preserve size/type
    ASSERT_NO_THROW(ImageUtils::enhanceLowLight(sample, labBuf, channelsBuf, clahePtr, 4.0f, 3));
    EXPECT_EQ(sample.cols, 1280);
    EXPECT_EQ(sample.rows, 720);
    EXPECT_EQ(sample.type(), CV_8UC3);

    cv::Mat labAfter;
    std::vector<cv::Mat> chAfter;
    cv::cvtColor(sample, labAfter, cv::COLOR_BGR2Lab);
    cv::split(labAfter, chAfter);
    double meanLAfter = cv::mean(chAfter[0])[0];

    // CLAHE should modify the lightness distribution (mean may change)
    EXPECT_NE(meanLBefore, meanLAfter);

    // Empty frame should be handled gracefully
    cv::Mat empty;
    cv::Mat emptyLab;
    std::vector<cv::Mat> emptyCh;
    cv::Ptr<cv::CLAHE> emptyClahe;
    ASSERT_NO_THROW(ImageUtils::enhanceLowLight(empty, emptyLab, emptyCh, emptyClahe, 4.0f, 3));
}
 
// --- PixelTarget Tracking Tests ---

TEST(PixelTrackingTest, SubPixelInterpolationMath) {
    // result is a CV_32FC1 matrix.
    cv::Mat result = cv::Mat::zeros(3, 3, CV_32FC1);
    
    // Set peak at (1, 1) with value 0.9f
    // Left at (1, 0) with value 0.7f
    // Right at (1, 2) with value 0.5f (peak shifted slightly to the left)
    // Top at (0, 1) with value 0.8f
    // Bottom at (2, 1) with value 0.4f (peak shifted slightly upwards)
    result.at<float>(1, 1) = 0.9f;
    result.at<float>(1, 0) = 0.7f;
    result.at<float>(1, 2) = 0.5f;
    result.at<float>(0, 1) = 0.8f;
    result.at<float>(2, 1) = 0.4f;

    double maxVal = 0.9;
    cv::Point maxLoc(1, 1);

    double dx = 0.0;
    double dy = 0.0;

    if (maxLoc.x > 0 && maxLoc.x < result.cols - 1) {
        float valL = result.at<float>(maxLoc.y, maxLoc.x - 1);
        float valR = result.at<float>(maxLoc.y, maxLoc.x + 1);
        float valC = static_cast<float>(maxVal);
        float denom = valL - 2.0f * valC + valR;
        if (std::abs(denom) > 1e-5f) {
            dx = static_cast<double>((valL - valR) / (2.0f * denom));
        }
    }
    if (maxLoc.y > 0 && maxLoc.y < result.rows - 1) {
        float valT = result.at<float>(maxLoc.y - 1, maxLoc.x);
        float valB = result.at<float>(maxLoc.y + 1, maxLoc.x);
        float valC = static_cast<float>(maxVal);
        float denom = valT - 2.0f * valC + valB;
        if (std::abs(denom) > 1e-5f) {
            dy = static_cast<double>((valT - valB) / (2.0f * denom));
        }
    }

    // Expected offsets:
    // dx = (0.7 - 0.5) / (2.0 * (0.7 - 1.8 + 0.5)) = 0.2 / -1.2 = -0.1666667
    EXPECT_NEAR(dx, -0.1666667, 1e-5);

    // dy = (0.8 - 0.4) / (2.0 * (0.8 - 1.8 + 0.4)) = 0.4 / -1.2 = -0.3333333
    EXPECT_NEAR(dy, -0.3333333, 1e-5);
}

TEST(PixelTrackingTest, SearchWindowClampingShift) {
    int cols = 1280;
    int rows = 720;
    
    // Predicted box close to left edge
    cv::Rect predictedBox(10 - 30, 100 - 30, 60, 60); // x = -20, y = 70, w = 60, h = 60
    
    int pad = 80;
    int rectW = predictedBox.width + pad * 2; // 220
    int rectH = predictedBox.height + pad * 2; // 220
    int rectX = predictedBox.x - pad; // -100
    int rectY = predictedBox.y - pad; // -10

    if (rectW > cols) rectW = cols;
    if (rectH > rows) rectH = rows;

    if (rectX < 0) rectX = 0;
    if (rectY < 0) rectY = 0;
    if (rectX + rectW > cols) rectX = cols - rectW;
    if (rectY + rectH > rows) rectY = rows - rectH;

    // Expected shifted positions:
    EXPECT_EQ(rectX, 0);
    EXPECT_EQ(rectY, 0);
    EXPECT_EQ(rectW, 220);
    EXPECT_EQ(rectH, 220);

    // Close to right edge
    cv::Rect predictedBox2(1270 - 30, 100 - 30, 60, 60); // x = 1240, y = 70, w = 60, h = 60
    rectX = predictedBox2.x - pad; // 1160
    rectY = predictedBox2.y - pad; // -10
    rectW = predictedBox2.width + pad * 2; // 220
    rectH = predictedBox2.height + pad * 2; // 220

    if (rectW > cols) rectW = cols;
    if (rectH > rows) rectH = rows;

    if (rectX < 0) rectX = 0;
    if (rectY < 0) rectY = 0;
    if (rectX + rectW > cols) rectX = cols - rectW;
    if (rectY + rectH > rows) rectY = rows - rectH;

    EXPECT_EQ(rectX, 1060);
    EXPECT_EQ(rectY, 0);
    EXPECT_EQ(rectW, 220);
    EXPECT_EQ(rectH, 220);
}

TEST(ObjectDetectorTest, ParseDetectionsJson) {
    std::string json = "{\"detections\": [{\"class_id\": 2, \"confidence\": 0.85, \"box\": [100, 200, 50, 60]}]}";
    std::vector<std::string> classes = {"person", "bicycle", "car", "motorcycle"};
    
    auto detections = ObjectDetector::parseDetectionsJson(json, classes);
    ASSERT_EQ(detections.size(), 1);
    EXPECT_EQ(detections[0].class_id, 2);
    EXPECT_EQ(detections[0].className, "car");
    EXPECT_NEAR(detections[0].confidence, 0.85f, 1e-4);
    EXPECT_EQ(detections[0].box.x, 100);
    EXPECT_EQ(detections[0].box.y, 200);
    EXPECT_EQ(detections[0].box.width, 50);
    EXPECT_EQ(detections[0].box.height, 60);
}

TEST(ObjectDetectorTest, ParseDetectionsJsonMultiple) {
    std::string json = "{\"detections\": ["
                       "{\"class_id\": 0, \"confidence\": 0.92, \"box\": [10, 20, 30, 40]},"
                       "{\"class_id\": 2, \"confidence\": 0.76, \"box\": [110, 120, 130, 140]}"
                       "]}";
    std::vector<std::string> classes = {"person", "bicycle", "car", "motorcycle"};
    
    auto detections = ObjectDetector::parseDetectionsJson(json, classes);
    ASSERT_EQ(detections.size(), 2);
    
    EXPECT_EQ(detections[0].class_id, 0);
    EXPECT_EQ(detections[0].className, "person");
    EXPECT_NEAR(detections[0].confidence, 0.92f, 1e-4);
    EXPECT_EQ(detections[0].box.x, 10);
    
    EXPECT_EQ(detections[1].class_id, 2);
    EXPECT_EQ(detections[1].className, "car");
    EXPECT_NEAR(detections[1].confidence, 0.76f, 1e-4);
    EXPECT_EQ(detections[1].box.x, 110);
}

// --- SubZooms Tests ---

TEST(SubZoomsTest, SettingsDefaultValues) {
    SystemSettings settings;
    EXPECT_TRUE(settings.subZoomsEnabled);
    EXPECT_FALSE(settings.subZoomsUseSeparateWindows);
}

TEST(SubZoomsTest, MotionTargetOverlapLogic) {
    cv::Rect targetBox(100, 100, 50, 50);
    
    cv::Rect motionBoxOverlap(110, 110, 50, 50); // overlap is 40x40 = 1600. area = 2500. ratio = 0.64
    cv::Rect interOverlap = motionBoxOverlap & targetBox;
    double ratioOverlap = (double)interOverlap.area() / motionBoxOverlap.area();
    EXPECT_GT(ratioOverlap, 0.2);
    
    cv::Rect motionBoxNoOverlap(200, 200, 50, 50);
    cv::Rect interNoOverlap = motionBoxNoOverlap & targetBox;
    double ratioNoOverlap = (interNoOverlap.area() > 0) ? (double)interNoOverlap.area() / motionBoxNoOverlap.area() : 0.0;
    EXPECT_LE(ratioNoOverlap, 0.2);
}

// --- AudioEngine Tests ---
#include "AudioEngine.hpp"

TEST(AudioEngineTest, ConfigInitAndSynthesis) {
    AudioEngine engine;
    AudioEngine::Config cfg;
    cfg.masterEnabled = true;
    cfg.masterVolume = 0.5f;

    // Should not throw or crash on initialization/synthesis
    ASSERT_NO_THROW({
        engine.init(cfg);
    });

    // Test applyConfig
    ASSERT_NO_THROW({
        engine.applyConfig(cfg);
    });

    engine.shutdown();
}

// --- TargetHistory Tests ---
TEST(TargetHistoryTest, VisualChronologySnapshottingAndFinalizing) {
    UniqueTargetRecord record;
    EXPECT_TRUE(record.cropped_image_first.empty());
    EXPECT_TRUE(record.cropped_image_mid.empty());
    EXPECT_TRUE(record.cropped_image_last.empty());

    cv::Mat mockFrame = cv::Mat::ones(100, 100, CV_8UC3);

    record.track_id = 42;
    record.cropped_image_first = mockFrame.clone();
    record.cropped_image_mid = mockFrame.clone();
    record.cropped_image_last = mockFrame.clone();
    record.cropped_image_first_version = 1;
    record.cropped_image_mid_version = 1;
    record.cropped_image_last_version = 1;

    TargetSnapshot snap1;
    snap1.image = mockFrame.clone();
    snap1.timestamp = "2026-06-06 17:00:00";
    snap1.confidence = 0.8f;
    snap1.box = cv::Rect(10, 10, 20, 20);
    record.periodic_snapshots.push_back(snap1);

    EXPECT_EQ(record.periodic_snapshots.size(), 1);

    TargetSnapshot snap2;
    cv::Mat mockFrame2 = cv::Mat::zeros(100, 100, CV_8UC3);
    snap2.image = mockFrame2.clone();
    snap2.timestamp = "2026-06-06 17:00:01";
    snap2.confidence = 0.9f;
    snap2.box = cv::Rect(15, 15, 20, 20);
    record.periodic_snapshots.push_back(snap2);

    if (!record.periodic_snapshots.empty()) {
        record.cropped_image_first = record.periodic_snapshots.front().image;
        record.cropped_image_first_version++;

        int midIdx = record.periodic_snapshots.size() / 2;
        record.cropped_image_mid = record.periodic_snapshots[midIdx].image;
        record.cropped_image_mid_version++;

        record.cropped_image_last = record.periodic_snapshots.back().image;
        record.cropped_image_last_version++;

        record.periodic_snapshots.clear();
    }

    EXPECT_EQ(record.periodic_snapshots.size(), 0);
    EXPECT_FALSE(record.cropped_image_first.empty());
    EXPECT_FALSE(record.cropped_image_mid.empty());
    EXPECT_FALSE(record.cropped_image_last.empty());
    EXPECT_EQ(record.cropped_image_first_version, 2);
    EXPECT_EQ(record.cropped_image_mid_version, 2);
    EXPECT_EQ(record.cropped_image_last_version, 2);
}



