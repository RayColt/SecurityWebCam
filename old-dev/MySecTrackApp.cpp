// main.cpp
// Win32 + DirectShow enumeration (Unicode) + OpenCV capture (CAP_DSHOW) + TrackerCSRT
// Compile with C++17, link with ole32.lib and OpenCV (.\vcpkg install opencv4:x64-windows)
// Notes: Requires OpenCV contrib (tracking module) present in vcpkg opencv4 port.
//
// .\vcpkg remove opencv4:x64-windows
// .\vcpkg install opencv4[contrib]:x64-windows
//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <atomic>
#include <mutex>

#include <dshow.h>
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

#include <opencv2/opencv.hpp>
#include <opencv2/tracking.hpp>


using namespace cv;
using namespace std;
namespace fs = std::filesystem;

const int version = 0;
const wchar_t CLASS_NAME[] = L"DSWinTrackClass";
const int ID_BTN_OPEN = 201;
const int ID_BTN_CLOSE = 202;
const int ID_COMBO = 301;
const int ID_CHECK_SAVE = 302;
const int ID_BTN_RESET = 303;
const int ID_TIMER_PREVIEW = 401;
const int ID_TIMER_SAVE = 402;

HINSTANCE g_hInst = nullptr;
HWND g_hCombo = nullptr;
cv::VideoCapture g_cap;
cv::Mat g_frame;
std::atomic<bool> g_running{ false };
std::vector<std::wstring> g_devNames;
std::string g_outDir = "captures";
std::mutex g_frameMutex;

// Tracking
cv::Ptr<cv::Tracker> g_tracker;
cv::Rect2d g_bbox;
std::atomic<bool> g_tracking{ false };
std::atomic<bool> g_requestSelect{ false };
POINT g_mouseStart = { 0,0 };
RECT g_previewRect = { 0,0,0,0 };

/*/ Helpers
static void log(const char* s) {
    CreateDirectoryW(L"C:\\Temp", NULL);
    std::ofstream f("C:\\Temp\\tr_log.txt", std::ios::app);
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        f << t.wYear << "-" << t.wMonth << "-" << t.wDay << " "
            << t.wHour << ":" << t.wMinute << ":" << t.wSecond
            << " pid=" << GetCurrentProcessId() << " : " << s << "\n";
    }
}*/
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

std::vector<std::wstring> EnumerateVideoDevices() {
    std::vector<std::wstring> result;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool coInit = SUCCEEDED(hr);
    ICreateDevEnum* pDevEnum = nullptr;
    IEnumMoniker* pEnum = nullptr;

    hr = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pDevEnum));
    if (FAILED(hr) || !pDevEnum) {
        if (coInit) CoUninitialize();
        return result;
    }

    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (hr == S_OK && pEnum) {
        IMoniker* pMoniker = nullptr;
        while (pEnum->Next(1, &pMoniker, NULL) == S_OK) {
            IPropertyBag* pPropBag = nullptr;
            hr = pMoniker->BindToStorage(0, 0, IID_PPV_ARGS(&pPropBag));
            if (SUCCEEDED(hr) && pPropBag) {
                VARIANT varName;
                VariantInit(&varName);
                hr = pPropBag->Read(L"FriendlyName", &varName, 0);
                if (SUCCEEDED(hr) && varName.vt == VT_BSTR) {
                    std::wstring name((wchar_t*)varName.bstrVal);
                    result.push_back(name);
                }
                else {
                    result.push_back(L"Unknown Device");
                }
                VariantClear(&varName);
                pPropBag->Release();
            }
            else {
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

    std::memcpy(bits, tmp.data, (size_t)tmp.total() * tmp.elemSize());
    return hbm;
}

void PaintPreview(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int topH = 50;
    RECT previewRc = rc;
    previewRc.top += topH;
    g_previewRect = previewRc;

    FillRect(hdc, &previewRc, (HBRUSH)(COLOR_WINDOW + 1));

    cv::Mat frameCopy;
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
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

    // draw tracking bbox (scaled)
    if (g_tracking) {
        RECT drawRect = { x + (int)(g_bbox.x * f), y + (int)(g_bbox.y * f),
                          x + (int)((g_bbox.x + g_bbox.width) * f),
                          y + (int)((g_bbox.y + g_bbox.height) * f) };
        HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
        HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 255, 0));
        HGDIOBJ oldPen = SelectObject(hdc, hPen);
        HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
        Rectangle(hdc, drawRect.left, drawRect.top, drawRect.right, drawRect.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(hBrush);
        DeleteObject(hPen);
    }

    SelectObject(memDC, old);
    DeleteObject(hbm);
    DeleteDC(memDC);
}

void FillDeviceCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_devNames.size(); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)g_devNames[i].c_str());
    }
    if (!g_devNames.empty()) SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

