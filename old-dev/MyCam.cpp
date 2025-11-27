#include <opencv2/opencv.hpp>
#include <chrono>
#include <iostream>cd
#include <filesystem>
#include <iomanip>
#include <sstream>

using namespace std;
using namespace cv;
using namespace filesystem;
using clock = chrono::steady_clock;

string timestampFilename() 
{
    auto now = chrono::system_clock::now();
    time_t t = chrono::system_clock::to_time_t(now);
    tm tm;
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    ostringstream ss;
    ss << put_time(&tm, "%Y%m%d_%H%M%S");
    return ss.str();
}

int main(int argc, char** argv) {
    // Defaults
    int cameraIndex = 0;
    string outDir = "captures";
    int captureIntervalSeconds = 1;

    // Optional simple CLI: camera index and output dir
    if (argc >= 2) cameraIndex = stoi(argv[1]);
    if (argc >= 3) outDir = argv[2];

    try {
        if (!exists(outDir)) {
            create_directories(outDir);
            cout << "Created directory: " << outDir << '\n';
        }
    }
    catch (const exception& e) {
        cerr << "Failed to create output directory: " << e.what() << '\n';
        return 1;
    }

    VideoCapture cap(cameraIndex);
    if (!cap.isOpened()) {
        cerr << "Error: could not open camera index " << cameraIndex << '\n';
        return 1;
    }

    cout << "Press Esc in the window to stop capturing.\n";

    // Optionally set a resolution (uncomment and adjust as needed)
    // cap.set(CAP_PROP_FRAME_WIDTH, 1280);
    // cap.set(CAP_PROP_FRAME_HEIGHT, 720);

    Mat frame;
    auto nextCapture = clock::now();

    while (true) {
        // Read latest frame
        if (!cap.read(frame)) {
            cerr << "Warning: failed to read frame from camera\n";
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }

        // Show live preview
        imshow("Webcam Preview - Press Esc to quit", frame);

        // Capture at the next scheduled time
        auto now = clock::now();
        if (now >= nextCapture) {
            string fname = outDir + "/" + timestampFilename() + ".jpg";
            vector<int> params = { IMWRITE_JPEG_QUALITY, 90 };
            bool ok = imwrite(fname, frame, params);
            if (ok) {
                cout << "Saved " << fname << '\n';
            }
            else {
                cerr << "Error saving " << fname << '\n';
            }
            nextCapture = now + chrono::seconds(captureIntervalSeconds);
        }

        // WaitKey for GUI events; check for Esc (27)
        int key = waitKey(10);
        if (key == 27) break;
    }

    cap.release();
    destroyAllWindows();
    cout << "Capture stopped.\n";
    return 0;
}