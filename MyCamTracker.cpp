// main.cpp
// Win32 UI + OpenCV tracker + auto motion init + save per second
// Build with C++17, link with OpenCV, user32, gdi32, ole32
// If using vcpkg: configure CMake with toolchain file.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <dshow.h>
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

#include <opencv2/opencv.hpp>
#if __has_include(<opencv2/tracking.hpp>)
#include <opencv2/tracking.hpp>
#define HAVE_OPENCV_TRACKING 1
#else
#define HAVE_OPENCV_TRACKING 0
#endif


namespace fs = std::filesystem;

static const wchar_t CLASS_NAME[] = L"AutoTrackWin";
static const int ID_BTN_START = 101;
static const int ID_BTN_STOP = 102;
static const int ID_CHECK_AUTO = 201;
static const int ID_CHECK_SAVE = 202;
static const int ID_TIMER_PREVIEW = 301;
static const int ID_TIMER_SAVE = 302;

HINSTANCE g_hInst = nullptr;
HWND g_hwndMain = nullptr;

std::atomic<bool> g_running{ false };
std::mutex g_frameMutex;
cv::Mat g_frame;
cv::VideoCapture g_cap;
std::string g_outDir = "captures";

std::atomic<bool> g_autoMode{ false };
std::atomic<bool> g_saveEnabled{ false };

// Tracking
cv::Ptr<cv::Tracker> g_tracker;
std::atomic<bool> g_tracking{ false };
cv::Rect2d g_bbox;

// Mouse selection
std::atomic<bool> g_selecting{ false };
POINT g_mouseStart = { 0,0 };
RECT g_previewRect = { 0,0,0,0 };
cv::Rect g_selectionRect; // integer screen coords while dragging

// Background subtractor for auto init
cv::Ptr<cv::BackgroundSubtractor> g_backSub;
double g_minContourArea = 500.0;

// Helpers
// Logging helper
static void log(const char* s) {
    CreateDirectoryW(L"C:\\Temp", NULL);
    std::ofstream f("C:\\Temp\\Track_log.txt", std::ios::app);
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        f << t.wYear << "-" << t.wMonth << "-" << t.wDay << " "
            << t.wHour << ":" << t.wMinute << ":" << t.wSecond
            << " pid=" << GetCurrentProcessId() << " : " << s << "\n";
    }
}
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

cv::Ptr<cv::Tracker> makeTracker() {
#if HAVE_OPENCV_TRACKING
    try { return cv::TrackerCSRT::create(); }
    catch (...) {}
    try { return cv::TrackerKCF::create(); }
    catch (...) {}
    return cv::Ptr<cv::Tracker>();
#else
    try { return cv::TrackerKCF::create(); }
    catch (...) { return cv::Ptr<cv::Tracker>(); }
#endif
}



HBITMAP MatToHBITMAP(const cv::Mat& mat) {
    if (mat.empty()) return nullptr;
    cv::Mat tmp;
    if (mat.channels() == 3) cv::cvtColor(mat, tmp, cv::COLOR_BGR2BGRA);
    else if (mat.channels() == 1) cv::cvtColor(mat, tmp, cv::COLOR_GRAY2BGRA);
    else tmp = mat;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tmp.cols;
    bmi.bmiHeader.biHeight = -tmp.rows;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = GetDC(NULL);
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!hbm || !bits) return nullptr;
    memcpy(bits, tmp.data, (size_t)tmp.total() * tmp.elemSize());
    return hbm;
}