// mouse ROI selection: convert mouse coords to frame coords and set g_bbox
bool PointInRect(POINT p, RECT r) {
    return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

cv::Rect2d ScreenToFrameRect(RECT previewRc, const cv::Mat& frame, RECT selRect) {
    int pw = previewRc.right - previewRc.left;
    int ph = previewRc.bottom - previewRc.top;
    double fx = double(pw) / frame.cols;
    double fy = double(ph) / frame.rows;
    double f = std::min(fx, fy);
    int sw = int(frame.cols * f);
    int sh = int(frame.rows * f);
    int x = previewRc.left + (pw - sw) / 2;
    int y = previewRc.top + (ph - sh) / 2;

    // clamp
    int sx = std::fmax(selRect.left, x);
    int sy = std::fmax(selRect.top, y);
    int ex = std::fmin(selRect.right, x + sw);
    int ey = std::fmin(selRect.bottom, y + sh);
    if (ex <= sx || ey <= sy) return cv::Rect2d();

    double rx = double(sx - x) / f;
    double ry = double(sy - y) / f;
    double rw = double(ex - sx) / f;
    double rh = double(ey - sy) / f;
    return cv::Rect2d(rx, ry, rw, rh);
}

// Window proc
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"BUTTON", L"Open", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 10, 80, 28, hwnd, (HMENU)ID_BTN_OPEN, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            100, 10, 80, 28, hwnd, (HMENU)ID_BTN_CLOSE, g_hInst, NULL);
        CreateWindowW(L"STATIC", L"Save every second", WS_CHILD | WS_VISIBLE,
            520, 14, 130, 18, hwnd, NULL, g_hInst, NULL);
        CreateWindowW(L"BUTTON", NULL, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            650, 10, 20, 20, hwnd, (HMENU)ID_CHECK_SAVE, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Reset Tracker", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            680, 10, 120, 28, hwnd, (HMENU)ID_BTN_RESET, g_hInst, NULL);

        g_hCombo = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            200, 10, 300, 200, hwnd, (HMENU)ID_COMBO, g_hInst, NULL);

        // enumerate devices and fill combo
        g_devNames = EnumerateVideoDevices();
        FillDeviceCombo(g_hCombo);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_BTN_OPEN) {
            int sel = (int)SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0);
            if (sel == CB_ERR) sel = 0;
            if (g_cap.isOpened()) g_cap.release();
            if (!g_cap.open(sel, cv::CAP_DSHOW)) {
                MessageBoxW(hwnd, L"Failed to open camera (DirectShow).", L"Error", MB_ICONERROR);
            }
            else {
                try { if (!fs::exists(g_outDir)) fs::create_directories(g_outDir); }
                catch (...) {}
                g_running = true;
                SetTimer(hwnd, ID_TIMER_PREVIEW, 33, NULL); // ~30fps preview
                if (IsDlgButtonChecked(hwnd, ID_CHECK_SAVE) == BST_CHECKED) SetTimer(hwnd, ID_TIMER_SAVE, 1000, NULL);
            }
        }
        else if (id == ID_BTN_CLOSE) {
            g_running = false;
            KillTimer(hwnd, ID_TIMER_PREVIEW);
            KillTimer(hwnd, ID_TIMER_SAVE);
            if (g_cap.isOpened()) g_cap.release();
            g_frame.release();
            g_tracking = false;
            InvalidateRect(hwnd, NULL, TRUE);
        }
        else if (id == ID_CHECK_SAVE) {
            if (g_running) {
                if (IsDlgButtonChecked(hwnd, ID_CHECK_SAVE) == BST_CHECKED) SetTimer(hwnd, ID_TIMER_SAVE, 1000, NULL);
                else KillTimer(hwnd, ID_TIMER_SAVE);
            }
        }
        else if (id == ID_BTN_RESET) {
            g_tracking = false;
            g_tracker.release();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;
    }
    case WM_TIMER: {
        if (wParam == ID_TIMER_PREVIEW && g_running) {
            cv::Mat frame;
            if (g_cap.read(frame) && !frame.empty()) {
                {
                    std::lock_guard<std::mutex> lock(g_frameMutex);
                    g_frame = frame.clone();
                }
                // update tracker
                if (g_tracking && g_tracker) {
                    cv::Rect newbox;//was Rect2d
                    bool ok = g_tracker->update(frame, newbox);
                    if (ok) {
                        g_bbox = newbox;
                    }
                    else {
                        // lost
                        g_tracking = false;
                        g_tracker.release();
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        else if (wParam == ID_TIMER_SAVE && g_running) {
            cv::Mat frame;
            if (g_cap.read(frame) && !frame.empty()) {
                std::string fname = g_outDir + "\\" + timestampFilename() + ".jpg";
                std::vector<int> p = { cv::IMWRITE_JPEG_QUALITY, 90 };
                cv::imwrite(fname, frame, p);
            }
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PointInRect(p, g_previewRect)) {
            g_requestSelect = true;
            g_mouseStart = p;
            SetCapture(hwnd);
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (g_requestSelect) {
            POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT sel = { std::min(g_mouseStart.x, p.x), std::min(g_mouseStart.y, p.y),
                         std::max(g_mouseStart.x, p.x), std::max(g_mouseStart.y, p.y) };
            cv::Mat frameCopy;
            {
                std::lock_guard<std::mutex> lock(g_frameMutex);
                if (g_frame.empty()) { g_requestSelect = false; ReleaseCapture(); break; }
                frameCopy = g_frame.clone();
            }
            cv::Rect2d rect = ScreenToFrameRect(g_previewRect, frameCopy, sel);
            if (rect.width > 5 && rect.height > 5) {
                // initialize tracker
                try {
                    g_tracker = cv::TrackerCSRT::create();
                }
                catch (...) {
                    g_tracker = cv::TrackerKCF::create(); // fallback
                }
				bool ok = true;// TODO: check return value
                    g_tracker->init(frameCopy, rect);
                if (ok) {
                    g_bbox = rect;
                    g_tracking = true;
                }
                else {
                    g_tracking = false;
                    g_tracker.release();
                }
            }


            g_requestSelect = false;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (g_requestSelect) {
            // optional: could draw rubberband; we keep simple and just refresh
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintPreview(hwnd, hdc);
        // optionally draw selection rectangle while dragging
        if (g_requestSelect) {
            POINT cur; GetCursorPos(&cur); ScreenToClient(hwnd, &cur);
            RECT sel = { std::fmin(g_mouseStart.x, cur.x), std::fmin(g_mouseStart.y, cur.y),
                         std::fmax(g_mouseStart.x, cur.x), std::fmax(g_mouseStart.y, cur.y) };
            HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
            HPEN hPen = CreatePen(PS_DASH, 1, RGB(255, 0, 0));
            HGDIOBJ oldPen = SelectObject(hdc, hPen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, sel.left, sel.top, sel.right, sel.bottom);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(hPen);
            DeleteObject(hBrush);
        }
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_DESTROY:
        g_running = false;
        KillTimer(hwnd, ID_TIMER_PREVIEW);
        KillTimer(hwnd, ID_TIMER_SAVE);
        if (g_cap.isOpened()) g_cap.release();
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

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"DS Camera Tracker (Unicode)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        NULL, NULL, hInstance, NULL);
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}