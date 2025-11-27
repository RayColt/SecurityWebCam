// main.cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <vector>
#include <thread>
#include <atomic>
#include <opencv2/opencv.hpp>

namespace fs = std::filesystem;

const char CLASS_NAME[] = "WebcamWinClass";
const int ID_BTN_OPEN = 1001;
const int ID_BTN_CLOSE = 1002;
const int ID_BTN_REFRESH = 1003;
const int ID_COMBO_CAM = 1101;
const int ID_CHECK_SAVE = 1201;
const int ID_CHECK_FAST = 1202;
const int ID_TIMER_PREV = 2001; // frequent preview refresh (fast view)
const int ID_TIMER_SAVE = 2002; // 1s save timer

HINSTANCE g_hInst = nullptr;
cv::VideoCapture g_cap;
cv::Mat g_frame;
std::atomic<bool> g_running{ false };
std::string g_outDir = "captures";
HWND g_hwndMain = nullptr;
HWND g_hwndCombo = nullptr;
HWND g_hwndFast = nullptr;
int g_selectedCam = 0;

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

    // Top UI area height
    const int topH = 60;
    RECT previewRc = rc;
    previewRc.top += topH;

    if (g_frame.empty()) {
        FillRect(hdc, &previewRc, (HBRUSH)(COLOR_WINDOW + 1));
        return;
    }

    int pw = previewRc.right - previewRc.left;
    int ph = previewRc.bottom - previewRc.top;
    if (pw <= 0 || ph <= 0) return;

    // scale frame to fit preview while keeping aspect ratio
    double fx = double(pw) / g_frame.cols;
    double fy = double(ph) / g_frame.rows;
    double f = std::min(fx, fy);
    int sw = int(g_frame.cols * f);
    int sh = int(g_frame.rows * f);

    cv::Mat resized;
    cv::resize(g_frame, resized, cv::Size(sw, sh));

    HBITMAP hbm = MatToHBITMAP(resized);
    if (!hbm) return;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP old = (HBITMAP)SelectObject(memDC, hbm);

    int x = previewRc.left + (pw - sw) / 2;
    int y = previewRc.top + (ph - sh) / 2;

    BitBlt(hdc, x, y, sw, sh, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, old);
    DeleteObject(hbm);
    DeleteDC(memDC);
}

