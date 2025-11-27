// SecurityWebCam.cpp
// Win32 UI + OpenCV tracker + auto motion init + save per second
// Build with >= C++17, link with OpenCV, user32, gdi32, ole32
// Win32 + DirectShow enumeration (Unicode) + OpenCV capture (CAP_DSHOW) + TrackerCSRT
// Notes: Requires OpenCV contrib (tracking module) present in vcpkg opencv4 port.
// .\vcpkg remove opencv4:x64-windows
// .\vcpkg install opencv4[contrib]:x64-windows
//

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

using namespace std;
namespace fs = filesystem;

static const wchar_t CLASS_NAME[] = L"AutoTrackWin";
enum { ID_BTN_START = 101, ID_BTN_STOP = 102, ID_CHECK_AUTO = 201, ID_CHECK_SAVE = 202, ID_TIMER_PREVIEW = 301, ID_TIMER_SAVE = 302, ID_COMBO = 303 };

HINSTANCE g_hInst = nullptr;
HWND g_hwndMain = nullptr;
HWND g_hCombo = nullptr;

vector<wstring> g_devNames;
atomic<bool> g_running{ false };
mutex g_frameMutex;
cv::Mat g_frame;
cv::VideoCapture g_cap;
string g_outDir = "captures";

atomic<bool> g_autoMode{ false };
atomic<bool> g_saveEnabled{ false };

// Tracking
cv::Ptr<cv::Tracker> g_tracker;
atomic<bool> g_tracking{ false };
cv::Rect2d g_bbox;

// Mouse selection
atomic<bool> g_selecting{ false };
POINT g_mouseStart = { 0,0 };
RECT g_previewRect = { 0,0,0,0 };
cv::Rect g_selectionRect; // integer screen coords while dragging

// Background subtractor for auto init
cv::Ptr<cv::BackgroundSubtractor> g_backSub;
double g_minContourArea = 500.0;

// Helpers
// Logging helper
static void log(const char* s) 
{
    CreateDirectoryW(L"C:\\Temp", NULL);
    ofstream f("C:\\Temp\\Track_log.txt", ios::app);
    if (f) 
    {
        SYSTEMTIME t; GetLocalTime(&t);
        f << t.wYear << "-" << t.wMonth << "-" << t.wDay << " "
            << t.wHour << ":" << t.wMinute << ":" << t.wSecond
            << " pid=" << GetCurrentProcessId() << " : " << s << "\n";
    }
}
string timestampFilename() 
{
    auto now = chrono::system_clock::now();
    time_t t = chrono::system_clock::to_time_t(now);
    tm tm;
#if defined(_MSC_VER)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    ostringstream ss;
    ss << put_time(&tm, "%Y%m%d_%H%M%S");
    return ss.str();
}

cv::Ptr<cv::Tracker> makeTracker() 
{
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

// Enumerate video capture devices via DirectShow and return friendly names (Unicode)
vector<wstring> EnumerateVideoDevices() 
{
    vector<wstring> result;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool coInit = SUCCEEDED(hr);
    ICreateDevEnum* pDevEnum = nullptr;
    IEnumMoniker* pEnum = nullptr;

    hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pDevEnum));
    if (FAILED(hr) || !pDevEnum) 
    {
        if (coInit) CoUninitialize();
        return result;
    }

    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (hr == S_OK && pEnum) 
    {
        IMoniker* pMoniker = nullptr;
        while (pEnum->Next(1, &pMoniker, NULL) == S_OK) 
        {
            IPropertyBag* pPropBag = nullptr;
            hr = pMoniker->BindToStorage(0, 0, IID_PPV_ARGS(&pPropBag));
            if (SUCCEEDED(hr) && pPropBag) 
            {
                VARIANT varName;
                VariantInit(&varName);
                // "FriendlyName" property holds the display name (BSTR, can contain Chinese)
                hr = pPropBag->Read(L"FriendlyName", &varName, 0);
                if (SUCCEEDED(hr) && varName.vt == VT_BSTR) 
                {
                    // convert BSTR (wide) to wstring
                    wstring name((wchar_t*)varName.bstrVal);
                    result.push_back(name);
                }
                else 
                {
                    result.push_back(L"Unknown Device");
                }
                VariantClear(&varName);
                pPropBag->Release();
            }
            else
            {
                result.push_back(L"Unknown Device");
            }
            pMoniker->Release();
        }
        pEnum->Release();
    }
    pDevEnum->Release();
    if (coInit) CoUninitialize();
    return result;
}

