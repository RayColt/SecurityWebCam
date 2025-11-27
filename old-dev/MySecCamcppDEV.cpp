// auto_track_and_save.cpp
// Compile: g++ -std=c++17 auto_track_and_save.cpp `pkg-config --cflags --libs opencv4`
// or use your Visual Studio/CMake + vcpkg OpenCV (with contrib for CSRT).

#define WIN32_LEAN_AND_MEAN
#include <opencv2/opencv.hpp>
#include <opencv2/tracking.hpp>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;
using clock = std::chrono::steady_clock;

std::mutex frameMtx;
cv::Mat latestFrame;
bool selecting = false;
cv::Rect selectionRect;
cv::Rect2d selectionForTracker;
bool haveSelection = false;

// mouse callback for manual ROI selection
void onMouse(int event, int x, int y, int flags, void* userdata) {
    if (event == cv::EVENT_LBUTTONDOWN) {
        selecting = true;
        selectionRect = cv::Rect(x, y, 0, 0);
    }
    else if (event == cv::EVENT_MOUSEMOVE && selecting) {
        selectionRect.width = x - selectionRect.x;
        selectionRect.height = y - selectionRect.y;
    }
    else if (event == cv::EVENT_LBUTTONUP && selecting) {
        selecting = false;
        if (selectionRect.width < 0) { selectionRect.x += selectionRect.width; selectionRect.width = -selectionRect.width; }
        if (selectionRect.height < 0) { selectionRect.y += selectionRect.height; selectionRect.height = -selectionRect.height; }
        if (selectionRect.width > 10 && selectionRect.height > 10) {
            selectionForTracker = cv::Rect2d(selectionRect.x, selectionRect.y, selectionRect.width, selectionRect.height);
            haveSelection = true;
        }
    }
}

// helper: timestamp filename
std::string timestampFilename() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#if defined(_MSC_VER)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return ss.str();
}