void PaintPreview(HDC hdc) {
    RECT rc;
    GetClientRect(g_hwndMain, &rc);
    const int topH = 40;
    RECT previewRc = rc;
    previewRc.top += topH;
    g_previewRect = previewRc;
    FillRect(hdc, &previewRc, (HBRUSH)(COLOR_WINDOW + 1));

    cv::Mat frameCopy;
    {
        std::lock_guard<std::mutex> lk(g_frameMutex);
        if (g_frame.empty()) return;
        frameCopy = g_frame.clone();
    }

    int pw = previewRc.right - previewRc.left;
    int ph = previewRc.bottom - previewRc.top;
    if (pw <= 0 || ph <= 0) return;

    double fx = double(pw) / frameCopy.cols;
    double fy = double(ph) / frameCopy.rows;
    double f = std::min(fx, fy);
    int sw = int(frameCopy.cols * f);
    int sh = int(frameCopy.rows * f);
    cv::Mat resized;
    cv::resize(frameCopy, resized, cv::Size(sw, sh));
    HBITMAP hbm = MatToHBITMAP(resized);
    if (!hbm) return;
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP old = (HBITMAP)SelectObject(memDC, hbm);
    int x = previewRc.left + (pw - sw) / 2;
    int y = previewRc.top + (ph - sh) / 2;
    BitBlt(hdc, x, y, sw, sh, memDC, 0, 0, SRCCOPY);

    // draw tracker bbox scaled
    if (g_tracking && !g_bbox.empty()) {
        RECT r;
        r.left = x + (LONG)std::round(g_bbox.x * f);
        r.top = y + (LONG)std::round(g_bbox.y * f);
        r.right = x + (LONG)std::round((g_bbox.x + g_bbox.width) * f);
        r.bottom = y + (LONG)std::round((g_bbox.y + g_bbox.height) * f);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 255, 0));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);
    }

    // draw selection rectangle while dragging
    if (g_selecting) {
        HPEN pen = CreatePen(PS_DASH, 1, RGB(255, 0, 0));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, g_selectionRect.x, g_selectionRect.y,
            g_selectionRect.x + g_selectionRect.width,
            g_selectionRect.y + g_selectionRect.height);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);
    }

    SelectObject(memDC, old);
    DeleteObject(hbm);
    DeleteDC(memDC);
}

void StartCamera() {
    if (g_running) return;
    try { if (!fs::exists(g_outDir)) fs::create_directories(g_outDir); }
    catch (...) {}
    if (g_cap.isOpened()) g_cap.release();
    g_cap.open(0, cv::CAP_DSHOW);
    if (!g_cap.isOpened()) {
        MessageBoxW(g_hwndMain, L"Failed to open camera.", L"Error", MB_ICONERROR);
        return;
    }
    g_backSub = cv::createBackgroundSubtractorMOG2(500, 16, true);
    g_running = true;
    SetTimer(g_hwndMain, ID_TIMER_PREVIEW, 33, NULL); // ~30fps
    if (g_saveEnabled) SetTimer(g_hwndMain, ID_TIMER_SAVE, 1000, NULL);
}

void StopCamera() {
    if (!g_running) return;
    KillTimer(g_hwndMain, ID_TIMER_PREVIEW);
    KillTimer(g_hwndMain, ID_TIMER_SAVE);
    g_running = false;
    if (g_cap.isOpened()) g_cap.release();
    {
        std::lock_guard<std::mutex> lk(g_frameMutex);
        g_frame.release();
    }
    g_tracking = false;
    g_tracker.release();
    InvalidateRect(g_hwndMain, NULL, TRUE);
}