void FillCameraList(HWND combo) {
    SendMessage(combo, CB_RESETCONTENT, 0, 0);
    // probe camera indices 0..9
    for (int i = 0; i < 10; ++i) {
        cv::VideoCapture probe(i);
        if (probe.isOpened()) {
            std::ostringstream s;
            s << "Camera " << i;
            int idx = (int)SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)s.str().c_str());
            SendMessage(combo, CB_SETITEMDATA, idx, i);
            probe.release();
        }
        else {
            // optionally include closed cameras too so user can try opening
            std::ostringstream s;
            s << "Camera " << i;
            int idx = (int)SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)s.str().c_str());
            SendMessage(combo, CB_SETITEMDATA, idx, i);
        }
    }
    SendMessage(combo, CB_SETCURSEL, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // Buttons and controls
        CreateWindow(L"BUTTON", L"Open", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 10, 70, 28, hwnd, (HMENU)ID_BTN_OPEN, g_hInst, NULL);
        CreateWindow(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            90, 10, 70, 28, hwnd, (HMENU)ID_BTN_CLOSE, g_hInst, NULL);
        CreateWindow(L"BUTTON", L"Refresh cams", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            170, 10, 100, 28, hwnd, (HMENU)ID_BTN_REFRESH, g_hInst, NULL);

        g_hwndCombo = CreateWindow(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | CBS_DROPDOWNLIST,
            290, 10, 160, 200, hwnd, (HMENU)ID_COMBO_CAM, g_hInst, NULL);

        CreateWindow(L"STATIC", L"Save every second", WS_CHILD | WS_VISIBLE,
            470, 14, 120, 20, hwnd, NULL, g_hInst, NULL);
        CreateWindow(L"BUTTON", NULL, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            590, 10, 20, 20, hwnd, (HMENU)ID_CHECK_SAVE, g_hInst, NULL);

        CreateWindow(L"STATIC", L"Continuous fast view", WS_CHILD | WS_VISIBLE,
            630, 14, 140, 20, hwnd, NULL, g_hInst, NULL);
        g_hwndFast = CreateWindow(L"BUTTON", NULL, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            770, 10, 20, 20, hwnd, (HMENU)ID_CHECK_FAST, g_hInst, NULL);

        FillCameraList(g_hwndCombo);
        g_hwndMain = hwnd;
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_BTN_OPEN) {
            int sel = (int)SendMessage(g_hwndCombo, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR) {
                int camIndex = (int)SendMessage(g_hwndCombo, CB_GETITEMDATA, sel, 0);
                g_selectedCam = camIndex;
            }
            else {
                g_selectedCam = 0;
            }

            if (g_cap.isOpened()) {
                g_cap.release();
            }
            if (!g_cap.open(g_selectedCam)) {
                MessageBox(hwnd, L"Failed to open selected camera.", L"Error", MB_ICONERROR);
            }
            else {
                // ensure output dir
                try { if (!fs::exists(g_outDir)) fs::create_directories(g_outDir); }
                catch (...) {}
                g_running = true;
                // start fast preview timer (30 ms)
                BOOL fast = IsDlgButtonChecked(hwnd, ID_CHECK_FAST) == BST_CHECKED;
                int fastInterval = fast ? 30 : 80;
                SetTimer(hwnd, ID_TIMER_PREV, fastInterval, NULL);
                // start save timer only if checkbox is checked
                if (IsDlgButtonChecked(hwnd, ID_CHECK_SAVE) == BST_CHECKED) {
                    SetTimer(hwnd, ID_TIMER_SAVE, 1000, NULL);
                }
                else {
                    KillTimer(hwnd, ID_TIMER_SAVE);
                }
            }
        }
        else if (id == ID_BTN_CLOSE) {
            g_running = false;
            KillTimer(hwnd, ID_TIMER_PREV);
            KillTimer(hwnd, ID_TIMER_SAVE);
            if (g_cap.isOpened()) g_cap.release();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        else if (id == ID_BTN_REFRESH) {
            FillCameraList(g_hwndCombo);
        }
        else if (id == ID_CHECK_FAST || id == ID_CHECK_SAVE) {
            // toggle timers accordingly
            if (id == ID_CHECK_FAST) {
                if (g_running) {
                    BOOL fast = IsDlgButtonChecked(hwnd, ID_CHECK_FAST) == BST_CHECKED;
                    int fastInterval = fast ? 30 : 80;
                    SetTimer(hwnd, ID_TIMER_PREV, fastInterval, NULL);
                }
            }
            else { // ID_CHECK_SAVE
                if (g_running) {
                    BOOL save = IsDlgButtonChecked(hwnd, ID_CHECK_SAVE) == BST_CHECKED;
                    if (save) SetTimer(hwnd, ID_TIMER_SAVE, 1000, NULL);
                    else KillTimer(hwnd, ID_TIMER_SAVE);
                }
            }
        }
        break;
    }
    case WM_TIMER: {
        if (wParam == ID_TIMER_PREV && g_running) {
            cv::Mat frame;
            if (g_cap.read(frame) && !frame.empty()) {
                g_frame = frame.clone();
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        else if (wParam == ID_TIMER_SAVE && g_running) {
            cv::Mat frame;
            if (g_cap.read(frame) && !frame.empty()) {
                // save image
                std::string fname = g_outDir + "\\" + timestampFilename() + ".jpg";
                std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 90 };
                cv::imwrite(fname, frame, params);
            }
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintPreview(hwnd, hdc);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_DESTROY:
        g_running = false;
        KillTimer(hwnd, ID_TIMER_PREV);
        KillTimer(hwnd, ID_TIMER_SAVE);
        if (g_cap.isOpened()) g_cap.release();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    g_hInst = hInstance;
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"WebcamWinClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        L"Webcam Selector + Continuous View",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}