int main(int argc, char** argv) {
    int camIndex = 0;
    if (argc >= 2) camIndex = std::atoi(argv[1]);
    std::string outDir = "captures";
    fs::create_directories(outDir);

    cv::VideoCapture cap(camIndex, cv::CAP_DSHOW);
    if (!cap.isOpened()) {
        std::cerr << "Cannot open camera " << camIndex << "\n";
        return 1;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);

    cv::namedWindow("preview", cv::WINDOW_AUTOSIZE);
    cv::setMouseCallback("preview", onMouse, nullptr);

    // motion detector
    cv::Ptr<cv::BackgroundSubtractor> backSub = cv::createBackgroundSubtractorMOG2(500, 16, true);
    double minContourArea = 500.0;

    // tracker (create on demand)
    cv::Ptr<cv::Tracker> tracker;
#ifdef CV_VERSION_EPOCH
    // older OpenCV handling (rare)
#endif

    auto makeTracker = [&]() -> cv::Ptr<cv::Tracker> {
#ifdef CV__VERSION
        (void)0;
#endif
#ifdef CV_TRACKER_CSRT_AVAILABLE
        try { return cv::TrackerCSRT::create(); }
        catch (...) {}
#endif
        try { return cv::TrackerKCF::create(); }
        catch (...) {}
        return cv::Ptr<cv::Tracker>();
        };

    bool tracking = false;
    clock::time_point lastSave = clock::now();

    std::cout << "Instructions:\n - Drag with mouse on preview to select object and start tracking\n - Or press 'a' to enable automatic motion-based detection and tracking\n - Press 's' to toggle saving every second (when tracking)\n - Press 'q' to quit\n";

    bool autoMode = false;
    bool saveEnabled = true;

    while (true) {
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            std::cerr << "Camera read failed\n";
            break;
        }
        {
            std::lock_guard<std::mutex> lk(frameMtx);
            latestFrame = frame.clone();
        }

        // If user selected ROI with mouse, initialize tracker
        if (haveSelection) {
            if (!tracker) tracker = makeTracker();
            if (!tracker) { std::cerr << "No tracker available\n"; haveSelection = false; }
            else {
                // clamp selection to image bounds
                selectionForTracker.x = std::max(0.0, selectionForTracker.x);
                selectionForTracker.y = std::max(0.0, selectionForTracker.y);
                selectionForTracker.width = std::min(selectionForTracker.width, (double)frame.cols - selectionForTracker.x);
                selectionForTracker.height = std::min(selectionForTracker.height, (double)frame.rows - selectionForTracker.y);
                if (selectionForTracker.width > 5 && selectionForTracker.height > 5 && !frame.empty()) {
                    bool ok = true;
                    tracker->init(frame, selectionForTracker);
                    if (ok) {
                        tracking = true;
                        lastSave = clock::now();
                        std::cout << "Tracker initialized (manual)\n";
                    }
                    else {
                        tracker.release();
                        tracking = false;
                        std::cerr << "Tracker init failed\n";
                    }
                }
                haveSelection = false;
            }
        }

        // Automatic motion-based initialization
        if (autoMode && !tracking) {
            cv::Mat fg;
            backSub->apply(frame, fg);
            cv::erode(fg, fg, cv::Mat(), cv::Point(-1, -1), 1);
            cv::dilate(fg, fg, cv::Mat(), cv::Point(-1, -1), 2);
            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(fg, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            double bestArea = 0;
            cv::Rect bestRect;
            for (auto& c : contours) {
                double area = cv::contourArea(c);
                if (area < minContourArea) continue;
                cv::Rect r = cv::boundingRect(c);
                if (area > bestArea) { bestArea = area; bestRect = r; }
            }
            if (bestArea > 0) {
                if (!tracker) tracker = makeTracker();
                if (tracker) {
                    cv::Rect2d r2d(bestRect.x, bestRect.y, bestRect.width, bestRect.height);
                    bool ok = true;
                    tracker->init(frame, r2d);
                    if (ok) {
                        tracking = true;
                        lastSave = clock::now();
                        std::cout << "Tracker initialized (auto) rect=" << bestRect << "\n";
                    }
                    else {
                        tracker.release();
                    }
                }
            }
        }

        // If tracking, update tracker and draw bbox
        cv::Rect2d bbox;
        if (tracking && tracker) {
            bool ok = tracker->update(frame, bbox);
            if (!ok) {
                // lost tracker
                tracking = false;
                tracker.release();
                std::cout << "Tracking lost\n";
            }
            else {
                // draw bbox
                cv::rectangle(frame, bbox, cv::Scalar(0, 255, 0), 2);
                // optionally compute motion magnitude and decide to save
                auto now = clock::now();
                if (saveEnabled && std::chrono::duration_cast<std::chrono::seconds>(now - lastSave).count() >= 1) {
                    // save full frame and cropped tracked object
                    std::string base = outDir + "/" + timestampFilename();
                    std::string fullfn = base + ".jpg";
                    cv::imwrite(fullfn, frame);
                    // crop safely
                    cv::Rect ir((int)std::round(bbox.x), (int)std::round(bbox.y),
                        (int)std::round(bbox.width), (int)std::round(bbox.height));
                    ir &= cv::Rect(0, 0, frame.cols, frame.rows);
                    if (ir.width > 0 && ir.height > 0) {
                        cv::Mat crop = frame(ir).clone();
                        std::string cropfn = base + "_crop.jpg";
                        cv::imwrite(cropfn, crop);
                    }
                    lastSave = now;
                    std::cout << "Saved: " << fullfn << "\n";
                }
            }
        }

        // Draw selection rectangle while dragging
        if (selecting) {
            cv::rectangle(frame, selectionRect, cv::Scalar(255, 0, 0), 2);
        }

        // show help overlay
        cv::putText(frame, "Drag mouse to select object, 'a' = auto detect, 's' toggle save, 'q' quit", cv::Point(10, 20),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1);

        cv::imshow("preview", frame);
        int k = cv::waitKey(1) & 0xFF;
        if (k == 'q') break;
        if (k == 'a') { autoMode = !autoMode; std::cout << "Auto mode " << (autoMode ? "ON" : "OFF") << "\n"; }
        if (k == 's') { saveEnabled = !saveEnabled; std::cout << "Save " << (saveEnabled ? "ON" : "OFF") << "\n"; }
        if (k == 'c') { // clear tracker
            tracking = false; tracker.release(); std::cout << "Tracker cleared\n";
        }
    }

    cap.release();
    cv::destroyAllWindows();
    return 0;
}