HBITMAP MatToHBITMAP(const cv::Mat& mat) 
{
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

void PaintPreview(HDC hdc) 
{
    RECT rc;
    GetClientRect(g_hwndMain, &rc);
    rc.top = rc.top + 40;
    rc.bottom = rc.bottom - 10;
    g_previewRect = rc;
    FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
    cv::Mat frameCopy;
    {
        lock_guard<mutex> lk(g_frameMutex);
        if (g_frame.empty()) return;
        frameCopy = g_frame.clone();
    }
    int pw = rc.right - rc.left;
    int ph = rc.bottom - rc.top;
    if (pw <= 0 || ph <= 0) return;
    double fx = double(pw) / frameCopy.cols;
    double fy = double(ph) / frameCopy.rows;
    double f = min(fx, fy);
    int sw = int(frameCopy.cols * f);
    int sh = int(frameCopy.rows * f);
    cv::Mat resized;
    cv::resize(frameCopy, resized, cv::Size(sw, sh));
    HBITMAP hbm = MatToHBITMAP(resized);
    if (!hbm) return;
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP old = (HBITMAP)SelectObject(memDC, hbm);
    int x = rc.left + (pw - sw) / 2;
    int y = rc.top + (ph - sh) / 2;
    BitBlt(hdc, x, y, sw, sh, memDC, 0, 0, SRCCOPY);

    // draw tracker bbox scaled
    if (g_tracking && !g_bbox.empty()) 
    {
        RECT r;
        r.left = x + (LONG)round(g_bbox.x * f);
        r.top = y + (LONG)round(g_bbox.y * f);
        r.right = x + (LONG)round((g_bbox.x + g_bbox.width) * f);
        r.bottom = y + (LONG)round((g_bbox.y + g_bbox.height) * f);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 255, 0));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);
    }

    // draw selection rectangle while dragging
    if (g_selecting) 
    {
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

void StartCamera(int sel) 
{
    if (g_running) return;
    try { if (!fs::exists(g_outDir)) fs::create_directories(g_outDir); }
    catch (...) {}
    if (g_cap.isOpened()) g_cap.release();
    g_cap.open(sel, cv::CAP_DSHOW);
    if (!g_cap.isOpened()) 
    {
        MessageBoxW(g_hwndMain, L"Failed to open camera.", L"Error", MB_ICONERROR);
        return;
    }
    g_backSub = cv::createBackgroundSubtractorMOG2(500, 16, true);
    g_running = true;
    SetTimer(g_hwndMain, ID_TIMER_PREVIEW, 33, NULL); // ~30fps
    if (g_saveEnabled) SetTimer(g_hwndMain, ID_TIMER_SAVE, 1000, NULL);
}

void StopCamera() 
{
    if (!g_running) return;
    KillTimer(g_hwndMain, ID_TIMER_PREVIEW);
    KillTimer(g_hwndMain, ID_TIMER_SAVE);
    g_running = false;
    if (g_cap.isOpened()) g_cap.release();
    {
        lock_guard<mutex> lk(g_frameMutex);
        g_frame.release();
    }
    g_tracking = false;
    g_tracker.release();
    InvalidateRect(g_hwndMain, NULL, TRUE);
}

// Convert screen selection rect to image coords (Rect2d)
cv::Rect2d ScreenToImageRect(const cv::Mat& frame, RECT previewRc, RECT sel) 
{
    int pw = previewRc.right - previewRc.left;
    int ph = previewRc.bottom - previewRc.top;
    double fx = double(pw) / frame.cols;
    double fy = double(ph) / frame.rows;
    double f = min(fx, fy);
    int sw = int(frame.cols * f);
    int sh = int(frame.rows * f);
    int x = previewRc.left + (pw - sw) / 2;
    int y = previewRc.top + (ph - sh) / 2;
    int sx = fmax(sel.left, x);
    int sy = fmax(sel.top, y);
    int ex = fmin(sel.right, x + sw);
    int ey = fmin(sel.bottom, y + sh);
    if (ex <= sx || ey <= sy) return cv::Rect2d();
    double rx = double(sx - x) / f;
    double ry = double(sy - y) / f;
    double rw = double(ex - sx) / f;
    double rh = double(ey - sy) / f;
    return cv::Rect2d(rx, ry, rw, rh);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) 
    {
        case WM_CREATE: 
        {
            HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

           HWND hStartButton = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                10, 6, 70, 26, hwnd, (HMENU)ID_BTN_START, g_hInst, NULL);
           HWND hStopButton = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                90, 6, 70, 26, hwnd, (HMENU)ID_BTN_STOP, g_hInst, NULL);
           HWND hTrackLabel = CreateWindowW(L"STATIC", L"Auto Tracker", WS_CHILD | WS_VISIBLE,
                180, 10, 120, 18, hwnd, NULL, g_hInst, NULL);
           CreateWindowW(L"BUTTON", NULL, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                260, 8, 20, 20, hwnd, (HMENU)ID_CHECK_AUTO, g_hInst, NULL);
           HWND hSaveCheck = CreateWindowW(L"STATIC", L"Save every second", WS_CHILD | WS_VISIBLE,
                320, 10, 170, 18, hwnd, NULL, g_hInst, NULL);
            CreateWindowW(L"BUTTON", NULL, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                455, 8, 20, 20, hwnd, (HMENU)ID_CHECK_SAVE, g_hInst, NULL);

           g_hCombo = CreateWindowW(L"COMBOBOX", NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                500, 10, 300, 200, hwnd, (HMENU)ID_COMBO, g_hInst, NULL);

            SendMessageW(hStartButton, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(hStopButton, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(hTrackLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(hSaveCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(g_hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);

            // enumerate devices and fill combo
            g_devNames = EnumerateVideoDevices();
            SendMessageW(g_hCombo, CB_RESETCONTENT, 0, 0);
            for (size_t i = 0; i < g_devNames.size(); ++i) 
            {
                SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM)g_devNames[i].c_str());
            }
            if (!g_devNames.empty()) SendMessageW(g_hCombo, CB_SETCURSEL, 0, 0);
            break;
        }
        case WM_COMMAND: 
        {
            int id = LOWORD(wParam);
            if (id == ID_BTN_START)
            {
                int sel = (int)SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0);
                StartCamera(sel);
            }
            else if (id == ID_BTN_STOP) StopCamera();
            else if (id == ID_CHECK_AUTO) 
            {
                g_autoMode = (IsDlgButtonChecked(hwnd, ID_CHECK_AUTO) == BST_CHECKED);
            }
            else if (id == ID_CHECK_SAVE) 
            {
                g_saveEnabled = (IsDlgButtonChecked(hwnd, ID_CHECK_SAVE) == BST_CHECKED);
                if (g_running) 
                {
                    if (g_saveEnabled) SetTimer(hwnd, ID_TIMER_SAVE, 1000, NULL);
                    else KillTimer(hwnd, ID_TIMER_SAVE);
                }
            }
            break;
        }
        case WM_TIMER: 
        {
            if (wParam == ID_TIMER_PREVIEW && g_running) 
            {
                cv::Mat frame;
                if (!g_cap.read(frame) || frame.empty()) 
                {
                    return 0;
                }
                {
                    lock_guard<mutex> lk(g_frameMutex);
                    g_frame = frame.clone();
                }

                // auto init with background subtraction if enabled and not tracking
                if (g_autoMode && !g_tracking) 
                {
                    cv::Mat fg;
                    // apply background subtractor (tune learning rate if needed)
                    g_backSub->apply(frame, fg, 0.01); // small learning rate to adapt slowly

                    // morphological cleanup: remove noise and fill holes
                    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
                    cv::morphologyEx(fg, fg, cv::MORPH_OPEN, kernel, cv::Point(-1, -1), 1);
                    cv::morphologyEx(fg, fg, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), 2);
                    cv::medianBlur(fg, fg, 5);

                    // find contours
                    vector<vector<cv::Point>> contours;
                    cv::findContours(fg, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

                    // candidate selection parameters (tune these for your scene)
                    double minArea = max(g_minContourArea, 500.0); // minimal moving area
                    double maxAreaRatio = 0.9; // ignore blobs covering almost whole frame
                    double minAspect = 1.0;    // height/width ratio lower bound for standing person
                    double maxAspect = 5.0;    // reasonable person aspect upper bound
                    double minSolidity = 0.4;  // area / convexHull area (people tend to have decent solidity)

                    cv::Rect bestRect;
                    double bestScore = 0.0;

                    // compute center preference (prefer blobs near previous track or center)
                    cv::Point2d prefCenter(frame.cols / 2.0, frame.rows / 2.0);
                    if (g_tracking && !g_bbox.empty()) prefCenter = cv::Point2d(g_bbox.x + g_bbox.width / 2.0, g_bbox.y + g_bbox.height / 2.0);

                    for (auto& c : contours) 
                    {
                        double area = cv::contourArea(c);
                        if (area < minArea) continue;

                        cv::Rect r = cv::boundingRect(c);
                        double areaRatio = area / (double)(frame.cols * frame.rows);
                        if (areaRatio > maxAreaRatio) continue;

                        double aspect = (r.height > 0) ? (double)r.height / (double)r.width : 0.0;
                        if (aspect < minAspect || aspect > maxAspect) continue;

                        // compute solidity
                        vector<cv::Point> hull;
                        cv::convexHull(c, hull);
                        double hullArea = cv::contourArea(hull);
                        double solidity = (hullArea > 1e-6) ? (area / hullArea) : 0.0;
                        if (solidity < minSolidity) continue;

                        // scoring: prefer larger area and closeness to preferred center
                        cv::Point2d cpos(r.x + r.width / 2.0, r.y + r.height / 2.0);
                        double dist = cv::norm(cpos - prefCenter);
                        // normalize dist by diag
                        double diag = sqrt(frame.cols * frame.cols + frame.rows * frame.rows);
                        double distScore = 1.0 - min(1.0, dist / diag);

                        // score = weighted combination
                        double score = 0.6 * (area / (double)(frame.cols * frame.rows)) + 0.4 * distScore;

                        // if you have HOG enabled later, you can boost score on HOG overlap
                        if (score > bestScore) { bestScore = score; bestRect = r; }
                    }

                    // optional HOG person detector check (only if you want extra validation)
                    // initialize once (outside this block ideally) to avoid cost each frame:
                    static cv::HOGDescriptor hog;
                    static bool hogInit = false;
                    if (!hogInit) 
                    {
                        hog.setSVMDetector(cv::HOGDescriptor::getDefaultPeopleDetector());
                        hogInit = true;
                    }
                    bool hogAccepted = false;
                    cv::Rect hogRect;
                    if (bestScore <= 0.0) 
                    {
                        // if no contour candidate, you can fallback to HOG-only detection
                        vector<cv::Rect> hogDet;
                        hog.detectMultiScale(frame, hogDet, 0, cv::Size(8, 8), cv::Size(32, 32), 1.05, 2);
                        if (!hogDet.empty()) 
                        {
                            // pick largest HOG detection near center
                            double bestA = 0.0;
                            for (auto& hr : hogDet) 
                            {
                                double a = hr.area();
                                if (a > bestA) { bestA = a; hogRect = hr; }
                            }
                            if (bestA > 0) { hogAccepted = true; bestRect = hogRect; bestScore = 0.5; }
                        }
                    }
                    else 
                    {
                        // if we have a contour candidate, check overlap with HOG detection to boost confidence
                        vector<cv::Rect> hogDet;
                        hog.detectMultiScale(frame, hogDet, 0, cv::Size(8, 8), cv::Size(32, 32), 1.05, 2);
                        for (auto& hr : hogDet) 
                        {
                            double iou = 0.0;
                            cv::Rect inter = bestRect & hr;
                            if (inter.area() > 0) 
                            {
                                double uni = (double)(bestRect.area() + hr.area() - inter.area());
                                iou = inter.area() / uni;
                            }
                            if (iou > 0.2) { bestScore += 0.3; break; } // boost if some overlap
                        }
                    }
                    // If we found a viable candidate, init tracker
                    if (bestScore > 0.0 && bestRect.area() > 0) 
                    {
                        cv::Rect2d r2d(bestRect.x, bestRect.y, bestRect.width, bestRect.height);
                        // clamp
                        r2d.x = max(0.0, r2d.x);
                        r2d.y = max(0.0, r2d.y);
                        r2d.width = min(r2d.width, (double)frame.cols - r2d.x);
                        r2d.height = min(r2d.height, (double)frame.rows - r2d.y);

                        auto t = makeTracker();
                        if (t) 
                        {
                            bool initOk = false;
                            try { initOk = true; t->init(frame, r2d); }
                            catch (...) { initOk = false; }
                            if (initOk) 
                            {
                                g_tracker = t;
                                g_bbox = r2d;
                                g_tracking = true;
                                log("Auto-init: tracker initialized (contour/HOG)");
                            }
                            else 
                            {
                                t.release();
                                log("Auto-init: tracker init failed");
                            }
                        }
                    } // end auto-init
                }
                // update tracker if running
                if (g_tracking && g_tracker) 
                {
                    // call update using integer rect (matches your tracking.hpp)
                    cv::Rect bboxInt;
                    bool ok = false;
                    try 
                    {
                        ok = g_tracker->update(frame, bboxInt);
                    }
                    catch (...) {
                        ok = false;
                    }
                    if (!ok) 
                    {
                        // lost tracker
                        g_tracking = false;
                        g_tracker.release();
                        log("Tracker update failed -> released");
                    }
                    else 
                    {
                        // convert to double-precision internal bbox
                        cv::Rect2d newbbox((double)bboxInt.x, (double)bboxInt.y,
                            (double)bboxInt.width, (double)bboxInt.height);

                        // clamp to image bounds
                        newbbox.x = max(0.0, newbbox.x);
                        newbbox.y = max(0.0, newbbox.y);
                        newbbox.width = max(0.0, min(newbbox.width, double(frame.cols) - newbbox.x));
                        newbbox.height = max(0.0, min(newbbox.height, double(frame.rows) - newbbox.y));

                        // sanity checks
                        double area = newbbox.width * newbbox.height;
                        double frameA = double(frame.cols) * double(frame.rows);
                        const double MAX_AREA_RATIO = 0.95;
                        const double MIN_AREA = 16.0;

                        if (newbbox.width <= 1.0 || newbbox.height <= 1.0 ||
                            area < MIN_AREA || area > MAX_AREA_RATIO * frameA) 
                        {
                            g_tracking = false;
                            g_tracker.release();
                            log("Tracker produced invalid bbox -> lost");
                        }
                        else 
                        {
                            g_bbox = newbbox;
                        }
                    }
                }

                InvalidateRect(g_hwndMain ? g_hwndMain : hwnd, NULL, FALSE);
                return 0;
            }
            else if (wParam == ID_TIMER_SAVE && g_running && g_saveEnabled) 
            {
                // save current frame and cropped object if tracked
                cv::Mat frameCopy;
                {
                    lock_guard<mutex> lk(g_frameMutex);
                    if (g_frame.empty())
                    {
                        return 0;
                    }
                    frameCopy = g_frame.clone();
                }

                string base = g_outDir + "/" + timestampFilename();
                string fullfn = base + ".jpg";
                cv::imwrite(fullfn, frameCopy);
                if (g_tracking && !g_bbox.empty()) 
                {
                    cv::Rect ir((int)round(g_bbox.x), (int)round(g_bbox.y),
                        (int)round(g_bbox.width), (int)round(g_bbox.height));
                    ir &= cv::Rect(0, 0, frameCopy.cols, frameCopy.rows);
                    if (ir.width > 0 && ir.height > 0) 
                    {
                        cv::Mat crop = frameCopy(ir).clone();
                        string cropfn = base + "_crop.jpg";
                        cv::imwrite(cropfn, crop);
                    }
                }

                InvalidateRect(g_hwndMain ? g_hwndMain : hwnd, NULL, FALSE);
                return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN: 
        {
            POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (p.x >= g_previewRect.left && p.x <= g_previewRect.right &&
                p.y >= g_previewRect.top && p.y <= g_previewRect.bottom) 
            {
                g_selecting = true;
                g_mouseStart = p;
                g_selectionRect = cv::Rect(p.x, p.y, 0, 0);
                SetCapture(hwnd);
            }
            break;
        }
        case WM_MOUSEMOVE: 
        {
            if (g_selecting) 
            {
                POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                int x = min(g_mouseStart.x, p.x);
                int y = min(g_mouseStart.y, p.y);
                int w = abs(p.x - g_mouseStart.x);
                int h = abs(p.y - g_mouseStart.y);
                g_selectionRect = cv::Rect(x, y, w, h);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        case WM_LBUTTONUP: 
        {
            if (g_selecting) 
            {
                POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                RECT sel = { min(g_mouseStart.x, p.x), min(g_mouseStart.y, p.y),
                             max(g_mouseStart.x, p.x), max(g_mouseStart.y, p.y) };
                ReleaseCapture();
                g_selecting = false;
                // convert to image coords and init tracker
                cv::Mat frameCopy;
                {
                    lock_guard<mutex> lk(g_frameMutex);
                    if (g_frame.empty()) break;
                    frameCopy = g_frame.clone();
                }
                cv::Rect2d r2d = ScreenToImageRect(frameCopy, g_previewRect, sel);
                if (r2d.width > 5 && r2d.height > 5) 
                {
                    auto t = makeTracker();
                    if (!t) break;
                    // clamp
                    r2d.x = max(0.0, r2d.x); r2d.y = max(0.0, r2d.y);
                    r2d.width = min(r2d.width, double(frameCopy.cols) - r2d.x);
                    r2d.height = min(r2d.height, double(frameCopy.rows) - r2d.y);
                    bool ok = true;//TODO check init return
                        t->init(frameCopy, r2d);
                    if (ok) 
                    {
                        g_tracker = t;
                        g_bbox = r2d;
                        g_tracking = true;
                    }
                    else 
                    {
                        t.release();
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            break;
        }
        case WM_PAINT: 
        {
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

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) 
{
    g_hInst = hInstance;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc)) return 0;

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Security WebCam with Hotspot Selection",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 666,
        NULL, NULL, hInstance, NULL);
    if (!hwnd) return 0;
    g_hwndMain = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // main message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) 
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}