// Convert screen selection rect to image coords (Rect2d)
cv::Rect2d ScreenToImageRect(const cv::Mat& frame, RECT previewRc, RECT sel) {
    int pw = previewRc.right - previewRc.left;
    int ph = previewRc.bottom - previewRc.top;
    double fx = double(pw) / frame.cols;
    double fy = double(ph) / frame.rows;
    double f = std::min(fx, fy);
    int sw = int(frame.cols * f);
    int sh = int(frame.rows * f);
    int x = previewRc.left + (pw - sw) / 2;
    int y = previewRc.top + (ph - sh) / 2;
    int sx = std::fmax(sel.left, x);
    int sy = std::fmax(sel.top, y);
    int ex = std::fmin(sel.right, x + sw);
    int ey = std::fmin(sel.bottom, y + sh);
    if (ex <= sx || ey <= sy) return cv::Rect2d();
    double rx = double(sx - x) / f;
    double ry = double(sy - y) / f;
    double rw = double(ex - sx) / f;
    double rh = double(ey - sy) / f;
    return cv::Rect2d(rx, ry, rw, rh);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 6, 70, 26, hwnd, (HMENU)ID_BTN_START, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            90, 6, 70, 26, hwnd, (HMENU)ID_BTN_STOP, g_hInst, NULL);
        CreateWindowW(L"STATIC", L"Auto init", WS_CHILD | WS_VISIBLE,
            180, 10, 60, 18, hwnd, NULL, g_hInst, NULL);
        CreateWindowW(L"BUTTON", NULL, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            240, 8, 20, 20, hwnd, (HMENU)ID_CHECK_AUTO, g_hInst, NULL);
        CreateWindowW(L"STATIC", L"Save every second", WS_CHILD | WS_VISIBLE,
            280, 10, 120, 18, hwnd, NULL, g_hInst, NULL);
        CreateWindowW(L"BUTTON", NULL, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            400, 8, 20, 20, hwnd, (HMENU)ID_CHECK_SAVE, g_hInst, NULL);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_BTN_START) StartCamera();
        else if (id == ID_BTN_STOP) StopCamera();
        else if (id == ID_CHECK_AUTO) {
            g_autoMode = (IsDlgButtonChecked(hwnd, ID_CHECK_AUTO) == BST_CHECKED);
        }
        else if (id == ID_CHECK_SAVE) {
            g_saveEnabled = (IsDlgButtonChecked(hwnd, ID_CHECK_SAVE) == BST_CHECKED);
            if (g_running) {
                if (g_saveEnabled) SetTimer(hwnd, ID_TIMER_SAVE, 1000, NULL);
                else KillTimer(hwnd, ID_TIMER_SAVE);
            }
        }
        break;
    }
    case WM_TIMER: {
        if (wParam == ID_TIMER_PREVIEW && g_running) {
            cv::Mat frame;
            if (!g_cap.read(frame) || frame.empty()) break;
            {
                std::lock_guard<std::mutex> lk(g_frameMutex);
                g_frame = frame.clone();
            }

            // auto init with background subtraction if enabled and not tracking
            if (g_autoMode && !g_tracking) {
                cv::Mat fg;
                g_backSub->apply(frame, fg);
                cv::erode(fg, fg, cv::Mat(), cv::Point(-1, -1), 1);
                cv::dilate(fg, fg, cv::Mat(), cv::Point(-1, -1), 2);
                std::vector<std::vector<cv::Point>> contours;
                cv::findContours(fg, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
                double bestArea = 0; cv::Rect bestRect;
                for (auto& c : contours) {
                    double area = cv::contourArea(c);
                    if (area < g_minContourArea) continue;
                    cv::Rect r = cv::boundingRect(c);
                    if (area > bestArea) { bestArea = area; bestRect = r; }
                }
                if (bestArea > 0) {
                    cv::Rect2d r2d(bestRect.x, bestRect.y, bestRect.width, bestRect.height);
                    auto t = makeTracker();
                    if (!t) { /* no tracker available */ }
                    else {
                        bool ok = true;//TODO check init return
                        t->init(frame, r2d);
                        if (ok) {
                            g_tracker = t;
                            g_bbox = r2d;
                            g_tracking = true;
                        }
                        else t.release();
                    }
                }
            }

            /*/ update tracker if running
            if (g_tracking && g_tracker) {
                //cv::Rect2d newbox;
                cv::Rect newbox;
                bool ok = g_tracker->update(frame, newbox);
                if (!ok) {
                    g_tracking = false;
                    g_tracker.release();
                }
                else {
                    g_bbox = newbox;
                }
            }*/
            if (g_tracking && g_tracker) {
                // 1) Call update using integer rect (matches your tracking.hpp)
                cv::Rect bboxInt;
                bool ok = false;
                try {
                    ok = g_tracker->update(frame, bboxInt); // compiles for builds expecting cv::Rect
                }
                catch (...) {
                    ok = false;
                }

                if (!ok) {
                    // lost tracker
                    g_tracking = false;
                    g_tracker.release();
                    log("Tracker update failed -> released");
                }
                else {
                    // 2) Convert to double-precision internal bbox
                    cv::Rect2d newbbox((double)bboxInt.x, (double)bboxInt.y,
                        (double)bboxInt.width, (double)bboxInt.height);

                    // 3) Clamp to image bounds
                    newbbox.x = std::max(0.0, newbbox.x);
                    newbbox.y = std::max(0.0, newbbox.y);
                    newbbox.width = std::max(0.0, std::min(newbbox.width, double(frame.cols) - newbbox.x));
                    newbbox.height = std::max(0.0, std::min(newbbox.height, double(frame.rows) - newbbox.y));

                    // 4) Sanity checks: reject tiny or ridiculously large boxes (tunable)
                    double area = newbbox.width * newbbox.height;
                    double frameA = double(frame.cols) * double(frame.rows);
                    const double MAX_AREA_RATIO = 0.95; // if tracker covers >95% of frame, likely drift
                    const double MIN_AREA = 16.0;       // minimal area to accept

                    if (newbbox.width <= 1.0 || newbbox.height <= 1.0 ||
                        area < MIN_AREA || area > MAX_AREA_RATIO * frameA) {
                        // treat as lost
                        g_tracking = false;
                        g_tracker.release();
                        log("Tracker produced invalid bbox -> lost");
                    }
                    else {
                        // Accept new bbox
                        g_bbox = newbbox;
                        // draw with OpenCV or later convert to RECT when painting in Win32
                    }
                }
            }

            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (wParam == ID_TIMER_SAVE && g_running && g_saveEnabled) {
            // save current frame and cropped object if tracked
            cv::Mat frameCopy;
            {
                std::lock_guard<std::mutex> lk(g_frameMutex);
                if (g_frame.empty())
                {
					break;// return 0 for exit wndproc
                }
                frameCopy = g_frame.clone();
            }
            std::string base = g_outDir + "/" + timestampFilename();
            std::string fullfn = base + ".jpg";
            cv::imwrite(fullfn, frameCopy);
            if (g_tracking && !g_bbox.empty()) {
                cv::Rect ir((int)std::round(g_bbox.x), (int)std::round(g_bbox.y),
                    (int)std::round(g_bbox.width), (int)std::round(g_bbox.height));
                ir &= cv::Rect(0, 0, frameCopy.cols, frameCopy.rows);
                if (ir.width > 0 && ir.height > 0) {
                    cv::Mat crop = frameCopy(ir).clone();
                    std::string cropfn = base + "_crop.jpg";
                    cv::imwrite(cropfn, crop);
                }
            }
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (p.x >= g_previewRect.left && p.x <= g_previewRect.right &&
            p.y >= g_previewRect.top && p.y <= g_previewRect.bottom) {
            g_selecting = true;
            g_mouseStart = p;
            g_selectionRect = cv::Rect(p.x, p.y, 0, 0);
            SetCapture(hwnd);
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (g_selecting) {
            POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int x = std::min(g_mouseStart.x, p.x);
            int y = std::min(g_mouseStart.y, p.y);
            int w = std::abs(p.x - g_mouseStart.x);
            int h = std::abs(p.y - g_mouseStart.y);
            g_selectionRect = cv::Rect(x, y, w, h);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (g_selecting) {
            POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT sel = { std::min(g_mouseStart.x, p.x), std::min(g_mouseStart.y, p.y),
                         std::max(g_mouseStart.x, p.x), std::max(g_mouseStart.y, p.y) };
            ReleaseCapture();
            g_selecting = false;
            // convert to image coords and init tracker
            cv::Mat frameCopy;
            {
                std::lock_guard<std::mutex> lk(g_frameMutex);
                if (g_frame.empty()) break;
                frameCopy = g_frame.clone();
            }
            cv::Rect2d r2d = ScreenToImageRect(frameCopy, g_previewRect, sel);
            if (r2d.width > 5 && r2d.height > 5) {
                auto t = makeTracker();
                if (!t) break;
                // clamp
                r2d.x = std::max(0.0, r2d.x); r2d.y = std::max(0.0, r2d.y);
                r2d.width = std::min(r2d.width, double(frameCopy.cols) - r2d.x);
                r2d.height = std::min(r2d.height, double(frameCopy.rows) - r2d.y);
                bool ok = true;//TODO check init return
                    t->init(frameCopy, r2d);
                if (ok) {
                    g_tracker = t;
                    g_bbox = r2d;
                    g_tracking = true;
                }
                else {
                    t.release();
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintPreview(hdc);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_DESTROY:
        StopCamera();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInst = hInstance;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc)) return 0;

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Auto Tracker (Win32 + OpenCV)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        NULL, NULL, hInstance, NULL);
    if (!hwnd) return 0;
    g_hwndMain = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // main message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}